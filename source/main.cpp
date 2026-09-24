#ifndef main_cpp
#define main_cpp

#include "main.h"
#include "remote_tools.h"
#include <atomic>
#include <thread>
#include <filesystem>
#include <csignal>
#include <cstring>
#include <curl/curl.h>
#include <unistd.h>
#include <fcntl.h>
#include <execinfo.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/syscall.h>

namespace {
    void print_usage()
    {
        std::cout << "Usage: olli [name]\n\n"
                      "  name          Use your own settings, history, and scenes, kept\n"
                      "                separate from everyone else's, under\n"
                      "                ~/olli_files_<name> instead of the shared\n"
                      "                ~/olli_files. Case-insensitive. Omit it and you'll\n"
                      "                be prompted for one at startup; pressing Enter with\n"
                      "                nothing typed uses the shared default.\n\n"
                      "  --help, -h    Show this help and exit.\n";
    }

    // Mirrors Settings::get_settings_path()'s logic (helper_olli.cpp) -
    // duplicated in full rather than exposed from there, since this is the
    // one thing the supervisor below needs to know before main_process()
    // (which owns the real Settings object) has even started.
    std::filesystem::path resolve_olli_files_dir(const std::string& profile_name)
    {
        #ifdef _WIN32
            const char* home_dir = std::getenv("USERPROFILE");
        #else
            const char* home_dir = std::getenv("HOME");
        #endif
        std::filesystem::path home = (home_dir != nullptr)
            ? std::filesystem::path(home_dir)
            : std::filesystem::current_path();
        std::string dir_name = profile_name.empty() ? "olli_files" : "olli_files_" + profile_name;
        return home / dir_name;
    }

    // Used for the chat log's speaker label - profile_name is already
    // lower_case()'d elsewhere (for the olli_files_<name> directory), so the
    // label needs its own first-letter capitalization to match "Olli: "'s
    // own capitalization.
    std::string capitalize_first_letter(std::string text)
    {
        if (!text.empty())
            text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
        return text;
    }

