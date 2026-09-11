#include "io_worker.h"

#include <chrono>
#include <thread>

#include "olla.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

#include <portaudio.h>
#include <whisper.h>

namespace fs = std::filesystem;

// ============================================================================
// TextToSpeech (offline TTS via espeak-ng + aplay) - moved here from
// tts.hpp/.cpp so it's declared/used only as IO_WORKER_CLASS's private `tts`
// member (see io_worker.h's class comment).
// ============================================================================

namespace {

// Exit code we force in the child when execlp() itself fails (see synthesize()/
// playback() below), so the parent can tell "binary not found" apart from "binary
// ran and returned an error" (e.g. espeak-ng given an uninstalled voice).
constexpr int kExecFailedExitCode = 127;

void reportFailure(const char* tool, int status) {
    if (WIFEXITED(status) && WEXITSTATUS(status) == kExecFailedExitCode) {
        std::cerr << "tts: '" << tool << "' not found - is it installed? "
                     "(see README for dependencies)\n";
    } else if (WIFEXITED(status)) {
        std::cerr << "tts: '" << tool << "' failed (exit code " << WEXITSTATUS(status) << ")\n";
    } else {
        std::cerr << "tts: '" << tool << "' terminated abnormally\n";
    }
}

std::string makeTempWavPath() {
    std::string tmpl = "/tmp/tts_XXXXXX.wav";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = mkstemps(buf.data(), 4); // ".wav" suffix length
    if (fd < 0) return {};
    close(fd);
    return std::string(buf.data());
}

void redirectStdoutStderrToDevNull() {
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull < 0) return;
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    close(devnull);
}

} // namespace

TextToSpeech::TextToSpeech(std::string voice, int wordsPerMinute)
    : voice_(std::move(voice)), speed_(wordsPerMinute) {
    workerThread_ = std::thread([this] { workerLoop(); });
}

TextToSpeech::~TextToSpeech() {
    stop();
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        shuttingDown_ = true;
    }
    queueCv_.notify_one();
    if (workerThread_.joinable()) workerThread_.join();
}

void TextToSpeech::setVoice(const std::string& voice) { voice_ = voice; }

void TextToSpeech::setSpeed(int wordsPerMinute) { speed_ = wordsPerMinute; }

bool TextToSpeech::isSpeaking() const { return speaking_.load(); }

bool TextToSpeech::synthesize(const std::string& text, const std::string& wavPath) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        redirectStdoutStderrToDevNull();
        execlp("espeak-ng", "espeak-ng",
               "-v", voice_.c_str(),
               "-s", std::to_string(speed_).c_str(),
               "-w", wavPath.c_str(),
               static_cast<char*>(nullptr));
        _exit(kExecFailedExitCode);
    }

    close(pipefd[0]);
    // Text goes over stdin (not argv or a shell string), so no command-injection risk.
    ssize_t written = write(pipefd[1], text.data(), text.size());
    (void)written;
    close(pipefd[1]);

    int status = 0;
    waitpid(pid, &status, 0);
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok) reportFailure("espeak-ng", status);
    return ok;
}

bool TextToSpeech::playback(const std::string& wavPath) {
    pid_t pid = fork();
    if (pid < 0) return false;

    if (pid == 0) {
        redirectStdoutStderrToDevNull();
        execlp("aplay", "aplay", "-q", wavPath.c_str(), static_cast<char*>(nullptr));
        _exit(kExecFailedExitCode);
    }

    playerPid_ = pid;
    int status = 0;
    waitpid(pid, &status, 0);
    playerPid_ = -1;
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok && !stopRequested_) reportFailure("aplay", status);
    return ok;
}

bool TextToSpeech::speak(const std::string& text) {
    if (text.empty()) return true;

    std::lock_guard<std::mutex> lock(speakMutex_);
    stopRequested_ = false;
    std::string wavPath = makeTempWavPath();
    if (wavPath.empty()) return false;

    speaking_ = true;
    bool ok = synthesize(text, wavPath);
    if (ok) {
        // stop() may have arrived while we were still synthesizing, before playerPid_
        // was set - catch that here instead of playing an utterance no one asked for.
        ok = stopRequested_ ? false : playback(wavPath);
    }
    speaking_ = false;

    std::error_code ec;
    fs::remove(wavPath, ec);
    return ok;
}

