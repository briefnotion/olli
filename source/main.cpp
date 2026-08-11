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
 * VOCA (speech-to-text) is a separate Python process; it talks to us only
 * through files under ~/olli_files (see system.key_input.PROPS.path_input).
 */
int main() {
    CLASS_SYSTEM system;

    ollama_system chat;
    SIDETRACK_CLASS sidetrack;
    
    system.setings_vars.load_settings();
    
    chat.PROPS.OLLI_DIERCTORY = system.setings_vars.get_settings_path();
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

    system.audio_control.create(system.setings_vars.get_settings_path());
    system.audio_control.thread_start();

    sidetrack.create(chat.PROPS);
    sidetrack.thread_start();

    //
    system.key_input.PROPS.path_input = system.setings_vars.get_settings_path() / "input";
    system.key_input.PROPS.ENABLED = true;

    //
    std::cout << "\n--- Chat Started (Type 'bye' or 'quit' or 'Goodbye.' to stop) ---\n" << std::endl;
    std::cout << "You: " << std::flush;


    //
    // Main loop: runs until user types 'bye' or 'quit' or 'Goodbye.'
    // Ticks every ~20ms (see the sleep_for at the bottom); nothing here
    // blocks for long except chat.input()'s interrupt-handling branch,
    // which stops an in-flight response before returning (see olla.cpp).
    while (chat.running)
    {
        // 1. Non-blocking check for user input

        // process text from keyboard (or a file dropped by VOCA - see
        // KEYBOARD_INPUT::getNextInteraction in helper_olli.cpp). Sets
        // INTERRUPTED (below) and/or ENTER_PRESSED (read by chat.input()).
        system.key_input.keyboard_input();
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
        if (chat.input(system))
        {
            sidetrack.SIGNALS.CHAT_FINISHED_SIGNAL = true;
            std::cout << "You: " << std::flush;
        }

        // Dispatches any pending tool calls, flushes new text to TTS
        // (write_to_tts), periodically writes history to disk if it
        // changed. See ollama_system::process in olla.cpp.
        chat.process(system.key_input.PROPS.ENABLED);

        // Runs sidetrack's main-thread half of both routines' state
        // machines - see SIDETRACK_CLASS::check's doc comment.
        sidetrack.check(chat);

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::cout << "\n--- Chat Ended ---" << std::endl;

    // Explicit final flush - process()'s reactive save (triggered by a
    // history size change) only runs from inside the loop above, which has
    // already exited by this point.
    chat.save_history();

    // audio_control's thread_stop must actually join before we start
    // tearing this process down (see AUDIO_CONTROL_CLASS::thread_stop for
    // why - the same reasoning as SIDETRACK_CLASS::thread_stop below).
    system.audio_control.thread_stop();
    system.setings_vars.save_settings();

    sidetrack.thread_stop();

    return 0;
}



#endif