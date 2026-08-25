#include "voca.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include <portaudio.h>
#include <whisper.h>

namespace {

// --- Audio capture / phrase segmentation tuning -----------------------
// A simple energy gate turns the continuous mic stream into discrete
// phrases before handing each one to whisper: kPauseThresholdSec is how
// much silence ends a phrase, kMinEnergyThreshold/kEnergyMultiplier decide
// what counts as speech vs. ambient noise. whisper.cpp has its own VAD, but
// it needs a second model file just for that - a plain energy gate avoids
// the extra download/dependency for what's otherwise a simple threshold.
constexpr int kSampleRate = 16000;
constexpr int kFrameSamples = 480; // 30ms blocks at 16kHz
constexpr double kPauseThresholdSec = 1.2;
constexpr double kMinPhraseSec = 0.3;
constexpr double kMaxPhraseSec = 30.0;
constexpr double kAmbientAdaptRate = 0.05; // exponential moving average rate
constexpr double kEnergyMultiplier = 1.8;  // speech threshold = ambient * this
constexpr double kMinEnergyThreshold = 100.0;

constexpr double kAutoSleepTimeoutSec = 300.0; // 5 minutes, matches AUTO_SLEEP_TIMEOUT

// Extra time beyond a beep's own duration to keep ignoring mic input for -
// covers speaker-to-mic latency and any room echo tail.
constexpr double kBeepMuteGuardSec = 0.3;

const std::vector<std::string> kInterruptPhrases = {
    "stop talking", "stop speaking", "stop now",
};
const std::vector<std::string> kSleepControlWords = {"sleep", "stop", "rest"};

// Unlike the old "voca" (a made-up word whisper had to guess a phonetic
// neighbor for), "olli"/"ollie" are real words/names whisper already knows,
// so it's less prone to wild mis-transcription - but the same "hey
// <prefix>" loose net is kept as a safety margin for however it mishears
// the vowel, e.g. a softer "o". "ol" right after "hey" is about as rare in
// ordinary conversation as "vo" was ("hey old", "hey olive" are about the
// extent of it) - safe for the same reason "hey vo-<word>" was. Without
// that "hey" context, only the exact bare fallbacks "olli"/"ollie" are
// accepted - a bare prefix match would also catch ordinary words like "old"
// or "olive". Not yet tuned against real mis-hearings the way the previous
// wake word was (its own list, before this one, was built from actual
// observed transcriptions over time - see git history) - adjust
// kOlPrefixMinLen/MaxLen or add more bare fallbacks below if real use turns
// up other variants worth catching.
constexpr size_t kOlPrefixMinLen = 3;
constexpr size_t kOlPrefixMaxLen = 8;

double monotonicSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// A lowercased word plus its [start, end) byte range in the original
// (un-lowercased) text, so a match can be mapped back to a position in the
// real transcript - see findPhraseWords().
struct Word {
    std::string text;
    size_t start;
    size_t end;
};

// Splits on anything that isn't alphanumeric, so whisper's punctuation
// ("Hey, Volca," / "Sleep Volca.") doesn't break phrase matching the way a
// plain substring search would.
std::vector<Word> tokenize(const std::string& text) {
    std::vector<Word> words;
    size_t i = 0;
    size_t n = text.size();
    while (i < n) {
        while (i < n && std::isalnum(static_cast<unsigned char>(text[i])) == 0) ++i;
        size_t start = i;
        while (i < n && std::isalnum(static_cast<unsigned char>(text[i])) != 0) ++i;
        if (i > start) {
            std::string word = text.substr(start, i - start);
            std::transform(word.begin(), word.end(), word.begin(),
                            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            words.push_back({std::move(word), start, i});
        }
    }
    return words;
}

// Finds the first phrase from `phrases` whose words appear as a contiguous
// run in `words` (already tokenized from the transcript being matched
// against). On a match, `matchEndPos` is set to the original-text byte
// offset right after the last matched word.
std::string findPhraseWords(const std::vector<Word>& words, const std::vector<std::string>& phrases,
                             size_t& matchEndPos) {
    for (const auto& phrase : phrases) {
        std::vector<Word> phraseWords = tokenize(phrase);
        size_t plen = phraseWords.size();
        if (plen == 0 || plen > words.size()) continue;
        for (size_t i = 0; i + plen <= words.size(); ++i) {
            bool matched = true;
            for (size_t j = 0; j < plen; ++j) {
                if (words[i + j].text != phraseWords[j].text) {
                    matched = false;
                    break;
                }
            }
            if (matched) {
                matchEndPos = words[i + plen - 1].end;
                return phrase;
            }
        }
    }
    return {};
}

bool startsWithOl(const std::string& word) {
    if (word.size() < kOlPrefixMinLen || word.size() > kOlPrefixMaxLen) return false;
    return word[0] == 'o' && word[1] == 'l';
}

bool containsWord(const std::vector<Word>& words, const std::string& target) {
    for (const auto& w : words) {
        if (w.text == target) return true;
    }
    return false;
}

// Finds a wake-word occurrence: an "ol"-prefixed word immediately following
// "hey" (loose - see the comment on kOlPrefixMinLen above for why that's
// safe), or a bare "olli"/"ollie" anywhere with no "hey" needed (tight, to
// avoid catching ordinary words - see kOlPrefixMinLen). On a match,
// `matchEndPos` is set to the original-text byte offset right after the
// matched word.
std::string findWakeWord(const std::vector<Word>& words, size_t& matchEndPos) {
    for (size_t i = 0; i + 1 < words.size(); ++i) {
        if (words[i].text == "hey" && startsWithOl(words[i + 1].text)) {
            matchEndPos = words[i + 1].end;
            return words[i + 1].text;
        }
    }
    for (const auto& w : words) {
        if (w.text == "olli" || w.text == "ollie") {
            matchEndPos = w.end;
            return w.text;
        }
    }
    return {};
}

// Sleep trigger: "stop listening", or a control word ("sleep"/"stop"/
// "rest") immediately next to an "ol"-prefixed word in either order
// ("sleep olli", "olli stop", ...).
bool findSleepTrigger(const std::vector<Word>& words) {
    if (containsWord(words, "stop") && containsWord(words, "listening")) return true;
    for (size_t i = 0; i < words.size(); ++i) {
        bool isControlWord = std::find(kSleepControlWords.begin(), kSleepControlWords.end(), words[i].text) !=
                              kSleepControlWords.end();
        if (!isControlWord) continue;
        // "stop talking"/"stop speaking"/"stop now" is the interrupt idiom
        // (see kInterruptPhrases), not the standalone "<olli> stop" sleep
        // synonym - don't let coincidental word order ("hey olli, stop
        // talking") also read as a sleep command.
        if (words[i].text == "stop" && i + 1 < words.size()) {
            const std::string& next = words[i + 1].text;
            if (next == "talking" || next == "speaking" || next == "now") continue;
        }
        if (i > 0 && startsWithOl(words[i - 1].text)) return true;
        if (i + 1 < words.size() && startsWithOl(words[i + 1].text)) return true;
    }
    return false;
}

// whisper emits bracketed non-speech tags ("[BLANK_AUDIO]", "[MUSIC]",
// "(wind blowing)", ...) when a segment has no real words - e.g. a noise
// blip that crossed the energy gate but wasn't actual speech. Recognized
// generically (by the surrounding brackets) rather than as a fixed list,
// since whisper doesn't document the exact set of tags it can produce.
bool isNonSpeechMarker(const std::string& text) {
    if (text.size() < 2) return false;
    char open = text.front();
    char close = text.back();
    return (open == '[' && close == ']') || (open == '(' && close == ')');
}

double rms(const int16_t* samples, size_t count) {
    if (count == 0) return 0.0;
    double sumSquares = 0.0;
    for (size_t i = 0; i < count; ++i) {
        double s = samples[i];
        sumSquares += s * s;
    }
    return std::sqrt(sumSquares / static_cast<double>(count));
}

// --- Beep feedback (wake/sleep audio cue) --------------------------------
// Same approach as lira_cpp's TextToSpeech::playback: shell out to the
// system's aplay rather than link an audio-encoding dependency.
void playBeepBlocking(double frequencyHz, double durationSec, double volume) {
    size_t nSamples = static_cast<size_t>(durationSec * 44100.0);
    std::vector<int16_t> samples(nSamples);
    for (size_t i = 0; i < nSamples; ++i) {
        double t = static_cast<double>(i) / 44100.0;
        double tone = std::sin(frequencyHz * t * 2.0 * M_PI);
        samples[i] = static_cast<int16_t>(tone * volume * 32767.0);
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) return;
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }
    if (pid == 0) {
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("aplay", "aplay", "-q", "-r", "44100", "-f", "S16_LE", "-t", "raw", "-c", "1",
               static_cast<char*>(nullptr));
        _exit(127);
    }
    close(pipefd[0]);
    ssize_t written = write(pipefd[1], samples.data(), samples.size() * sizeof(int16_t));
    (void)written;
    close(pipefd[1]);
    int status = 0;
    waitpid(pid, &status, 0);
}

void playBeepAsync(double frequencyHz, double durationSec, double volume = 0.5) {
    std::thread(playBeepBlocking, frequencyHz, durationSec, volume).detach();
}

} // namespace