    // A persistent, durable record of every restart the supervisor
    // triggers - the std::cerr messages alone only live as long as the
    // terminal/screen session does, which is exactly what left the first
    // two crashes (see TODO.md) with no evidence at all once the screen
    // session was gone. Best-effort: a failure to write here must never
    // itself take down the supervisor.
    void append_crash_log(const std::string& profile_name, const std::string& message)
    {
        try
        {
            std::filesystem::path dir = resolve_olli_files_dir(profile_name);
            std::filesystem::create_directories(dir);

            std::ofstream log_file(dir / "crash_log.txt", std::ios::app);
            if (!log_file) return;

            auto now = std::chrono::system_clock::now();
            std::time_t now_time = std::chrono::system_clock::to_time_t(now);
            std::tm local_tm{};
            localtime_r(&now_time, &local_tm);

            char timestamp[32];
            std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_tm);

            log_file << timestamp << " - " << message << "\n";
        }
        catch (...)
        {
            // Best-effort only.
        }
    }

    // A SIGSEGV/SIGABRT/etc. crash bypasses main_process()'s try/catch
    // entirely (signals aren't C++ exceptions) and, until now, left nothing
    // behind but the supervisor's own "terminated by signal N" line in
    // crash_log.txt - true for every one of the still-undiagnosed crashes in
    // TODO.md's "Unexplained crash" entry. Ubuntu's apport drops core dumps
    // for locally-built binaries like this one (also in TODO.md), so a real
    // core file isn't a reliable fallback here either. This writes an actual
    // backtrace into crash_log.txt itself instead, from inside the crashing
    // process, right as it dies.
    //
    // Raw fd + write()/backtrace_symbols_fd() only, no std::ofstream/iostream
    // and no std::string building - a signal handler can fire with the heap
    // or libc's internal locks in an arbitrary, possibly corrupted state, so
    // it sticks to the small set of calls conventionally treated as safe
    // enough for this (see `man 7 signal-safety`); backtrace_symbols_fd()
    // specifically exists, instead of backtrace_symbols(), to avoid an
    // internal malloc() here. g_crash_log_fd is opened once, in
    // main_process(), long before any crash - never inside the handler.
    int g_crash_log_fd = -1;
    // Which thread (gettid() - async-signal-safe, a plain syscall) is
    // currently inside crash_signal_handler(), or 0 if none. A plain bool
    // here would also silently swallow a second, genuinely DIFFERENT
    // thread's own crash if it fires while the first is still mid-write -
    // confirmed missing (2026-09-23): two SIGABRT crashes the same day
    // this handler was added both got a full backtrace, but a later one
    // (same signal, same general shape) got none at all, just the
    // supervisor's own "exited abnormally" line - consistent with a
    // second thread's own crash hitting the guard and immediately
    // _exit()-ing before the first thread's own write finished. Comparing
    // thread ids keeps the guard's real purpose (a fault recursing inside
    // its OWN handler, e.g. backtrace() itself faulting) while letting a
    // genuinely different thread's crash still get written - the two
    // backtraces can interleave in the log if that happens, which is
    // still strictly better than one vanishing with no trace at all.
    volatile std::sig_atomic_t g_crash_handler_thread = 0;

    void crash_write(const char* text)
    {
        if (g_crash_log_fd >= 0)
        {
            [[maybe_unused]] ssize_t ignored = write(g_crash_log_fd, text, std::strlen(text));
        }
    }

    extern "C" void crash_signal_handler(int sig)
    {
        // gettid() via the raw syscall (not glibc's own wrapper, which
        // isn't guaranteed present on every glibc this might build
        // against) - a plain syscall, async-signal-safe. Only bail out
        // for TRUE self-recursion (this exact thread faulting again
        // inside its own handler, e.g. backtrace() itself faulting) - a
        // different thread's own, genuinely separate crash still gets to
        // write its own backtrace below, see g_crash_handler_thread's own
        // comment for why a plain bool here used to silently lose one.
        std::sig_atomic_t this_thread = static_cast<std::sig_atomic_t>(syscall(SYS_gettid));
        if (g_crash_handler_thread == this_thread)
        {
            _exit(128 + sig);
        }
        g_crash_handler_thread = this_thread;

        crash_write("\n[CRASH] signal ");
        crash_write(strsignal(sig));
        crash_write(" - backtrace:\n");

        void* frames[64];
        int frame_count = backtrace(frames, 64);
        backtrace_symbols_fd(frames, frame_count, g_crash_log_fd);

        crash_write("[CRASH] end of backtrace\n");
        if (g_crash_log_fd >= 0) fsync(g_crash_log_fd);

        // Restore the default disposition and re-raise rather than _exit()
        // here - main()'s supervisor identifies *how* the child died via
        // WIFSIGNALED()/WTERMSIG() on the real termination signal, and that
        // logic (and its "was terminated by signal N" crash_log.txt line)
        // shouldn't have to change just because a backtrace also got written.
        std::signal(sig, SIG_DFL);
        std::raise(sig);
    }

    // Registered once per process (including every supervisor restart, since
    // execv() resets signal dispositions same as it does for SIGPIPE above).
    // backtrace() lazily loads/allocates the unwind machinery on its very
    // first call - doing one throwaway call here, outside of any signal
    // context, means that allocation is already done by the time a real
    // crash needs it from inside the handler.
    void install_crash_handler(const std::string& profile_name)
    {
        std::filesystem::path dir = resolve_olli_files_dir(profile_name);
        std::filesystem::create_directories(dir);
        g_crash_log_fd = open((dir / "crash_log.txt").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);

        void* warmup[1];
        backtrace(warmup, 1);

        for (int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS})
            std::signal(sig, crash_signal_handler);
    }
}

