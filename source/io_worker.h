#ifndef IO_WORKER_H
#define IO_WORKER_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "threading.h"
#include "user_io.h" // KEYBOARD_INPUT, OUTPUT_CLASS, COMMS (via comms.h)

class ollama_system;
class TOOL_BASE;

#define DEF_VOCA_SLEEP  0
#define DEF_VOCA_PAUSE  1
#define DEF_VOCA_LISTEN 2

/**
 * @struct VOCA_EVENT
 * @brief One piece of voice input handed off from Voca's background
 * transcription thread to the main loop (see IO_WORKER_CLASS::popVocaEvent()).
 */
struct VOCA_EVENT {
    // Text to submit as chat input. Empty for an interrupt-only event (e.g.
    // "stop talking" heard while TTS is speaking) - the caller should still
    // treat the event as an interrupt, just not submit anything new.
    std::string text;

    // Set (instead of text) for a wake/sleep status notification (see
    // onWake/onSleep in IO_WORKER_CLASS::create(), io_worker.cpp) - a log
    // line for the caller to display, not a transcript and not an
    // interrupt. Mutually exclusive with text; the caller checks this first.
    std::string status_message;
};

// Offline text-to-speech via the local espeak-ng + aplay binaries.
// No network access, no model downloads - everything runs on-device.
//
// Only ever declared as IO_WORKER_CLASS's own private `tts` member below -
// nothing outside this file touches TextToSpeech directly (see
// IO_WORKER_CLASS::speak()/stop_speaking()).
class TextToSpeech {
public:
    explicit TextToSpeech(std::string voice = "en-us", int wordsPerMinute = 170);
    ~TextToSpeech();

    TextToSpeech(const TextToSpeech&) = delete;
    TextToSpeech& operator=(const TextToSpeech&) = delete;

    // Synthesizes and plays text, blocking until playback finishes or stop() is called.
    bool speak(const std::string& text);

    // Queues text to be spoken on a background worker thread and returns immediately.
    // If nothing is currently speaking, playback starts right away. If something is
    // already speaking, this text is appended and plays after everything queued ahead
    // of it. Does not interrupt speech already in progress - call stop() for that.
    void speakAsync(const std::string& text);

    // Interrupts speech currently playing and clears anything queued via speakAsync().
    // If called while text is still being synthesized (before playback starts), that
    // utterance is dropped once synthesis finishes rather than being played.
    void stop();

    bool isSpeaking() const;

    void setVoice(const std::string& voice);
    void setSpeed(int wordsPerMinute);

private:
    bool synthesize(const std::string& text, const std::string& wavPath);
    bool playback(const std::string& wavPath);
    void workerLoop();

    std::string voice_;
    int speed_;
    std::atomic<bool> speaking_{false};
    std::atomic<int> playerPid_{-1};
    std::atomic<bool> stopRequested_{false};
    std::mutex speakMutex_;

    std::deque<std::string> queue_;
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    bool shuttingDown_ = false;
    std::thread workerThread_;
};

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
//
// Only ever declared as IO_WORKER_CLASS's own private `voca` member below -
// nothing outside this file touches Voca directly.
class Voca {
public:
    // Called on a background thread - keep these short (they run on the
    // transcription worker thread, blocking it until they return). An
    // exception thrown here is caught internally and logged rather than
    // propagating out - it won't crash the host program.
    struct Callbacks {
        // Normal transcript heard while awake and not paused.
        std::function<void(const std::string& text)> onTranscript;
        // Woken by a wake phrase (e.g. "hey olli"). trigger is the phrase matched.
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
    // handling is suspended. Mirrors DEF_VOCA_PAUSE above - callers pause
    // Voca while TTS is speaking so it doesn't hear its own voice.
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

struct IO_WORKER_CLASS_PROPERTIES
{
    double INTERVAL = 20; // ms, matches olli's original main-loop tick rate