void TextToSpeech::speakAsync(const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queue_.push_back(text);
    }
    queueCv_.notify_one();
}

void TextToSpeech::stop() {
    stopRequested_ = true;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queue_.clear();
    }
    int pid = playerPid_.load();
    if (pid > 0) kill(pid, SIGTERM);
}

void TextToSpeech::workerLoop() {
    while (true) {
        std::string text;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [this] { return shuttingDown_ || !queue_.empty(); });
            if (shuttingDown_ && queue_.empty()) return;
            text = std::move(queue_.front());
            queue_.pop_front();
        }
        speak(text);
    }
}

// ============================================================================
// Voca (offline wake-word + speech-to-text via PortAudio + whisper.cpp) -
// moved here from voca.hpp/.cpp so it's declared/used only as
// IO_WORKER_CLASS's private `voca` member (see io_worker.h's class comment).
// ============================================================================

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

constexpr double kAutoSleepTimeoutSec = 25.0; // 25 seconds

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

// ============================================================================
// IO_WORKER_CLASS
// ============================================================================

IO_WORKER_CLASS::~IO_WORKER_CLASS()
{
}

void IO_WORKER_CLASS::create(const std::filesystem::path& audio_shared_path)
{
    // Voca's whisper model lives in the shared ~/olli_files/models, not the
    // per-profile directory (see Settings::get_shared_path(), main.cpp).
    // Stashed for thread_main() to actually build voca from, right before
    // it sets RUN = true - see its own comment for why construction is
    // deferred that far instead of happening here.
    audio_settings_path = audio_shared_path;
}

void IO_WORKER_CLASS::speak(const std::string& text)
{
    if (tts) tts->speakAsync(text);
}

void IO_WORKER_CLASS::stop_speaking()
{
    if (tts) tts->stop();
}

void IO_WORKER_CLASS::VOCA_manual_set(int Command)
{
    if (voca == nullptr) return;

    switch (Command)
    {
        case DEF_VOCA_SLEEP:
            voca->sleep();
            break;
        case DEF_VOCA_PAUSE:
            voca->pause();
            break;
        case DEF_VOCA_LISTEN:
            voca->wake();
            voca->resume();
            break;
        default:
            std::cerr << "Invalid VOCA command: " << Command << std::endl;
            break;
    }
}

void IO_WORKER_CLASS::adjust_audio_files()
{
    if (voca == nullptr || tts == nullptr) return;

    bool tts_is_speaking = tts->isSpeaking();
    bool tts_speaking_changed = (tts_is_speaking != tts_was_speaking);
    tts_was_speaking = tts_is_speaking;

    // If TTS starts speaking, pause Voca so it doesn't hear olli's own
    // voice. If TTS stops speaking, resume listening. Voca handles its own
    // wake-word detection and auto-sleep timeout internally - nothing else
    // to poll here.
    if (tts_speaking_changed)
    {
        if (tts_is_speaking)
        {
            voca->pause();
        }
        else
        {
            voca->resume();
        }
    }
}

bool IO_WORKER_CLASS::popVocaEvent(VOCA_EVENT& out)
{
    // Interrupt-only events take priority - they signal "stop what's
    // happening now" and shouldn't wait behind queued transcript text.
    {
        std::lock_guard<std::mutex> lock(voca_events_mutex);
        if (!voca_events.empty())
        {
            out = std::move(voca_events.front());
            voca_events.pop_front();
            return true;
        }
    }

    if (voca != nullptr && voca->textAvailable())
    {
        out = VOCA_EVENT{voca->getNextLine(), ""};
        return true;
    }

    return false;
}