/**
 * The real program body - everything that used to live directly in main()
 * before the crash supervisor (see main() below) was added. Runs inside its
 * own freshly exec()'d process every time; has no idea it's being
 * supervised at all.
 *
 * Three moving pieces, each running on its own background thread:
 *   - chat (ollama_system)       the conversation itself; a chat_thread is
 *                                 spawned per turn (see ollama_system::input)
 *   - sidetrack (SIDETRACK_CLASS) history consolidation + the post-turn
 *                                 "second guess" review - see sidetrack.h
 *                                 for the two-thread design
 *   - io_worker (IO_WORKER_CLASS) keyboard input, voice input/output, and
 *                                 screen drawing - see io_worker.h. Owns
 *                                 text-to-speech/speech-to-text privately
 *                                 (nothing outside io_worker.h/.cpp touches
 *                                 either); io_worker's own thread_main()
 *                                 tick copies chat.comms's own
 *                                 INPUT_FROM_LLM (already relayed in by
 *                                 exchange() below into io_worker's own
 *                                 comms_buffer) into its comms_stt_tts and
 *                                 speaks that via display_with_tts() -
 *                                 sidetrack has no speech path of its own
 *                                 right now (see olla.h's note near
 *                                 output_buffer_mutex). The main loop below
 *                                 only ever talks to io_worker via exchange
 *                                 (chat, chat.comms) once per tick.
 * VOCA (speech-to-text) runs in-process via io_worker (see io_worker.h) -
 * its transcripts are polled and merged into a submission by io_worker's
 * own thread, same as a typed line.
 *
 * crash_restart: true if this run followed a crash (see main()'s
 * --crash-restart marker) - logged once startup completes, so a crash is a
 * visible, findable event instead of silence.
 *
 * debug_crash: DEBUG/TEST ONLY - deliberately segfaults 5 seconds into the
 * main loop, to exercise main()'s restart path on demand instead of waiting
 * for a real crash. Only reachable via --debug-crash; not mentioned in
 * print_usage()'s output.
 *
 * Returns 0 for a clean, intentional exit (typed "bye"/"quit"/"Goodbye.",
 * or Ctrl+C) - anything else (a caught exception here, or not returning at
 * all because the process actually crashed) tells main()'s supervisor to
 * restart.
 */