Voca::Voca(std::string modelPath, Callbacks callbacks)
    : modelPath_(std::move(modelPath)), callbacks_(std::move(callbacks)) {}

void Voca::beep(double frequencyHz, double durationSec) {
    muteUntilSec_ = monotonicSeconds() + durationSec + kBeepMuteGuardSec;
    playBeepAsync(frequencyHz, durationSec);
}

Voca::~Voca() { stop(); }

bool Voca::start() {
    if (running_.load()) return true;

    if (Pa_Initialize() != paNoError) {
        std::cerr << "voca: failed to initialize PortAudio\n";
        return false;
    }

    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = true; // falls back to CPU automatically if no CUDA device is found
    ctx_ = whisper_init_from_file_with_params(modelPath_.c_str(), cparams);
    if (ctx_ == nullptr) {
        std::cerr << "voca: failed to load whisper model at '" << modelPath_ << "'\n";
        Pa_Terminate();
        return false;
    }

    shuttingDown_ = false;
    running_ = true;
    lastSpeechMonotonicSec_ = monotonicSeconds();

    captureThread_ = std::thread([this] { captureLoop(); });
    transcribeThread_ = std::thread([this] { transcribeLoop(); });
    return true;
}

void Voca::stop() {
    if (!running_.load()) return;
    shuttingDown_ = true;
    running_ = false;

    queueCv_.notify_all();
    if (captureThread_.joinable()) captureThread_.join();
    if (transcribeThread_.joinable()) transcribeThread_.join();

    if (ctx_ != nullptr) {
        whisper_free(ctx_);
        ctx_ = nullptr;
    }
    Pa_Terminate();
}

