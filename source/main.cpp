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
#include <sys/wait.h>
#include <sys/resource.h>

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
 *                                 either); exchange() below fans chat.comms's
 *                                 own INPUT_FROM_LLM out into io_worker's own
 *                                 comms_buffer_audio and speaks that
 *                                 directly - sidetrack has no speech path of
 *                                 its own right now (see olla.h's note near
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
        std::signal(SIGPIPE, SIG_IGN);

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

        // The main chat's real tools_list - the 3 built-ins plus every
        // remote tool that registers over its lifetime (see the remote-tool
        // handshake below). Declared here, not owned by 'chat' itself - see
        // process()'s comment in olla.h for why tools_list moved to a
        // reference parameter rather than living on ollama_system.
        std::vector<std::unique_ptr<TOOL_BASE>> tools_list;
        populate_default_tools(tools_list);

        system.setings_vars.profile_name = profile_name;
        system.user.name = profile_name;

        system.setings_vars.load_settings();
        std::filesystem::path settings_path = system.setings_vars.get_settings_path();

        chat.PROPS.OLLI_DIRECTORY = settings_path;

        // Raw, unfiltered debug log of every message any ollama_system
        // instance ever creates (main chat, sidetrack, task-runner
        // background tasks) - review only, wiped fresh on every startup.
        // See debug_log_message()'s declaration in helper_olli.h for why
        // this exists: consolidate() (sidetrack.cpp) eventually folds old
        // 'tool'/DIRECTOR_NOTE messages (and everything else) into an
        // LLM-written summary in the live history/history.json - this is
        // the one place the full, original wording survives afterward.
        debug_log_reset(settings_path / "debug_full_history.txt");

        // Flat-text, human-readable transcript (speaker labels, "Olli: " for
        // the assistant) kept independent of history.json's own structured,
        // periodically-rewritten persistence - see
        // OUTPUT_CLASS::append_to_chat_log() in user_io.cpp.
        io_worker.output.chat_log_path = settings_path / "chat_log.txt";
        if (!profile_name.empty())
        {
            // profile_name is already lower_case()'d above (for the
            // olli_files_<name> directory) - capitalize just the first letter
            // for the log label, matching "Olli: "'s own capitalization.
            std::string label = profile_name;
            label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
            io_worker.output.chat_log_user_label = label;
        }
        chat.PROPS.web_search_api_key = system.setings_vars.tool_web_search_apiKey;

        chat.PROPS.use_thinking = false;
        chat.PROPS.model = "qwen3:8b";
        chat.debug_label = "chat";
        chat.open(tools_list);
        debug_log_instance_event("chat", "instance created");

        if (crash_restart)
        {
            chat.log("[System] Recovered from a previous crash - starting fresh.\n");
        }

        // TTS output no longer needs wiring up here - COMMS::audio is gone;
        // IO_WORKER_CLASS::exchange() (io_worker.cpp) fans chat.comms's own
        // INPUT_FROM_LLM out into its private comms_buffer_audio and speaks
        // that directly. sidetrack's own generated text has no speech path
        // at all right now (sidetrack is being reworked) - see olla.h's
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

        sidetrack.create(chat.PROPS);

        io_worker.key_input.PROPS.ENABLED = true;
        // Under ncurses, keyboard_input()'s own raw per-character echo would
        // corrupt the ncurses-controlled screen - the input window renders the
        // typed line itself instead (see display_with_ncurses()).
        io_worker.key_input.PROPS.RAW_ECHO = !USE_NCURSES;
        io_worker.thread_start();

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
            // Non-blocking check for a remote tool completing its registration
            // handshake (see system.remote_tools' declaration in system.h and
            // tools/PROTOCOL.md) - if one just did, hand it to chat as a real
            // tool. Scoped to the main chat instance only for now, not
            // background task-runner/jump instances - see PROTOCOL.md's Scope
            // section. Unrelated to IO - stays here rather than moving onto
            // io_worker.
            auto remote_registration = system.remote_tools.poll();
            if (remote_registration.has_value())
            {
                chat.log("[RemoteTools] Registered " +
                    std::to_string(remote_registration->tools.size()) + " tool(s)\n");

                auto remote_tool = std::make_unique<TOOL_REMOTE>(
                    remote_registration->fd, std::move(remote_registration->tools));

                // Tell it who's running olli right now - see tools/PROTOCOL.md's
                // "identity" message and TOOL_REMOTE::send_identity()'s comment
                // (remote_tools.h). Sent once, right after registration - a
                // reconnect re-registers from scratch, so it lands here again
                // naturally rather than needing its own separate trigger.
                remote_tool->send_identity(system.user.name, system.user.full_name, system.user.about);

                tools_list.push_back(std::move(remote_tool));
            }

            // Keyboard, voice, and screen drawing all happen entirely on
            // io_worker's own background thread now (see io_worker.h's
            // class comment). This relays whatever it staged this tick
            // (a submitted line, a stop-request, an exit-request) into
            // chat.comms.
            io_worker.exchange(comms, tools_list);

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
            bool response_complete = chat.input(comms, tools_list);

            // Dispatches any pending tool calls, flushes new text to TTS
            // (write_to_tts), periodically writes history to disk if it
            // changed. See ollama_system::process in olla.cpp.
            chat.process(&system, tools_list, comms, io_worker.key_input.PROPS.ENABLED);

            // Runs sidetrack's main-thread half of both routines' state
            // machines - see SIDETRACK_CLASS::check's doc comment.
            sidetrack.check(chat, comms, tools_list, &system);

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

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        // io_worker owns key_input/output/audio exclusively - its thread
        // must be fully stopped (joined) before anything else touches them
        // again, including the end_ncurses()/close_chat_log() calls right
        // below. This also stops Voca once that join completes (see
        // IO_WORKER_CLASS::thread_stop()'s own comment for why that order
        // matters) - no separate audio thread_stop() call needed anymore.
        // See IO_WORKER_CLASS's class comment (io_worker.h).
        io_worker.thread_stop();

        // Hand the real terminal screen back before printing any of the
        // shutdown messages below - otherwise they'd print while ncurses'
        // alternate screen is still up and never actually be seen.
        io_worker.output.end_ncurses();

        std::cout << "\n--- Chat Ended ---" << std::endl;

        // Explicit final flush - process()'s reactive save (triggered by a
        // history size change) only runs from inside the loop above, which has
        // already exited by this point.
        chat.save_history();
        debug_log_instance_event("chat", "instance closed");

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

        // sidetrack is being reworked - commented out for now.
        //sidetrack.thread_stop();

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