int main_process(const std::string& profile_name, bool crash_restart, bool debug_crash)
{
    try
    {
        // Writing to a remote tool's socket (source/remote_tools.cpp) after it's
        // closed the connection raises SIGPIPE, whose default disposition kills
        // the whole process - ignoring it here makes write()/send() just return
        // -1 (EPIPE) instead, which the remote-tools code already checks for.
        // Set once, as early as possible, before anything else can touch a
        // socket. Re-done on every restart since a fresh exec()'d process
        // starts with default signal dispositions again.
        // Printed first thing, before any other startup output, so it's
        // always visible in scrollback regardless of which profile or mode
        // follows - this is the __DATE__/__TIME__ of whenever main.cpp
        // itself was last compiled, which is enough to tell two builds
        // apart (e.g. ~/olli's installed build vs. one just rebuilt in
        // build/) without needing a real version number or git-hash
        // plumbing through CMake.
        std::cout << "olli build: " << __DATE__ << " " << __TIME__ << "\n";

        std::signal(SIGPIPE, SIG_IGN);
        install_crash_handler(profile_name);

        // TOOL_WEB_SEARCH (tools.cpp) calls curl_easy_init() directly
        // without ever calling this first - libcurl does its own lazy
        // global init on first use in that case, and its own docs are
        // explicit that path is NOT thread-safe (it can call into other
        // libraries' similarly-unsafe init routines). A real, if narrow,
        // crash risk if two threads ever made their first curl call at the
        // same moment - this process already has more than one running
        // tool-dispatch code (chat_thread, olla.h/.cpp; sidetrack.cpp's own
        // background thread). Doing it once here, before any thread that
        // could touch curl gets spawned, closes that window entirely.
        curl_global_init(CURL_GLOBAL_DEFAULT);

        CLASS_SYSTEM system;
        COMMS comms;
        ollama_system chat;
        SIDETRACK_CLASS sidetrack;
        IO_WORKER_CLASS io_worker; // keyboard input + screen display - see io_worker.h
        TOOL_WORKER_CLASS tool_worker; // remote-tool communications - see tool_worker.h
        SUBCON_WORKER_CLASS subcon_worker; // background/idle reasoning - skeleton only for now, see subcon_worker.h

        // The main chat's real tools_list - the 4 built-ins. Declared here,
        // not owned by 'chat' itself - see process()'s comment in olla.h for
        // why this is a reference parameter rather than living on
        // ollama_system. Remote tools live in tool_worker's own separate
        // tools_list instead (tool_worker.cpp) - this one never holds one.
        std::vector<std::unique_ptr<TOOL_BASE>> tools_list;
        populate_default_tools(tools_list);

        // --- system: profile/settings ---
        system.setings_vars.profile_name = profile_name;
        system.user.name = profile_name;

        system.setings_vars.load_settings();
        std::filesystem::path settings_path = system.setings_vars.get_settings_path();

        // Raw, unfiltered debug log of every message any ollama_system
        // instance ever creates (main chat, sidetrack, task-runner
        // background tasks) - review only, wiped fresh on every startup.
        // See DEBUG_LOG_CLASS::log_message()'s declaration in helper_olli.h for why
        // this exists: consolidate() (sidetrack.cpp) eventually folds old
        // 'tool'/DIRECTOR_NOTE messages (and everything else) into an
        // LLM-written summary in the live history/history.json - this is
        // the one place the full, original wording survives afterward.
        DEBUG_LOG_CLASS::instance().reset(settings_path / "debug_full_history.txt");

        // --- chat ---
        chat.PROPS.OLLI_DIRECTORY = settings_path;
        chat.PROPS.web_search_api_key = system.setings_vars.tool_web_search_apiKey;
        chat.PROPS.use_thinking = false;
        chat.PROPS.model = "qwen3:8b";
        chat.debug_label = "chat";

        // Wiped fresh on every startup too, same reasoning as
        // debug_full_history.txt above - chat.open() (below) loads this via
        // loadHistoryFromJson() if it's still there, carrying forward old
        // conversation turns (including any baked-in canned responses the
        // model repeated enough times to start imitating on its own).
        // Removing it (not truncating) is deliberate: loadHistoryFromJson()
        // already treats a missing file as "start fresh" via its own
        // !file.is_open() check, with no exception/stderr noise, whereas an
        // empty-but-present file would hit its catch block instead.
        std::filesystem::remove(settings_path / "history.json");

        chat.open(tools_list);
        DEBUG_LOG_CLASS::instance().log_event("chat", "instance created");

        if (crash_restart)
        {
            chat.log("[System] Recovered from a previous crash - starting fresh.\n");
        }

        // --- sidetrack ---
        sidetrack.create(chat.PROPS);

        // --- io_worker: keyboard input + screen display ---

        // Flat-text, human-readable transcript (speaker labels, "Olli: " for
        // the assistant) kept independent of history.json's own structured,
        // periodically-rewritten persistence - see
        // OUTPUT_CLASS::append_to_chat_log() in user_io.cpp.
        io_worker.output.chat_log_path = settings_path / "chat_log.txt";
        if (!profile_name.empty())
            io_worker.output.chat_log_user_label = capitalize_first_letter(profile_name);

        // TTS output no longer needs wiring up here - COMMS::audio is gone;
        // IO_WORKER_CLASS::thread_main() (io_worker.cpp) copies chat.comms's
        // own INPUT_FROM_LLM (already relayed in by exchange() into its
        // comms_buffer) into its own comms_stt_tts and speaks that via
        // display_with_tts(). sidetrack's own generated text has no speech
        // path at all right now (sidetrack is being reworked) - see olla.h's
        // note near output_buffer_mutex.
        //
        // io_worker owns text-to-speech/speech-to-text privately (see
        // IO_WORKER_CLASS's class comment, io_worker.h) - create() below
        // just stashes the shared settings path; tts/voca themselves aren't
        // constructed/started until io_worker.thread_start() below actually
        // runs thread_main() (see its own comment for why). Voca's whisper
        // model lives in the shared ~/olli_files/models, not the
        // per-profile directory (see Settings::get_shared_path()).
        io_worker.create(system.setings_vars.get_shared_path());

        io_worker.key_input.PROPS.ENABLED = true;
        // Under ncurses, keyboard_input()'s own raw per-character echo would
        // corrupt the ncurses-controlled screen - the input window renders the
        // typed line itself instead (see display_with_ncurses()).
        io_worker.key_input.PROPS.RAW_ECHO = !USE_NCURSES;

        // Needs chat.open() to have already run - see IO_WORKER_CLASS::
        // thread_main()'s own comment for why its very first tick flushing
        // chat.open()'s startup log depends on that ordering.
        io_worker.thread_start();

        // --- tool_worker: remote-tool communications ---

        // Built-ins are back in tools_list/dispatch_tool_call() (olla.h), but
        // tool_worker's own registry never learns about them on its own -
        // see register_local_tools()'s own comment (tool_worker.h).
        tool_worker.register_local_tools(tools_list, chat);

        // Tell tool_worker who's running olli right now, same info main's
        // old registration code used to send directly (see tools/
        // PROTOCOL.md's "identity" message and TOOL_REMOTE::send_identity()'s
        // comment, remote_tools.h) - before thread_start(), so the very
        // first registration already has it rather than the empty default.
        tool_worker.set_identity(system.user);

        tool_worker.thread_start();

        // One-way copy of chat's own PROPS (subcon_worker.h's own comment) -
        // same model/host/port, so the subconscious always talks to the
        // same LLM the real conversation does. Just a stepping stone, not
        // canon: thread_main() (subcon_worker.cpp) applies its own real
        // overrides (LOAD_SAVE_HISTORY_ON_DISK, OLLI_DIRECTORY, use_thinking)
        // on top of this copy once it opens subcon_llm - nothing here should
        // be assumed final. Must happen after chat.PROPS.OLLI_DIRECTORY is
        // set (above) and before thread_start() below, since thread_main()
        // copies this into its own local ollama_system once, right at the
        // start.
        subcon_worker.PROPS = chat.PROPS;

        subcon_worker.thread_start(); // skeleton only for now - see subcon_worker.h

        // No separate priming call needed here (there used to be one - a
        // one-off get_response()+display() to flush chat.open()'s startup
        // log before the loop's first tick) - IO_WORKER_CLASS::thread_main()
        // does exactly that, every tick, starting with its very first one.
        if (!USE_NCURSES)
        {
            std::cout << "\n--- Chat Started (Type 'bye' or 'quit' or 'Goodbye.' to stop) ---\n" << std::endl;
            std::cout << "You: " << std::flush;
        }

        // DEBUG/TEST ONLY - see this function's doc comment. Measured from
        // here so a deliberate crash exercises a normal, fully-initialized
        // session, not startup itself.
        auto main_process_start_time = std::chrono::steady_clock::now();

        //
        // Main loop: runs until user types 'bye' or 'quit' or 'Goodbye.'
        // Ticks every ~20ms (see the sleep_for at the bottom); nothing here
        // blocks for long except chat.input()'s interrupt-handling branch,
        // which stops an in-flight response before returning (see olla.cpp).
        while (chat.running)
        {
            // Remote-tool registration used to be polled here
            // (system.remote_tools.poll(), tools/PROTOCOL.md) and pushed
            // into tools_list - that whole handshake now happens inside
            // TOOL_WORKER_CLASS::thread_main() itself (tool_worker.cpp),
            // continuously on its own thread rather than once per tick here.

            // Keyboard, voice, and screen drawing all happen entirely on
            // io_worker's own background thread now (see io_worker.h's
            // class comment). This relays whatever it staged this tick
            // (a submitted line, a stop-request, an exit-request) into
            // chat.comms.
            io_worker.exchange(comms, &tool_worker);

            // Ctrl+C - see COMMS::EXIT_REQUESTED's comment (comms.h) for
            // why this needs its own handling instead of a real SIGINT.
            // Checked before anything else this tick since it should win
            // over any in-progress work, same as it would as a real signal.
            if (comms.EXIT_REQUESTED)
            {
                comms.EXIT_REQUESTED = false;
                chat.request_exit();
                continue;
            }

            // Stops an in-flight response if comms.INTERRUPTED, and/or
            // submits comms.INPUT_FROM_USER as a new message if comms.ENTER_PRESSED.
            // Returns true once a full response cycle has completed (see
            // olla.cpp for the exact conditions), at which point we're
            // ready for new input.
            bool response_complete = chat.input(comms, &tool_worker);

            // Dispatches any pending tool calls, flushes new text to TTS
            // (write_to_tts), periodically writes history to disk if it
            // changed. See ollama_system::process in olla.cpp.
            chat.process(io_worker, &system, tools_list, &tool_worker, comms);

            // Runs sidetrack's main-thread half of both routines' state
            // machines - see SIDETRACK_CLASS::check's doc comment.
            sidetrack.check(io_worker, chat, comms, tools_list, &tool_worker, &system);

            //if (sidetrack.SIGNALS.CONTEXT_CLEARED_SIGNAL)
            //{
            //    sidetrack.SIGNALS.CONTEXT_CLEARED_SIGNAL = false;
            //    // A cleared context is a conversation sidetrack considers
            //    // "over" - close the chat log the same way real program exit
            //    // does (see the other close_chat_log() call site below).
            //    // Relayed through comms rather than called directly - the
            //    // worker thread is still running here, and it's the only
            //    // safe caller of output's methods (see COMMS::
            //    // close_chat_log_requested's comment, comms.h).
            //    chat.comms.close_chat_log_requested.store(true);
            //}

            // Drawing (and pulling chat/background-tasks/sidetrack output
            // into it) now happens every io_worker tick, not here - see
            // IO_WORKER_CLASS::thread_main().

            if (response_complete)
            {
                // sidetrack is being reworked - commented out for now.
                //sidetrack.SIGNALS.CHAT_FINISHED_SIGNAL = true;
                // Under ncurses the input window's "> " prompt is always
                // visible, so there's no separate "ready for input" line to
                // print - that's the plain-display()'s equivalent of it.
                if (!USE_NCURSES) std::cout << "You: " << std::flush;
            }

            if (debug_crash && std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - main_process_start_time).count() >= 5)
            {
                // DEBUG/TEST ONLY - see this function's doc comment. A real
                // SIGSEGV (not a thrown exception - the try/catch around this
                // whole function deliberately can't catch this), to test
                // main()'s fork()/execv() restart supervisor end-to-end.
                volatile int* debug_crash_trigger = nullptr;
                *debug_crash_trigger = 1;
            }

            // Once per tick - see comms_busy_dec()'s own comment (comms.h)
            // for why this drains rather than resets: any access that
            // changed comms this tick already had its own comms_busy_inc()
            // call (IO_WORKER_CLASS::exchange(), io_worker.cpp - the only
            // call sites wired in so far), so this just brings it back
            // down by one, letting several increments in the same tick
            // still show up as "busy" for a bit rather than being erased
            // the instant the tick ends.
            comms_busy_dec(comms);

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        // Joins anything still mid-response before it's torn down - a
        // task-runner automation/consult_expert delegation still streaming
        // (chat.background_tasks) or a sidetrack review pass still streaming
        // (SIDETRACK_CHAT_INSTANCE) used to leave its own chat_thread
        // joinable with nothing left to join it once the main loop above
        // stops calling chat.process()/sidetrack.check() - a real, live
        // std::terminate/SIGABRT crash confirmed via a real backtrace. Done
        // before io_worker/tool_worker's own thread_stop() below, while
        // both are still fully alive, in case anything still in flight
        // needs them.
        chat.shutdown_background_tasks();
        sidetrack.shutdown();

        // io_worker owns key_input/output/audio exclusively - its thread
        // must be fully stopped (joined) before anything else touches them
        // again, including the end_ncurses()/close_chat_log() calls right
        // below. This also stops Voca once that join completes (see
        // IO_WORKER_CLASS::thread_stop()'s own comment for why that order
        // matters) - no separate audio thread_stop() call needed anymore.
        // See IO_WORKER_CLASS's class comment (io_worker.h).
        io_worker.thread_stop();
        tool_worker.thread_stop();
        subcon_worker.thread_stop();

        // Hand the real terminal screen back before printing any of the
        // shutdown messages below - otherwise they'd print while ncurses'
        // alternate screen is still up and never actually be seen.
        io_worker.output.end_ncurses();

        std::cout << "\n--- Chat Ended ---" << std::endl;

        // Explicit final flush - process()'s reactive save (triggered by a
        // history size change) only runs from inside the loop above, which has
        // already exited by this point.
        chat.save_history();
        DEBUG_LOG_CLASS::instance().log_event("chat", "instance closed");

        // Archives this run's chat_log.txt into chat_logs/<timestamp>.chat_log.txt
        // - see OUTPUT_CLASS::close_chat_log() in user_io.cpp. The other call
        // site is inside the loop above, when sidetrack's context-clear routine
        // signals it just cleared history. Safe to call directly here (unlike
        // that other site) since io_worker.thread_stop() above already
        // guarantees the worker thread isn't running anymore.
        io_worker.output.close_chat_log();

        // PROPS.keep_alive_seconds (-1 by default) keeps the model loaded in
        // Ollama indefinitely across requests - that's independent of this
        // process, so without this it would stay loaded after olli exits too.
        chat.unload_model();

        system.setings_vars.save_settings();

        // Matches curl_global_init() near the top of this function - safe to
        // call now that every thread that could have touched curl
        // (chat_thread, joined per-turn in the loop above; sidetrack's own
        // thread, just joined by thread_stop()) is done.
        curl_global_cleanup();

        return 0;
    }
    catch (const std::exception& e)
    {
        // Converts what would otherwise be an uncaught-exception
        // std::terminate() (an uncatchable SIGABRT, same as any other
        // crash) into a clean, logged, non-crashing return instead - the
        // fork()/execv() supervisor in main() below still restarts on this
        // non-zero return, it just gets a real reason logged first.
        std::cerr << "\n[FATAL] Uncaught exception in main_process(): " << e.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << "\n[FATAL] Uncaught unknown exception in main_process()." << std::endl;
        return 1;
    }
}