bool Voca::isRunning() const { return running_.load(); }

void Voca::pause() { busy_ = true; }

void Voca::resume() {
    busy_ = false;
    std::lock_guard<std::mutex> lock(lastSpeechMutex_);
    lastSpeechMonotonicSec_ = monotonicSeconds();
}

bool Voca::isBusy() const { return busy_.load(); }

bool Voca::isAwake() const { return awake_.load(); }

void Voca::setAwake(bool awake) {
    awake_ = awake;
    std::lock_guard<std::mutex> lock(lastSpeechMutex_);
    lastSpeechMonotonicSec_ = monotonicSeconds();
}

void Voca::wake() { setAwake(true); }

void Voca::sleep() { setAwake(false); }

bool Voca::textAvailable() const {
    std::lock_guard<std::mutex> lock(textQueueMutex_);
    return !textQueue_.empty();
}

std::string Voca::getNextLine() {
    std::lock_guard<std::mutex> lock(textQueueMutex_);
    if (textQueue_.empty()) return {};
    std::string line = std::move(textQueue_.front());
    textQueue_.pop_front();
    return line;
}

void Voca::pushTranscript(const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(textQueueMutex_);
        textQueue_.push_back(text);
    }
    if (callbacks_.onTranscript) callbacks_.onTranscript(text);
}

