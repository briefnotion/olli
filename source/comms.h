#ifndef COMMS_H
#define COMMS_H

#include <string>
#include <mutex>
#include <atomic>

class AUDIO_CONTROL_CLASS; // for COMMS::audio below - see its own comment

// A single shared mutex guarding every ollama_system instance's COMMS.
//
// This MUST be an 'inline' variable (C++17), not 'static' - same reasoning
// as history_mutex (olla.h): a 'static' definition in a header gives every
// translation unit that includes this header its OWN private mutex, so
// different threads touching different instances' COMMS would lock
// unrelated mutexes and never actually exclude one another. 'inline'
// yields one shared instance across all translation units with no
// "multiple definition" linker error.
//
// Deliberately ONE mutex covering every instance's COMMS, not one per
// instance - see COMMS's own class comment for why a private-per-instance
// mutex here specifically would be a mistake even though the buffers
// themselves are per-instance.
inline std::mutex output_buffer_mutex;

/**
 * COMMS
 * Bundles what an ollama_system instance uses to hand output to whatever's
 * consuming it (the screen, TTS, a log) and reach audio output - moved out
 * of olla.h so it can be included/passed around on its own instead of
 * needing the rest of ollama_system along with it.
 *
 * Each ollama_system instance (the main chat, background tasks,
 * sidetrack's own SIDETRACK_CHAT_INSTANCE) owns its OWN COMMS - the four
 * text buffers are per-instance, same as before this move (audio is a
 * pointer, so multiple instances' COMMS can point at the same physical
 * speaker - see its own comment below). output_buffer_mutex
 * above is the one exception: it's deliberately shared across every
 * instance's COMMS rather than being a member here, for the same reason
 * history_mutex is shared across every instance's history - one coarse
 * lock is simpler to reason about correctly than a private mutex per
 * instance, which would silently fail to exclude anything.
 *
 * response_buffer / thinking_buffer / log_buffer are streamed into
 * incrementally by whoever's producing them, then drained (read + cleared)
 * by whoever's consuming them, under output_buffer_mutex - see
 * OUTPUT_CLASS::get_response() (user_io.cpp) and SIDETRACK_CLASS::
 * pull_output() (sidetrack.cpp) for the two existing consumers.
 * tts_buffer follows the same append/drain shape but is NOT locked at
 * every touch today (see ollama_system::write_to_tts(), olla.cpp) - this
 * is a pure move, so that's preserved exactly as it was, not "fixed" here.
 */
class COMMS
{
    public:
        std::string response_buffer = "";
        std::string thinking_buffer = "";
        std::string tts_buffer = "";
        std::string log_buffer = "";

        // Appends to log_buffer under output_buffer_mutex - the one place
        // that lock actually gets taken for it, so call sites (tool
        // handlers, etc.) don't each need their own lock_guard.
        void log(const std::string& text);

        // Where this COMMS's spoken output goes, if anywhere - nullptr
        // until explicitly set (see write_to_tts(), olla.cpp, which
        // no-ops on nullptr same as before this existed). Used to be a
        // single process-wide global (g_audio_control) instead of living
        // here - moved onto COMMS so a subprogram/task that only ever
        // receives a COMMS (not the rest of ollama_system, not main.cpp's
        // globals) still has a real, self-contained way to reach audio
        // output, and so a future different COMMS (e.g. a remote web-page
        // session with no local speaker) can simply leave this nullptr
        // instead of being forced to share the one process-wide output.
        // AUDIO_CONTROL_CLASS is already internally thread-safe (owns its
        // own mutex-guarded queue and background thread), so pointing
        // multiple instances' COMMS at the same one - which is still the
        // normal case, since there's only one physical speaker - needs no
        // extra locking here.
        AUDIO_CONTROL_CLASS* audio = nullptr;

        // --------------------------------------------------------------
        // Input-direction signals - set by IO_WORKER_CLASS (io_worker.h/
        // .cpp), relayed here via its exchange(), consumed by
        // ollama_system::input() (olla.cpp) and main.cpp's own loop. Not
        // protected by output_buffer_mutex above - IO_WORKER_CLASS's own
        // INTERUPTED/PROCESSING lock covers the handoff into these
        // fields instead (see its class comment). Once a field lands
        // here, it's the CONSUMING side's job to clear it when actually
        // acted on - exchange() only ever sets these, never clears them.
        //
        // Deliberately not named "interrupt"/"INTERRUPTED" - that name
        // is already taken by IO_WORKER_CLASS's own lock signal
        // (WORKER_THREAD_CLASS's INTERUPTED/signal_interrupt()), which
        // means something entirely different (a threading primitive, not
        // this domain-level "stop what you're doing" signal).
        // --------------------------------------------------------------
        bool send = false;             // a line is ready to submit
        std::string submitted_line;    // valid when send == true
        bool stop_requested = false;   // abort in-flight generation/speech
        bool exit_requested = false;   // Ctrl+C - shut olli down
        // --------------------------------------------------------------

        // Opposite direction from the block above: set by main.cpp (main
        // thread) when sidetrack's context-clear routine fires mid-loop,
        // consumed by IO_WORKER_CLASS::thread_main() (its own thread),
        // which is the only safe caller of output.close_chat_log() while
        // the worker thread is still running - see IO_WORKER_CLASS's
        // class comment. Not part of exchange()'s relay - both sides
        // touch this field directly, so it's atomic instead (same
        // reasoning as KEYBOARD_INPUT_PROPERTIES::ENABLED, user_io.h).
        std::atomic<bool> close_chat_log_requested{false};
};

#endif