void IO_WORKER_CLASS::display_with_tts(COMMS& comms_tty_stt)
{
    // THINKING/SYSTEM/TOOL_ATTACHMENTS aren't spoken - drop them here each
    // tick regardless of whether tts exists, same as INPUT_FROM_LLM below,
    // so they don't grow unbounded doing nothing.
    comms_tty_stt.INPUT_FROM_THINKING.clear();
    comms_tty_stt.INPUT_FROM_SYSTEM.clear();
    comms_tty_stt.TOOL_ATTACHMENTS.clear();

    if (!tts)
    {
        comms_tty_stt.INPUT_FROM_LLM.clear();
        return;
    }

    // comms_tty_stt itself gets wholesale-overwritten from comms_buffer
    // every tick (see thread_main()'s own comment, step 5/9), so anything
    // not yet spoken has to live in tts_pending instead, surviving across
    // ticks independent of comms_tty_stt's own lifetime.
    tts_pending += comms_tty_stt.INPUT_FROM_LLM;
    comms_tty_stt.INPUT_FROM_LLM.clear();

    // Speak once idle, chunking into "whatever arrived since it last went
    // idle" instead of one call per streamed token, without needing a
    // punctuation/length heuristic.
    if (!tts->isSpeaking() && !tts_pending.empty())
    {
        // Drained either way so disabled speech doesn't just pile up and
        // dump out once re-enabled - ENABLE_TTS_OUTPUT (copied through
        // comms_buffer -> comms_tty_stt same as every other field) only
        // gates whether it's actually spoken.
        if (comms_tty_stt.ENABLE_TTS_OUTPUT) tts->speakAsync(tts_pending);
        tts_pending.clear();
    }
}

bool IO_WORKER_CLASS::poll_web_event(std::string& out)
{
    return web_server && web_server->poll_input(out);
}

void IO_WORKER_CLASS::display_with_web(const std::string& /*input_from_user_echo*/, COMMS& comms_browser,
                                        const std::vector<std::string>& tool_names_arg,
                                        SCROLL_KEY /*scroll_request*/, bool /*focus_cycle_requested*/)
{
    // input_from_user_echo/scroll_request/focus_cycle_requested still
    // unused - tool_names_arg is now the second parameter actually used
    // (named _arg to avoid shadowing this class's own tool_names member -
    // see this method's own comment, io_worker.h, for why the signature
    // matches display_with_ncurses()'s in full regardless).
    if (!web_server || !web_server->has_client())
    {
        comms_browser.INPUT_FROM_LLM.clear();
        comms_browser.INPUT_FROM_THINKING.clear();
        comms_browser.INPUT_FROM_SYSTEM.clear();
        comms_browser.TOOL_ATTACHMENTS.clear();
        return;
    }

    web_server->push_output(comms_browser.INPUT_FROM_LLM, comms_browser.INPUT_FROM_THINKING,
                             comms_browser.INPUT_FROM_SYSTEM, comms_browser.TOOL_ATTACHMENTS);
    web_server->push_tool_names(tool_names_arg);
    comms_browser.INPUT_FROM_LLM.clear();
    comms_browser.INPUT_FROM_THINKING.clear();
    comms_browser.INPUT_FROM_SYSTEM.clear();
    comms_browser.TOOL_ATTACHMENTS.clear();
}

void IO_WORKER_CLASS::thread_start()
{
    THREAD_CONTROL.create(1000);
    THREAD_CONTROL.start_render_thread([this]() { thread_main(); });
}

void IO_WORKER_CLASS::thread_stop()
{
    // Signal exit, then actually wait for the background thread to finish
    // before returning - key_input/output are destroyed right after this
    // object is (reverse member order), so thread_main() must be fully
    // joined first or it could still be touching them mid-teardown. tts/
    // voca are torn down inside thread_main() itself, before it actually
    // returns (and this wait unblocks) - see its own comment.
    RUN = false;
    THREAD_CONTROL.wait_for_thread_to_finish();
}