void Voca::checkAutoSleep() {
    if (!awake_.load()) return;
    double elapsed;
    {
        std::lock_guard<std::mutex> lock(lastSpeechMutex_);
        elapsed = monotonicSeconds() - lastSpeechMonotonicSec_;
    }
    if (elapsed > kAutoSleepTimeoutSec) {
        beep(440, 0.1);
        awake_ = false;
        if (callbacks_.onSleep) callbacks_.onSleep();
    }
}

void Voca::captureLoop() {
    PaStreamParameters inputParams{};
    inputParams.device = Pa_GetDefaultInputDevice();
    // Lets a specific mic be pinned (e.g. a USB mic on a machine where the
    // ALSA/pipewire "default" input isn't the one actually wired up).
    if (const char* override = std::getenv("VOCA_INPUT_DEVICE")) {
        inputParams.device = static_cast<PaDeviceIndex>(std::atoi(override));
    }
    if (inputParams.device == paNoDevice) {
        std::cerr << "voca: no default input (microphone) device found\n";
        running_ = false;
        return;
    }
    inputParams.channelCount = 1;
    inputParams.sampleFormat = paInt16;
    inputParams.suggestedLatency = Pa_GetDeviceInfo(inputParams.device)->defaultLowInputLatency;
    inputParams.hostApiSpecificStreamInfo = nullptr;

    PaStream* stream = nullptr;
    PaError err = Pa_OpenStream(&stream, &inputParams, nullptr, kSampleRate, kFrameSamples,
                                 paClipOff, nullptr, nullptr);
    if (err != paNoError) {
        std::cerr << "voca: failed to open mic stream: " << Pa_GetErrorText(err) << "\n";
        running_ = false;
        return;
    }
    if (Pa_StartStream(stream) != paNoError) {
        std::cerr << "voca: failed to start mic stream\n";
        Pa_CloseStream(stream);
        running_ = false;
        return;
    }

    std::vector<int16_t> frame(kFrameSamples);
    std::vector<float> phraseBuffer;
    bool inSpeech = false;
    double ambientRms = kMinEnergyThreshold;
    double silenceStartSec = 0.0;
    double phraseStartSec = 0.0;
    const bool debug = std::getenv("VOCA_DEBUG") != nullptr;

    while (!shuttingDown_.load()) {
        PaError readErr = Pa_ReadStream(stream, frame.data(), static_cast<unsigned long>(kFrameSamples));
        if (readErr != paNoError && readErr != paInputOverflowed) {
            std::cerr << "voca: mic read error: " << Pa_GetErrorText(readErr) << "\n";
            break;
        }

        checkAutoSleep();

        double now = monotonicSeconds();
        if (now < muteUntilSec_.load()) {
            // Ignoring our own wake/sleep beep - see beep(). Don't fold this
            // frame into ambient tracking or any in-progress phrase either.
            continue;
        }

        double level = rms(frame.data(), frame.size());
        double threshold = std::max(ambientRms * kEnergyMultiplier, kMinEnergyThreshold);

        if (level > threshold) {
            if (!inSpeech) {
                inSpeech = true;
                phraseBuffer.clear();
                phraseStartSec = now;
            }
            for (int16_t s : frame) phraseBuffer.push_back(static_cast<float>(s) / 32768.0f);
            silenceStartSec = now;
        } else {
            // Not currently loud, but keep tracking the ambient noise floor.
            ambientRms += kAmbientAdaptRate * (level - ambientRms);

            if (inSpeech) {
                for (int16_t s : frame) phraseBuffer.push_back(static_cast<float>(s) / 32768.0f);

                bool pausedLongEnough = (now - silenceStartSec) >= kPauseThresholdSec;
                bool tooLong = (now - phraseStartSec) >= kMaxPhraseSec;
                if (pausedLongEnough || tooLong) {
                    double phraseDurationSec = static_cast<double>(phraseBuffer.size()) / kSampleRate;
                    inSpeech = false;
                    if (phraseDurationSec >= kMinPhraseSec) {
                        if (debug) {
                            std::cerr << "voca[debug]: phrase queued, " << phraseDurationSec << "s\n";
                        }
                        std::lock_guard<std::mutex> lock(queueMutex_);
                        phraseQueue_.push_back(std::move(phraseBuffer));
                        queueCv_.notify_one();
                    } else if (debug) {
                        std::cerr << "voca[debug]: phrase discarded (too short), " << phraseDurationSec
                                   << "s\n";
                    }
                    phraseBuffer.clear();
                }
            }
        }
        if (debug && level > 5.0) {
            std::cerr << "voca[debug]: level=" << level << " threshold=" << threshold
                       << " ambient=" << ambientRms << " inSpeech=" << inSpeech << "\n";
        }
    }

    Pa_StopStream(stream);
    Pa_CloseStream(stream);
}

