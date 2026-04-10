#ifndef main_cpp
#define main_cpp

#include "main.h"
#include <atomic>
#include <thread>
#include <filesystem>


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

    system.audio_control.create(system.setings_vars.get_settings_path());
    system.audio_control.thread_start();

    sidetrack.create(chat.PROPS);
    sidetrack.thread_start();

    //
    system.key_input.PROPS.path_input = system.setings_vars.get_settings_path() / "input";
    system.key_input.PROPS.lira_control_file = system.setings_vars.get_settings_path() / "lira_control.json";
    system.key_input.PROPS.ENABLED = true;

    //
    std::cout << "\n--- Chat Started (Type 'bye' or 'quit' or 'Goodbye.' to stop) ---\n" << std::endl;
    std::cout << "You: " << std::flush;


    //
    // Main loop: runs until user types 'bye' or 'quit' or 'Goodbye.'
    while (chat.running) 
    {
        // 1. Non-blocking check for user input

        // process text from keyboard
        system.key_input.keyboard_input();
        if (system.key_input.INTERRUPTED)
        {
            sidetrack.SIGNALS.INTERUPT_SIGNAL = true;
        }

        if (chat.input(system))
        {
            sidetrack.SIGNALS.CHAT_FINISHED_SIGNAL = true;
            std::cout << "You: " << std::flush;
        }

        chat.process(system.key_input.PROPS.ENABLED);

        sidetrack.check(chat);

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::cout << "\n--- Chat Ended ---" << std::endl;

    system.audio_control.thread_stop();
    system.setings_vars.save_settings();

    sidetrack.thread_stop();

    return 0;
}



#endif