void IO_WORKER_CLASS::thread_main()
{
    // tts/voca are constructed here, right before RUN = true, and torn
    // down right after the while(RUN) loop below exits - not for this
    // object's whole lifetime, just this worker's own running window (see
    // io_worker.h's class comment). Voca's whisper model load + mic/thread
    // startup and TextToSpeech's own worker-thread startup both happen
    // here as a result, on this background thread rather than blocking
    // whoever calls thread_start().
    tts = std::make_unique<TextToSpeech>();

    Voca::Callbacks callbacks;

    // Ordinary transcript text is pulled directly from voca's own
    // textAvailable()/getNextLine() in popVocaEvent() below - no need to
    // duplicate it into a callback-fed queue here too.

    callbacks.onInterrupt = [this](const std::string& /*text*/)
    {
        // Interrupt-only: signals this same thread_main() tick to stop
        // TTS/sidetrack without submitting anything new as a chat message.
        std::lock_guard<std::mutex> lock(voca_events_mutex);
        voca_events.push_back(VOCA_EVENT{"", ""});
    };

    // Same queue/mutex onInterrupt above already uses - not a raw cout,
    // since this callback runs on Voca's own thread and a direct terminal
    // write would either get overwritten by the next ncurses redraw or
    // briefly corrupt the screen. The tick loop below is the one place
    // that actually turns this into a displayed system message, via
    // COMMS::log() - same as any other status text.
    callbacks.onWake = [this](const std::string& trigger)
    {
        std::lock_guard<std::mutex> lock(voca_events_mutex);
        VOCA_EVENT event;
        event.status_message = "[VOCA] awake (\"" + trigger + "\")\n";
        voca_events.push_back(event);
    };

    callbacks.onSleep = [this]()
    {
        std::lock_guard<std::mutex> lock(voca_events_mutex);
        VOCA_EVENT event;
        event.status_message = "[VOCA] asleep\n";
        voca_events.push_back(event);
    };

    voca = std::make_unique<Voca>((audio_settings_path / "models" / "ggml-small.en.bin").string(),
                                   std::move(callbacks));
    if (!voca->start())
    {
        std::cerr << "IO_WORKER_CLASS: Voca failed to start - speech-to-text disabled.\n";
        voca.reset();
    }

    // Same RUN-window lifecycle as tts/voca above - see this method's own
    // comment. Non-fatal if it fails to bind its port (e.g. already in
    // use) - web_server.reset() leaves poll_web_event()/display_with_web()
    // no-oping, same as tts/voca being null.
    web_server = std::make_unique<WEB_SERVER_CLASS>();
    if (!web_server->start())
    {
        web_server.reset();
    }

    RUN = true;
    while (RUN)
    {
        if (!INTERUPTED.load())
        {
            PROCESSING.store(true);

            // 0. Opposite-direction request from the main thread, relayed
            // into comms_buffer by exchange() same as everything else now -
            // see COMMS::close_chat_log_requested's comment, comms.h.
            if (comms_buffer.close_chat_log_requested.load())
            {
                comms_buffer.close_chat_log_requested.store(false);
                output.close_chat_log();
            }

            // 1. Keyboard - non-blocking, drains whatever's available.
            // Synced from comms_buffer right before reading, so
            // keyboard_input() actually reflects whatever exchange() last
            // relayed in from the real comms - previously nothing set this,
            // so it stayed stuck at its default true regardless of the flag.
            key_input.PROPS.CHAT_INPUT_ENABLED = comms_buffer.ENABLE_KEYBOARD_INPUT;
            key_input.keyboard_input();

            // 2. Mirror key_input's own live state. IS_TYPING goes into
            // comms_buffer - relayed on to comms via exchange() same as
            // everything else in that block (see COMMS::IS_TYPING's
            // comment, comms.h). input_from_user_echo is separate - not
            // part of the comms/exchange() surface at all, just what's
            // currently typed, for display_with_ncurses() to show live
            // (see its own comment).
            comms_buffer.IS_TYPING = key_input.IS_TYPING;
            input_from_user_echo = key_input.LINE;
            scroll_request = key_input.SCROLL_REQUEST;
            key_input.SCROLL_REQUEST = SCROLL_KEY::NONE;
            focus_cycle_requested = key_input.FOCUS_CYCLE_REQUESTED;
            key_input.FOCUS_CYCLE_REQUESTED = false;
            show_links_requested = key_input.SHOW_LINKS_REQUESTED;
            key_input.SHOW_LINKS_REQUESTED = false;

            // 3. Ctrl+C - checked early, same priority it had in main.cpp's
            // old loop (wins over anything else this tick).
            if (key_input.EXIT_REQUESTED)
            {
                key_input.EXIT_REQUESTED = false;
                comms_buffer.EXIT_REQUESTED = true;
            }

            // 4. Pause/resume voca on a tts.isSpeaking() transition - see
            // this method's own comment for why this used to be a separate
            // 500ms-tick thread (AUDIO_CONTROL_CLASS) and no longer is.
            adjust_audio_files();

            // 5. Snapshot comms_buffer into every channel's own copy - each
            // channel below writes any new input from its own source into
            // its own copy first, then hands the WHOLE copy back to
            // comms_buffer as one unit (see comms_buffer's own comment,
            // io_worker.h). Snapshotting here, before any channel is
            // checked, means whichever channel goes first sees (and hands
            // back) an accurate, current copy rather than a stale one.
            comms_stt_tts = comms_buffer;
            comms_keyboard = comms_buffer;
            comms_web = comms_buffer;

            // 5.5. Web: same shape as STT's step 6 and keyboard's step 8
            // below, just checked here since it doesn't interact with
            // key_input.INTERRUPTED (step 7) the way STT does.
            std::string web_line;
            if (poll_web_event(web_line))
            {
                if (comms_buffer.ENTER_PRESSED)
                {
                    comms_buffer.INPUT_FROM_USER += "\n" + web_line;
                }
                else
                {
                    comms_web.INPUT_FROM_USER = web_line;
                    comms_web.ENTER_PRESSED = true;
                    comms_buffer = comms_web;
                }
            }

            // 6. Pop at most one pending voice event this tick. STT (Voca)
            // lives inside this worker's own thread_main() now, not behind
            // key_input like typed input - so a transcript goes straight
            // into comms_stt_tts rather than staging through key_input.LINE/
            // ENTER_PRESSED first. A wake/sleep status_message (see
            // VOCA_EVENT, io_worker.h) is neither a transcript nor an
            // interrupt - just log it straight to comms_buffer and skip the
            // rest of this block for that event.
            VOCA_EVENT voca_event;
            if (popVocaEvent(voca_event))
            {
                if (!voca_event.status_message.empty())
                {
                    comms_buffer.INPUT_FROM_SYSTEM += voca_event.status_message;
                }
                else
                {
                    if (!voca_event.text.empty())
                    {
                        if (comms_buffer.ENTER_PRESSED)
                        {
                            // exchange() hasn't picked up a still-pending
                            // submission yet (from this channel, or from
                            // keyboard's own step below, later this same
                            // tick) - append rather than drop or overwrite
                            // it, so nothing said/typed gets lost; both go
                            // out together on the next exchange() cycle.
                            comms_buffer.INPUT_FROM_USER += "\n" + voca_event.text;
                        }
                        else
                        {
                            comms_stt_tts.INPUT_FROM_USER = voca_event.text;
                            comms_stt_tts.ENTER_PRESSED = true;
                            comms_buffer = comms_stt_tts;
                        }
                    }
                    key_input.INTERRUPTED = true;
                }
            }

            // 7. Interrupt handling - audio is interrupted directly (this
            // worker already owns/holds it for its own draining/polling
            // below, so this isn't part of the comms/exchange() surface at
            // all). Only the "stop chat's own in-flight generation" part
            // needs to reach the main thread, since only ollama_system can
            // stop its own chat_thread - that goes through
            // comms_buffer.INTERRUPTED instead. sidetrack used to also get
            // signaled here (SIGNALS.INTERUPT_SIGNAL) - dropped along with
            // the rest of sidetrack's wiring into this class; sidetrack is
            // being reworked and will need its own way to learn about this.
            if (key_input.INTERRUPTED)
            {
                stop_speaking();
                comms_buffer.INTERRUPTED = true;
                key_input.INTERRUPTED = false; // consumed - NOT a blanket
                                                // reset(), which would also
                                                // wipe LINE mid-typing (see
                                                // step 8's own note)
            }

            // 8. Submission - echo to the screen here, once, uniformly for
            // typed and voice input (see step 6's own comment). Same
            // append-if-pending handling as step 6 above, via comms_keyboard
            // instead of comms_stt_tts. Only clears LINE/ENTER_PRESSED here,
            // not via key_input.reset() - that would also clear LINE on
            // ticks where the user is still mid-typing (ENTER_PRESSED still
            // false), erasing whatever they'd typed so far before they ever
            // got to press Enter.
            if (key_input.ENTER_PRESSED)
            {
                output.user_input += key_input.LINE;

                if (comms_buffer.ENTER_PRESSED)
                {
                    comms_buffer.INPUT_FROM_USER += "\n" + key_input.LINE;
                }
                else
                {
                    comms_keyboard.INPUT_FROM_USER = key_input.LINE;
                    comms_keyboard.ENTER_PRESSED = true;
                    comms_buffer = comms_keyboard;
                }

                key_input.LINE.clear();
                key_input.ENTER_PRESSED = false;
            }

            // 9. Final re-sync: every channel gets the authoritative,
            // up-to-date snapshot, including whatever any of them merged
            // back in steps 5.5/6/8 above. comms_buffer's own copy of the
            // screen/speech-direction fields is cleared right here, now
            // that every channel holds its own copy to work from - NOT
            // done any earlier (e.g. alongside step 5's snapshot), which
            // would wipe out what step 5 just gave them before any had a
            // chance to use it.
            comms_stt_tts = comms_buffer;
            comms_keyboard = comms_buffer;
            comms_web = comms_buffer;
            comms_buffer.INPUT_FROM_LLM.clear();
            comms_buffer.INPUT_FROM_THINKING.clear();
            comms_buffer.INPUT_FROM_SYSTEM.clear();
            comms_buffer.TOOL_ATTACHMENTS.clear();

            // 10. Drain into the screen - comms_keyboard (this channel's
            // own copy, synced from comms_buffer at step 9 above), not
            // comms_buffer directly, so this doesn't race comms_stt_tts's
            // own independent drain in display_with_tts() below over the
            // same source text. Still under output_buffer_mutex
            // (get_response()'s own lock).
            output.get_response(comms_keyboard);

            // 10.5. Web-links popup - Ctrl+L. Handled right here, after
            // get_response() just above has drained comms_keyboard's own
            // TOOL_ATTACHMENTS into output.web_links, so the popup always
            // sees whatever arrived this same tick. A no-op (inside the
            // function itself) if there's nothing to show yet.
            if (show_links_requested)
            {
                output.show_web_links_panel();
            }

            // chat.pull_background_output(output) used to run here too -
            // dropped along with chat's removal from this class; task
            // runner is expected to go through comms instead once it's
            // reworked, rather than this getting reconnected as-is.
            // sidetrack.pull_output(output) used to run here too - dropped
            // along with the rest of sidetrack's wiring into this class
            // (see thread_main()'s parameter comment); sidetrack's own
            // output isn't drawn to the screen until that's reworked.

            // 11. Draw.
            if (USE_NCURSES)
            {
                output.display_with_ncurses(input_from_user_echo, comms_keyboard, tool_names,
                                             scroll_request, focus_cycle_requested);
            }
            else
            {
                output.display();
            }

            // 12. Voice channel's own drain+speak step - see its own
            // comment (io_worker.h) for why this is a dedicated function
            // now rather than inline accumulate-and-speak logic here.
            display_with_tts(comms_stt_tts);

            // 13. Web channel's own drain+push step - same signature as
            // display_with_ncurses() above, see this method's own comment
            // (io_worker.h) for why.
            display_with_web(input_from_user_echo, comms_web, tool_names,
                              scroll_request, focus_cycle_requested);

            PROCESSING.store(false);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(PROPS.INTERVAL)));
    }

    // Unload tts/voca/web_server now that this worker is no longer running
    // - see this function's own opening comment. voca->stop() first (joins
    // its capture/transcribe threads) before reset() actually destroys it;
    // tts's and web_server's own destructors (run by their reset()) stop/
    // join their own worker thread(s) the same way.
    if (voca != nullptr) voca->stop();
    voca.reset();
    tts.reset();
    web_server.reset();
}