/**
 * Entry point / crash supervisor.
 *
 * Two ways this gets invoked:
 *   1. Normal, top-level: `./olli [name] [--debug-crash]` - resolves the
 *      profile name (argv or interactive prompt), then loops
 *      fork()+execv()ing this same binary as a supervised child, restarting
 *      it if it ever exits abnormally (crashed, or main_process() returned
 *      non-zero) rather than cleanly (returned 0).
 *   2. Supervised child: `./olli --supervised-child <name> [--crash-restart]
 *      [--debug-crash]` - the marker this same loop passes to execv() below.
 *      Skips straight to running main_process() directly, no forking - a
 *      supervised child must never re-enter supervisor mode itself, or every
 *      restart would nest one fork/exec layer deeper than the last.
 *
 * fork() alone (no exec()) would copy the parent's entire memory image into
 * the child, including whatever heap/global state existed at the moment of
 * a crash - if the crash was caused by memory corruption (a buffer overrun,
 * a use-after-free) rather than a clean null-deref, that corruption would
 * ride along into the "fresh" child too, same as it was. execv() replaces
 * the process image entirely instead - a genuinely clean restart, loading
 * the binary fresh, not just a copy of however things were left.
 *
 * Crash-loop protection: tracks how close together restarts happen, not
 * just that they happened - MAX_CONSECUTIVE_CRASHES within
 * CRASH_LOOP_WINDOW_SECONDS of each other gives up rather than spinning
 * forever (e.g. something broken at startup itself, crashing immediately on
 * every single launch); a rare crash months apart just quietly restarts,
 * counter reset, no different from today.
 */
