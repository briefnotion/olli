#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct whisper_context;

// Offline wake-word + speech-to-text.
//
// Everything runs on-device: mic capture via PortAudio, transcription via a
// vendored whisper.cpp (ggml-quantized model, CPU inference). No network
// access at any point after the model file is on disk.
//
// Mirrors the shape of lira_cpp's TextToSpeech: a small class wrapping the
// chosen engine, background worker threads, and a synchronous state query
// surface (isAwake/isBusy) for the caller to poll or react to via callbacks.
class Voca {
public:
    // Called on a background thread - keep these short (they run on the
    // transcription worker thread, blocking it until they return). An
    // exception thrown here is caught internally and logged rather than
    // propagating out - it won't crash the host program.
    struct Callbacks {
        // Normal transcript heard while awake and not paused.
        std::function<void(const std::string& text)> onTranscript;
        // Woken by a wake phrase (e.g. "hey voca"). trigger is the phrase matched.
        std::function<void(const std::string& trigger)> onWake;
        // Went back to sleep, either via a sleep phrase or the auto-sleep timeout.
        std::function<void()> onSleep;
        // An interrupt phrase (e.g. "stop talking") was heard while paused/busy.
        std::function<void(const std::string& text)> onInterrupt;
    };

    // modelPath: path to a ggml-format whisper model (e.g. models/ggml-small.en.bin).
    explicit Voca(std::string modelPath, Callbacks callbacks = {});
    ~Voca();

    Voca(const Voca&) = delete;
    Voca& operator=(const Voca&) = delete;

    // Loads the model and starts mic capture + transcription worker threads.
    // Returns false (and logs to stderr) if the model failed to load or the
    // mic couldn't be opened. No-op (returns true) if already running.
    bool start();

    // Stops capture/transcription and joins the worker threads.
    void stop();

    bool isRunning() const;

    // Pauses command handling: audio is still captured and transcribed (so
    // interrupt phrases still work), but wake/sleep/normal-transcript
    // handling is suspended. Mirrors DEF_VOCA_PAUSE in olli's audio_control -
    // callers pause Voca while TTS is speaking so it doesn't hear its own voice.
    void pause();
    void resume();
    bool isBusy() const;

    bool isAwake() const;

    // Force awake/asleep directly, bypassing wake-word detection (e.g. a
    // manual override). Does not fire onWake/onSleep.
    void wake();
    void sleep();

    // Poll-style alternative to Callbacks::onTranscript, for callers who'd
    // rather check-and-fetch than register a callback. Every transcript
    // still reaches onTranscript too (if set) - this is an additional way
    // to get the same text, not a replacement. Scoped to transcript text
    // only: wake/sleep/interrupt are discrete state-change events better
    // handled immediately via their callbacks, not polled for.
    //
    // Each call to getNextLine() returns exactly one complete utterance
    // (Voca only finalizes a phrase after ~1.2s of silence, so two queued
    // lines means two distinct things were said, not one split in half).
    // Drain with textAvailable() in a loop and handle each line as its own
    // turn - don't concatenate them, that would merge separate requests
    // into one confusing blob:
    //
    //   while (person_talking.textAvailable()) {
    //       std::string words = person_talking.getNextLine();
    //       // handle this one utterance, then loop for the next
    //   }
    bool textAvailable() const;
    std::string getNextLine();

private:
    void captureLoop();
    void transcribeLoop();
    void handleTranscript(const std::string& text);
    void checkAutoSleep();
    void setAwake(bool awake);
    // Plays a feedback beep and briefly mutes capture (see muteUntilSec_)
    // so the mic doesn't hear the beep and queue it as a "phrase" itself.
    void beep(double frequencyHz, double durationSec);
    // Delivers a transcript both ways at once: fires onTranscript (if set)
    // and queues it for textAvailable()/getNextLine().
    void pushTranscript(const std::string& text);

    std::string modelPath_;
    Callbacks callbacks_;
    whisper_context* ctx_ = nullptr;

    std::atomic<bool> running_{false};
    std::atomic<bool> awake_{false};
    std::atomic<bool> busy_{false};
    std::atomic<bool> shuttingDown_{false};

    std::thread captureThread_;
    std::thread transcribeThread_;

    std::deque<std::vector<float>> phraseQueue_;
    std::mutex queueMutex_;
    std::condition_variable queueCv_;

    std::mutex lastSpeechMutex_;
    double lastSpeechMonotonicSec_ = 0.0;

    mutable std::mutex textQueueMutex_;
    std::deque<std::string> textQueue_;

    // Monotonic time (seconds) until which captureLoop ignores mic input -
    // set by beep() so the wake/sleep confirmation tone doesn't get heard
    // and transcribed as a phrase of its own.
    std::atomic<double> muteUntilSec_{0.0};
};