void Voca::transcribeLoop() {
    const bool debug = std::getenv("VOCA_DEBUG") != nullptr;
    while (true) {
        std::vector<float> audio;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [this] { return shuttingDown_.load() || !phraseQueue_.empty(); });
            if (shuttingDown_.load() && phraseQueue_.empty()) return;
            audio = std::move(phraseQueue_.front());
            phraseQueue_.pop_front();
        }

        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wparams.language = "en";
        wparams.n_threads = 4;
        wparams.print_progress = false;
        wparams.print_realtime = false;
        wparams.print_special = false;
        wparams.print_timestamps = false;
        wparams.single_segment = false;
        wparams.no_context = true;

        if (whisper_full(ctx_, wparams, audio.data(), static_cast<int>(audio.size())) != 0) {
            continue;
        }

        std::string text;
        int nSegments = whisper_full_n_segments(ctx_);
        for (int i = 0; i < nSegments; ++i) {
            if (!text.empty()) text += " ";
            text += whisper_full_get_segment_text(ctx_, i);
        }
        // Trim whitespace whisper.cpp tends to leave around segments.
        while (!text.empty() && text.front() == ' ') text.erase(text.begin());
        while (!text.empty() && text.back() == ' ') text.pop_back();

        if (debug) {
            std::cerr << "voca[debug]: whisper returned " << nSegments << " segments, text='" << text
                       << "'\n";
        }
        if (!text.empty() && !isNonSpeechMarker(text)) {
            // handleTranscript() dispatches to caller-supplied callbacks -
            // an uncaught exception from user code would otherwise escape
            // this thread and call std::terminate() on the whole process
            // (standard behavior for exceptions crossing a std::thread
            // boundary), taking the host program down with it.
            try {
                handleTranscript(text);
            } catch (const std::exception& e) {
                std::cerr << "voca: callback threw: " << e.what() << "\n";
            } catch (...) {
                std::cerr << "voca: callback threw a non-standard exception\n";
            }
        }
    }
}

void Voca::handleTranscript(const std::string& text) {
    std::vector<Word> words = tokenize(text);
    size_t matchEndPos = 0;

    // 1. Interrupt check - takes priority whenever paused/busy (e.g. TTS speaking).
    if (busy_.load()) {
        std::string found = findPhraseWords(words, kInterruptPhrases, matchEndPos);
        if (!found.empty() && callbacks_.onInterrupt) callbacks_.onInterrupt(text);
        return;
    }

    // 2. Wake word check.
    if (!awake_.load()) {
        std::string found = findWakeWord(words, matchEndPos);
        if (found.empty()) return;

        beep(880, 0.1);
        setAwake(true);
        if (callbacks_.onWake) callbacks_.onWake(found);

        std::string remaining = text.substr(std::min(matchEndPos, text.size()));
        size_t firstNonPunct = remaining.find_first_not_of(".,!?; ");
        remaining = (firstNonPunct == std::string::npos) ? "" : remaining.substr(firstNonPunct);

        if (!remaining.empty()) pushTranscript(remaining);
        return;
    }

    // 3. Sleep phrase check.
    if (findSleepTrigger(words)) {
        beep(440, 0.1);
        setAwake(false);
        if (callbacks_.onSleep) callbacks_.onSleep();
        return;
    }

    // 4. Normal transcription.
    {
        std::lock_guard<std::mutex> lock(lastSpeechMutex_);
        lastSpeechMonotonicSec_ = monotonicSeconds();
    }
    pushTranscript(text);
}