void IO_WORKER_CLASS::exchange(COMMS& comms, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list)
{
    if (!PROPS.BLOCKING) return;

    INTERUPTED.store(true);

    // Wait out any background pass already in flight - INTERUPTED only
    // stops a NEW pass from starting, it doesn't abort one already
    // running.
    while (PROCESSING.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Safe to touch tool_names here - thread_main() is confirmed not
    // running (the wait above), so this doesn't race its own read of
    // tool_names down in the display step. Each tool's own tool_functions
    // (TOOL_BASE, tools.h) already holds whatever names its last
    // register_tool() call registered - no need to call register_tool()
    // again here just to re-derive them into a throwaway json array.
    tool_names.clear();
    for (auto& tool : tools_list)
        for (auto& name : tool->tool_functions)
            tool_names.push_back(name);

    // Output direction: comms (real, owned by main_process()) -> comms_buffer
    // (this worker's own copy). Used to also fan out a second copy directly
    // to comms_buffer_audio here - dropped once STT and TTS were folded
    // into one shared comms_stt_tts covering the whole voice channel;
    // thread_main() now copies comms_buffer's own drain into comms_stt_tts
    // itself each tick (see its step 4.5) instead of this doing it. Same
    // output_buffer_mutex OUTPUT_CLASS::get_response() (user_io.cpp) uses,
    // since producer threads (chat_thread, sidetrack) append to comms
    // concurrently with this main-thread copy.
    {
        std::lock_guard<std::mutex> lock(output_buffer_mutex);

        if (!comms.INPUT_FROM_LLM.empty())
        {
            comms_buffer.INPUT_FROM_LLM += comms.INPUT_FROM_LLM;
            comms.INPUT_FROM_LLM.clear();
        }

        if (!comms.TOOL_ATTACHMENTS.empty())
        {
            comms_buffer.TOOL_ATTACHMENTS.insert(comms_buffer.TOOL_ATTACHMENTS.end(), comms.TOOL_ATTACHMENTS.begin(), comms.TOOL_ATTACHMENTS.end());
            comms.TOOL_ATTACHMENTS.clear();
        }

        if (!comms.INPUT_FROM_THINKING.empty())
        {
            comms_buffer.INPUT_FROM_THINKING += comms.INPUT_FROM_THINKING;
            comms.INPUT_FROM_THINKING.clear();
        }

        if (!comms.INPUT_FROM_SYSTEM.empty())
        {
            comms_buffer.INPUT_FROM_SYSTEM += comms.INPUT_FROM_SYSTEM;
            comms.INPUT_FROM_SYSTEM.clear();
        }

        // Plain assignment, not append-then-clear like the text buffers
        // above - these are a current setting, not accumulating content, so
        // there's nothing to clear on the source side either.
        comms_buffer.INPUT_FROM_LLM_COLOR = comms.INPUT_FROM_LLM_COLOR;
        comms_buffer.INPUT_FROM_USER_COLOR = comms.INPUT_FROM_USER_COLOR;
        comms_buffer.ENABLE_KEYBOARD_INPUT = comms.ENABLE_KEYBOARD_INPUT;
        comms_buffer.ENABLE_TTS_OUTPUT = comms.ENABLE_TTS_OUTPUT;
    }

    // Same direction as the block above (comms -> comms_buffer), just
    // atomic instead of mutex-guarded - see COMMS::close_chat_log_requested's
    // comment (comms.h). thread_main() reads/clears its own comms_buffer
    // copy instead of touching comms directly now.
    if (comms.close_chat_log_requested.load())
    {
        comms_buffer.close_chat_log_requested.store(true);
        comms.close_chat_log_requested.store(false);
    }

    // Input direction: comms_buffer (this worker's own copy) -> comms
    // (real, owned by main_process()) - same fields as before, just one
    // simple copy-then-clear if per field instead of a combined guard.
    if (comms_buffer.ENTER_PRESSED)
    {
        comms.ENTER_PRESSED = true;
        comms_buffer.ENTER_PRESSED = false;
    }

    if (!comms_buffer.INPUT_FROM_USER.empty())
    {
        // Locked - ollama_system::input() (olla.cpp) can write this same
        // field from chat_thread at the same time, to restore it right
        // before send() reads it back out.
        std::lock_guard<std::mutex> lock(output_buffer_mutex);
        comms.INPUT_FROM_USER = comms_buffer.INPUT_FROM_USER;
        comms_buffer.INPUT_FROM_USER.clear();
    }

    if (comms_buffer.INTERRUPTED)
    {
        comms.INTERRUPTED = true;
        comms_buffer.INTERRUPTED = false;
    }

    if (comms_buffer.IS_TYPING)
    {
        comms.IS_TYPING = true;
        comms_buffer.IS_TYPING = false;
    }

    if (comms_buffer.EXIT_REQUESTED)
    {
        comms.EXIT_REQUESTED = true;
        comms_buffer.EXIT_REQUESTED = false;
    }

    INTERUPTED.store(false);
}
