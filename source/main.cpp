#ifndef main_cpp
#define main_cpp

#include "main.h"
#include <atomic>
#include <thread>
#include <filesystem>

/**
 * Entry point and the whole program's single-threaded main loop.
 *
 * Three moving pieces, two of which run on their own background thread:
 *   - chat (ollama_system)       the conversation itself; a chat_thread is
 *                                 spawned per turn (see ollama_system::input)
 *   - sidetrack (SIDETRACK_CLASS) history consolidation + the post-turn
 *                                 "second guess" review - see sidetrack.h
 *                                 for the two-thread design
 *   - system.audio_control        owns text-to-speech (audio_control.h);
 *                                 speech is driven by g_audio_control
 *                                 (declared in olla.h), a single shared
 *                                 pointer any ollama_system instance can
 *                                 speak through, not just the main one
 * VOCA (speech-to-text) runs in-process via system.audio_control (see
 * audio_control.h/voca.hpp) - its transcripts are drained each loop tick
 * below and fed into system.key_input the same way a typed line would be.
 */
int main(int argc, char* argv[]) {
    // Checked before anything else: argv[1] is otherwise taken as-is for
    // profile_name below (see the comment there), so a typo'd flag would
    // silently become a brand-new profile instead of failing loudly.
    if (argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        std::cout << "Usage: olli [name]\n\n"
                      "  name          Use your own settings, history, and scenes, kept\n"
                      "                separate from everyone else's, under\n"
                      "                ~/olli_files_<name> instead of the shared\n"
                      "                ~/olli_files. Case-insensitive. Omit it and you'll\n"
                      "                be prompted for one at startup; pressing Enter with\n"
                      "                nothing typed uses the shared default.\n\n"
                      "  --help, -h    Show this help and exit.\n";
        return 0;
    }

    // ./olli <name> gives that person their own settings/history/scenes
    // under ~/olli_files_<name> instead of the shared ~/olli_files - see
    // Settings::load_settings(). Without a name on the command line, ask
    // for one; pressing Enter with nothing typed keeps the shared default.
    // Resolved before CLASS_SYSTEM (below) puts the terminal into raw mode
    // for keyboard input, since this prompt needs normal line input.
    std::string profile_name;
    if (argc > 1) {
        profile_name = argv[1];
    } else {
        std::cout << "Each name gets its own settings, history, and scenes,\n"
                      "kept separate from everyone else's.\n\n";
        std::cout << "What is your name? (Enter for the shared default) " << std::flush;
        std::getline(std::cin, profile_name);
    }
    // Lower-cased so olli_files_Ron and olli_files_ron can't both exist.
    profile_name = lower_case(trim(profile_name));

    CLASS_SYSTEM system;

    ollama_system chat;
    SIDETRACK_CLASS sidetrack;

    system.setings_vars.profile_name = profile_name;

    system.setings_vars.load_settings();
    std::filesystem::path settings_path = system.setings_vars.get_settings_path();

    chat.PROPS.OLLI_DIRECTORY = settings_path;
    // Flat-text, human-readable transcript (speaker labels, "Olli: " for
    // the assistant) kept independent of history.json's own structured,
    // periodically-rewritten persistence - see
    // OUTPUT_CLASS::append_to_chat_log() in user_io.cpp.
    system.output.chat_log_path = settings_path / "chat_log.txt";
    if (!profile_name.empty())
    {
        // profile_name is already lower_case()'d above (for the
        // olli_files_<name> directory) - capitalize just the first letter
        // for the log label, matching "Olli: "'s own capitalization.
        std::string label = profile_name;
        label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
        system.output.chat_log_user_label = label;
    }
    chat.PROPS.web_search_api_key = system.setings_vars.tool_web_search_apiKey;
    chat.PROPS.hue_ip = system.setings_vars.tool_hue_lights_bridge_ip;
    chat.PROPS.hue_key = system.setings_vars.tool_hue_lights_apiKey;

    chat.TOOL_PERMISSIONS.CURRENT_TIME = true;
    chat.TOOL_PERMISSIONS.TIMER = true;
    chat.TOOL_PERMISSIONS.HUE = true;
    chat.TOOL_PERMISSIONS.THINKING = true;
    chat.TOOL_PERMISSIONS.WEB = true;
    chat.TOOL_PERMISSIONS.DELEGATOR = true;
    chat.TOOL_PERMISSIONS.TASK_RUNNER = true;

    chat.PROPS.use_thinking = false;
    chat.PROPS.model = "qwen3:8b";
    chat.open();
    // Wires up TTS output (see g_audio_control's declaration in olla.h).
    // Every ollama_system instance shares this one pointer - including
    // sidetrack's SIDETRACK_CHAT_INSTANCE - so its own generated text
    // (the "second guess" review) can be spoken too, not just chat's.
    g_audio_control = &system.audio_control;

    // Voca's whisper model lives in the shared ~/olli_files/models, not the
    // per-profile directory (see Settings::get_shared_path()).
    system.audio_control.create(system.setings_vars.get_shared_path());
    system.audio_control.thread_start();

    sidetrack.create(chat.PROPS);
    sidetrack.thread_start();

    //
    system.key_input.PROPS.ENABLED = true;
    // Under ncurses, keyboard_input()'s own raw per-character echo would
    // corrupt the ncurses-controlled screen - the input window renders the
    // typed line itself instead (see display_with_ncurses()).
    system.key_input.PROPS.RAW_ECHO = !USE_NCURSES;

    // Flush anything chat.open() already logged (e.g. "[System] Connecting
    // to...") before the prompt appears - otherwise it sits in log_buffer
    // until the loop's first tick and prints after "You: " instead of
    // before it. Under ncurses this is also what triggers its lazy init,
    // so the very first thing on screen is already the real windowed
    // layout instead of a plain-terminal banner that ncurses would
    // immediately paper over anyway.
    system.output.get_response(chat);
    if (USE_NCURSES)
    {
        system.output.display_with_ncurses(system.key_input);
    }
    else
    {
        system.output.display();

        std::cout << "\n--- Chat Started (Type 'bye' or 'quit' or 'Goodbye.' to stop) ---\n" << std::endl;
        std::cout << "You: " << std::flush;
    }


    //
    // Main loop: runs until user types 'bye' or 'quit' or 'Goodbye.'
    // Ticks every ~20ms (see the sleep_for at the bottom); nothing here
    // blocks for long except chat.input()'s interrupt-handling branch,
    // which stops an in-flight response before returning (see olla.cpp).
    while (chat.running)
    {
        // 1. Non-blocking check for user input

        // process text from the keyboard. Sets INTERRUPTED (below) and/or
        // ENTER_PRESSED (read by chat.input()).
        system.key_input.keyboard_input();

        // Ctrl+C - see EXIT_REQUESTED's comment in user_io.h for why this
        // needs its own handling instead of a real SIGINT. Checked before
        // anything else this tick since it should win over any in-progress
        // work, same as it would as a real signal.
        if (system.key_input.EXIT_REQUESTED)
        {
            system.key_input.EXIT_REQUESTED = false;
            chat.request_exit();
            continue;
        }

        // Pop at most one pending voice event this tick (see
        // AUDIO_CONTROL_CLASS::popVocaEvent) and feed it into key_input the
        // same way a typed line would arrive. Deliberately not a "drain
        // everything" loop: chat.input() below only acts on this single
        // LINE/ENTER_PRESSED snapshot, and it starts an async chat turn -
        // if two voice events were said back-to-back, draining both here
        // would silently lose the first one (overwritten before
        // chat.input() ever sees it) instead of giving each its own turn.
        // One per tick means any backlog drains naturally over the next
        // few ~20ms iterations instead.
        //
        // An empty event's text means "interrupt only" (e.g. "stop talking"
        // heard while TTS is speaking) - INTERRUPTED still fires below, but
        // nothing gets submitted as a new chat message.
        VOCA_EVENT voca_event;
        if (system.audio_control.popVocaEvent(voca_event))
        {
            if (!voca_event.text.empty())
            {
                system.key_input.LINE = voca_event.text;
                system.output.user_input += voca_event.text + "\n";
                system.key_input.ENTER_PRESSED = true;
            }
            system.key_input.INTERRUPTED = true;
        }

        if (system.key_input.INTERRUPTED)
        {
            // Same trigger, two independent things to stop: the sidetrack
            // signal aborts an in-flight second-guess review (see
            // SIDETRACK_CLASS::check in sidetrack.cpp - it's the one that
            // actually calls .stop() on that instance, since this thread
            // isn't the one blocked inside its LLM call). stop_speaking()
            // separately kills whatever's currently playing/queued in TTS.
            // Neither of these submits the input the user just gave -
            // that's chat.input()'s job, right below.
            sidetrack.SIGNALS.INTERUPT_SIGNAL = true;
            system.audio_control.stop_speaking();
        }

        // Stops an in-flight response if INTERRUPTED, and/or submits
        // whatever's in LINE as a new message if ENTER_PRESSED. Returns
        // true once a full response cycle has completed (see olla.cpp for
        // the exact conditions), at which point we're ready for new input.
        // chat_thread is already joined by the time this returns true, so
        // response_buffer holds everything send() ever wrote, including its
        // final trailing newline - the get_response()/display() call below
        // must run before "You: " prints, or that last newline (and any
        // last few streamed characters) would print after the prompt
        // instead of before it.
        bool response_complete = chat.input(system);

        // Dispatches any pending tool calls, flushes new text to TTS
        // (write_to_tts), periodically writes history to disk if it
        // changed. See ollama_system::process in olla.cpp.
        chat.process(system.key_input.PROPS.ENABLED);

        // Runs sidetrack's main-thread half of both routines' state
        // machines - see SIDETRACK_CLASS::check's doc comment.
        sidetrack.check(chat);

        if (sidetrack.SIGNALS.CONTEXT_CLEARED_SIGNAL)
        {
            sidetrack.SIGNALS.CONTEXT_CLEARED_SIGNAL = false;
            // A cleared context is a conversation sidetrack considers
            // "over" - close the chat log the same way real program exit
            // does (see the other close_chat_log() call site below).
            system.output.close_chat_log();
        }

        // Pull whatever chat, its background tasks (task-runner automations),
        // and sidetrack's second-guess review each streamed since the last
        // tick, then show everything accumulated this tick - see
        // OUTPUT_CLASS in user_io.h/.cpp.
        system.output.get_response(chat);
        chat.pull_background_output(system.output);
        sidetrack.pull_output(system.output);
        if (USE_NCURSES)
        {
            system.output.display_with_ncurses(system.key_input);
        }
        else
        {
            system.output.display();
        }

        if (response_complete)
        {
            sidetrack.SIGNALS.CHAT_FINISHED_SIGNAL = true;
            // Under ncurses the input window's "> " prompt is always
            // visible, so there's no separate "ready for input" line to
            // print - that's the plain-display()'s equivalent of it.
            if (!USE_NCURSES) std::cout << "You: " << std::flush;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // Hand the real terminal screen back before printing any of the
    // shutdown messages below - otherwise they'd print while ncurses'
    // alternate screen is still up and never actually be seen.
    system.output.end_ncurses();

    std::cout << "\n--- Chat Ended ---" << std::endl;

    // Explicit final flush - process()'s reactive save (triggered by a
    // history size change) only runs from inside the loop above, which has
    // already exited by this point.
    chat.save_history();

    // Archives this run's chat_log.txt into chat_logs/<timestamp>.chat_log.txt
    // - see OUTPUT_CLASS::close_chat_log() in user_io.cpp. The other call
    // site is inside the loop above, when sidetrack's context-clear routine
    // signals it just cleared history.
    system.output.close_chat_log();

    // PROPS.keep_alive_seconds (-1 by default) keeps the model loaded in
    // Ollama indefinitely across requests - that's independent of this
    // process, so without this it would stay loaded after olli exits too.
    chat.unload_model();

    // audio_control's thread_stop must actually join before we start
    // tearing this process down (see AUDIO_CONTROL_CLASS::thread_stop for
    // why - the same reasoning as SIDETRACK_CLASS::thread_stop below).
    system.audio_control.thread_stop();
    system.setings_vars.save_settings();

    sidetrack.thread_stop();

    return 0;
}



#endif