    // ================================================================
    // WARNING - READ BEFORE CHANGING THE DEFAULT BELOW.
    //
    // BLOCKING = true: exchange() spins until PROCESSING clears before
    // touching the passed-in COMMS. thread_main()'s per-tick work here
    // (keyboard read, voice poll, draining comms into the screen, an
    // ncurses draw) is all fast/non-blocking, so this stall is brief -
    // unlike SIDETRACK_CLASS's multi-second LLM calls, this is safe to
    // leave on.
    //
    // BLOCKING = false: exchange() does NOT wait - but it also does NOT
    // relay anything. Whatever was staged this tick is simply not handed
    // off, and stays staged in comms_buffer for the next exchange() call
    // instead (unlike WORKER_THREAD_CLASS's original EXCHANGE_DUMMY,
    // nothing is lost here - see IO_WORKER_CLASS's class comment on
    // comms_buffer/comms).
    // ================================================================
    bool BLOCKING = true;
};

/**
 * IO_WORKER_CLASS
 *
 * Owns everything that talks to the user: keyboard input, voice input, text-
 * to-speech output, and the ncurses/plain-terminal display. Runs on its own
 * background thread (thread_main(), via WORKER_THREAD_CLASS's shape - see
 * templates/worker_thread.h/.cpp, which this was copied and adapted from) so
 * none of that interleaves with chat/model logic on the main thread.
 *
 * TextToSpeech and Voca (declared above) are owned exclusively here, as the
 * private `tts`/`voca` members below - nothing outside this file ever
 * touches either directly. Other subsystems (chat, sidetrack) that need to
 * speak reach through IO_WORKER_CLASS's own speak()/stop_speaking() (see
 * COMMS::audio, comms.h, which now points at an IO_WORKER_CLASS instead of
 * a speech class directly). Both are constructed right before thread_main()
 * sets RUN = true, and torn down right after its while(RUN) loop exits -
 * live for exactly this worker's own running window, not this object's
 * whole lifetime - see thread_main()'s own comment. This worker's own
 * thread_main() below also folds in what used to be a separate
 * AUDIO_CONTROL_CLASS background thread (adjust_audio_files() - pausing/
 * resuming Voca while TTS speaks): that work now rides this thread's
 * existing ~20ms tick instead of its own 500ms one, so nothing is lost by
 * not having a second thread for it.
 *
 * KEYBOARD_INPUT and OUTPUT_CLASS (user_io.h/.cpp) have zero built-in
 * thread-safety of their own - raw termios manipulation on STDIN_FILENO,
 * unlocked plain fields, and ncurses itself is single-thread-only. So they
 * are OWNED here, directly, as members - not borrowed from CLASS_SYSTEM -
 * and this worker is their sole caller for their entire lifetime. Declared
 * in this order (key_input before output) to match CLASS_SYSTEM's old
 * member order: OUTPUT_CLASS's destructor comment documents relying on
 * being destroyed before KEYBOARD_INPUT's termios-restoring destructor
 * runs, which C++'s reverse-declaration-order destruction still gives for
 * free as long as this order is preserved.
 *
 * comms (COMMS, comms.h) is the ONLY thing that crosses the exchange()
 * boundary - not key_input/output themselves. `comms_buffer` below is this
 * worker's own local COMMS, touched only by thread_main() (its own
 * thread). exchange() relays both directions once per main-thread tick,
 * each field its own simple copy-then-clear if: INPUT_FROM_LLM/
 * INPUT_FROM_THINKING/INPUT_FROM_SYSTEM flow comms -> comms_buffer (under
 * output_buffer_mutex, same as OUTPUT_CLASS::get_response()); ENTER_PRESSED/
 * INPUT_FROM_USER/INTERRUPTED/IS_TYPING/EXIT_REQUESTED flow comms_buffer ->
 * comms (not protected by output_buffer_mutex - IO_WORKER_CLASS's own
 * INTERUPTED/PROCESSING lock covers this handoff instead). Either way, the
 * DEST side is cleared later by whoever actually consumes it, not by
 * exchange() itself.
 *
 * chat is not reachable from this object at all, in any form - not stored,
 * not passed to thread_start()/thread_main()/exchange(). Only comms
 * crosses the exchange() boundary; the two things that used to reach chat
 * directly from here - pulling background-task output, and calling
 * register_tool() to build tool_names - have been dropped/reworked to go
 * through comms and TOOL_BASE::tool_functions instead (see each one's own
 * comment).
 */
class IO_WORKER_CLASS
{
    protected:
        THREADING_INFO THREAD_CONTROL;
        std::atomic<bool> INTERUPTED{false};
        std::atomic<bool> PROCESSING{false};

        // Just the names of whatever's in the caller's tools_list (see
        // exchange()'s comment in the .cpp), for the right-side tools panel
        // (OUTPUT_CLASS::display_with_ncurses()). Copied fresh every
        // exchange() call, on the main thread, while
        // thread_main() is confirmed not running (same PROCESSING-wait
        // synchronization exchange() already does for `comms_buffer`) - this
        // worker's own thread only ever reads it, never writes it, so no
        // separate lock is needed for that side.
        std::vector<std::string> tool_names;