int main(int argc, char* argv[]) {
    // --- Supervised-child path: run main_process() directly, no forking. ---
    if (argc > 1 && std::string(argv[1]) == "--supervised-child")
    {
        bool crash_restart = false;
        bool debug_crash = false;
        std::string name;
        for (int i = 2; i < argc; ++i)
        {
            std::string arg = argv[i];
            if (arg == "--crash-restart") crash_restart = true;
            else if (arg == "--debug-crash") debug_crash = true;
            else name = arg;
        }
        return main_process(name, crash_restart, debug_crash);
    }

    // --- Top-level path: resolve argv, then supervise. ---
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h")
        {
            print_usage();
            return 0;
        }
    }

    // ./olli <name> gives that person their own settings/history/scenes
    // under ~/olli_files_<name> instead of the shared ~/olli_files - see
    // Settings::load_settings(). Without a name on the command line, ask
    // for one; pressing Enter with nothing typed keeps the shared default.
    // Resolved here (not in main_process()) since this prompt needs normal
    // line input, before anything puts the terminal into raw mode.
    bool debug_crash_requested = false;
    std::string profile_name;
    bool name_given = false;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--debug-crash") debug_crash_requested = true;
        else { profile_name = arg; name_given = true; }
    }

    if (!name_given)
    {
        std::cout << "Each name gets its own settings, history, and scenes,\n"
                      "kept separate from everyone else's.\n\n";
        std::cout << "What is your name? (Enter for the shared default) " << std::flush;
        std::getline(std::cin, profile_name);
    }
    // Lower-cased so olli_files_Ron and olli_files_ron can't both exist.
    profile_name = lower_case(trim(profile_name));

    // Inherited across execv() (rlimits survive exec, unlike most other
    // process state), so setting this once here covers every supervised
    // child - without it, a crash today just silently has no core file (see
    // TODO.md's crash-investigation notes). Only raises it if it's
    // currently 0 (disabled) - leaves an operator-configured limit alone.
    rlimit core_limit{};
    if (getrlimit(RLIMIT_CORE, &core_limit) == 0 && core_limit.rlim_cur == 0)
    {
        core_limit.rlim_cur = core_limit.rlim_max;
        setrlimit(RLIMIT_CORE, &core_limit);
    }

    constexpr int MAX_CONSECUTIVE_CRASHES = 3;
    constexpr int CRASH_LOOP_WINDOW_SECONDS = 30;

    int consecutive_crashes = 0;
    auto last_crash_time = std::chrono::steady_clock::time_point{};
    bool crash_restart = false;
    int final_exit_code = 0;

    while (true)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            std::cerr << "[Supervisor] fork() failed - running unsupervised.\n";
            return main_process(profile_name, crash_restart, debug_crash_requested);
        }

        if (pid == 0)
        {
            // Child: re-exec fresh (see this function's doc comment for why
            // execv(), not just falling through to a plain function call).
            std::vector<std::string> args = {argv[0], "--supervised-child", profile_name};
            if (crash_restart) args.push_back("--crash-restart");
            if (debug_crash_requested) args.push_back("--debug-crash");

            std::vector<char*> exec_argv;
            for (auto& a : args) exec_argv.push_back(a.data());
            exec_argv.push_back(nullptr);

            execv(argv[0], exec_argv.data());

            // execv() only returns on failure.
            std::cerr << "[Supervisor] execv() failed - cannot restart.\n";
            _exit(1);
        }

        // Parent: wait for the child, then decide whether to restart.
        int status = 0;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        {
            final_exit_code = 0;
            break; // clean, intentional exit
        }

        auto now = std::chrono::steady_clock::now();
        bool within_window = last_crash_time.time_since_epoch().count() != 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(now - last_crash_time).count() < CRASH_LOOP_WINDOW_SECONDS;
        consecutive_crashes = within_window ? consecutive_crashes + 1 : 1;
        last_crash_time = now;

        std::string crash_description;
        if (WIFSIGNALED(status))
        {
            crash_description = "terminated by signal " + std::to_string(WTERMSIG(status)) +
                " (" + strsignal(WTERMSIG(status)) + ")";
        }
        else
        {
            crash_description = "exited abnormally (code " + std::to_string(WEXITSTATUS(status)) + ")";
        }
        std::cerr << "\n[Supervisor] olli was " << crash_description << ".\n";
        append_crash_log(profile_name, "olli was " + crash_description +
            " (crash " + std::to_string(consecutive_crashes) + "/" + std::to_string(MAX_CONSECUTIVE_CRASHES) +
            " within the last " + std::to_string(CRASH_LOOP_WINDOW_SECONDS) + "s)");

        if (consecutive_crashes >= MAX_CONSECUTIVE_CRASHES)
        {
            std::cerr << "[Supervisor] " << consecutive_crashes << " crashes within "
                      << CRASH_LOOP_WINDOW_SECONDS << "s - not retrying further. "
                      << "Fix the underlying issue before restarting olli.\n";
            append_crash_log(profile_name, std::to_string(consecutive_crashes) +
                " crashes within " + std::to_string(CRASH_LOOP_WINDOW_SECONDS) +
                "s - giving up, not retrying further.");
            final_exit_code = 1;
            break;
        }

        std::cerr << "[Supervisor] Restarting (" << consecutive_crashes << "/"
                  << MAX_CONSECUTIVE_CRASHES << ")...\n";
        crash_restart = true;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return final_exit_code;
}

#endif