    private:
        // This worker's own local COMMS - see this class's own comment
        // (comms_buffer/comms) for what it's for and how exchange() drains
        // it into the real comms (owned by main_process(), not chat) once
        // per main-thread tick. Drained by
        // thread_main()'s own screen-drawing step (see its own comment) -
        // NOT the same copy TTS reads from (see comms_buffer_audio below);
        // each has its own independent drain pace, so a single shared copy
        // would have the two race to steal chunks of the same text.
        COMMS comms_buffer;

        // A second, independent copy of the same INPUT_FROM_LLM/THINKING/
        // SYSTEM text comms_buffer gets - see exchange()'s own comment for
        // how both get filled from one drain of the real comms. This one
        // is TTS's own to read/clear at its own pace, separate from
        // whatever comms_buffer still has pending for the screen.
        COMMS comms_buffer_audio;

        // Live mirror of key_input.LINE, refreshed every tick (see
        // thread_main()'s own comment) - purely for display_with_ncurses()
        // to show what's currently being typed, as it's typed. Separate
        // from comms_buffer.INPUT_FROM_USER, which only holds a line once
        // actually submitted (ENTER_PRESSED) and is cleared by exchange()
        // once relayed - this one is never cleared, just overwritten.
        std::string input_from_user_echo;

        // Owns text-to-speech (via TextToSpeech) and speech-to-text (via
        // Voca) in-process, and coordinates them: while TTS is speaking,
        // Voca is paused so it doesn't hear olli's own voice; once speech
        // stops, Voca resumes listening (see adjust_audio_files()). Both
        // are null outside thread_main()'s own RUN window - constructed
        // right before RUN = true, torn down right after while(RUN) exits
        // - see thread_main()'s own comment. speak()/stop_speaking()/
        // adjust_audio_files()/popVocaEvent() all no-op safely on null,
        // same as any other not-yet-available audio path.
        std::unique_ptr<TextToSpeech> tts;
        bool tts_was_speaking = false; // last-seen isSpeaking(), to detect start/stop transitions

        std::unique_ptr<Voca> voca;
        std::filesystem::path audio_settings_path;

        std::mutex voca_events_mutex;
        std::deque<VOCA_EVENT> voca_events;

        // Pauses/resumes voca on a tts.isSpeaking() transition - called
        // once per thread_main() tick (see its own comment above for why
        // this used to be a separate 500ms-tick thread and no longer is).
        void adjust_audio_files();

        // Pops the next pending voice event, if any - called from
        // thread_main() to feed transcripts into comms_buffer the same way
        // a typed line would. Returns false if nothing is pending.
        bool popVocaEvent(VOCA_EVENT& out);

        IO_WORKER_CLASS_PROPERTIES PROPS;

        bool RUN = false;

    public:
        KEYBOARD_INPUT key_input;
        OUTPUT_CLASS output;

        ~IO_WORKER_CLASS();

        // Stores audio_settings_path for later use - does NOT construct
        // tts/voca itself. See thread_main()'s own comment for why that's
        // deferred to its own RUN window instead.
        void create(const std::filesystem::path& audio_shared_path);

        // Queues text to be spoken (see TextToSpeech::speakAsync). This and
        // stop_speaking() below are the only way anything outside this file
        // reaches tts/voca. No-ops before thread_main() has constructed tts
        // (i.e. before thread_start(), or after thread_stop()) - same as
        // any other not-yet-available audio path.
        void speak(const std::string& text);

        // Interrupts speech in progress and clears anything queued.
        void stop_speaking();

        // Forces Voca directly into a state, bypassing wake-word detection
        // (e.g. a manual override from a chat tool - see olla.cpp).
        void VOCA_manual_set(int Command);

        // No parameters - thread_main() only ever touches this object's own
        // comms_buffer/comms_buffer_audio, never chat directly (see this
        // class's own comment). SIDETRACK_CLASS used to be threaded through
        // here too - dropped now that sidetrack is being reworked; nothing
        // here reaches it anymore.
        void thread_start();
        void thread_stop();

        // Runs on the background thread - see thread_start()'s comment for
        // why it takes no parameters.
        void thread_main();

        // Runs on the MAIN/owner thread - call once per its own loop tick,
        // passing the real comms (see this class's own comment for why
        // that's the only thing that crosses this boundary) and the main
        // chat's own tools_list (see process()'s comment in olla.h for why
        // that's a reference parameter, not owned by ollama_system) -
        // copies each tool's own registered names (TOOL_BASE::
        // tool_functions) into tool_names for the ncurses tools panel.
        void exchange(COMMS& comms, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list);
};

#endif
