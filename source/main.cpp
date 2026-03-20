#ifndef main_cpp
#define main_cpp

#include "main.h"
#include <atomic>
#include <thread>
#include <filesystem>


int main() {
    //CLASS_SYSTEM system;

    ollama_system chat;
    
    Settings setings_vars;
    setings_vars.load_settings();
    std::filesystem::create_directories(setings_vars.get_settings_path() / "output");
    std::filesystem::create_directories(setings_vars.get_settings_path() / "input");
    chat.PROPS.web_search_api_key = setings_vars.tool_web_search_apiKey;
    chat.PROPS.hue_ip = setings_vars.tool_hue_lights_bridge_ip;
    chat.PROPS.hue_key = setings_vars.tool_hue_lights_apiKey;
    chat.PROPS.hue_path = (setings_vars.get_settings_path() / "scenes.json").string(); 
    chat.PROPS.path_output = setings_vars.get_settings_path() / "output";
    chat.PROPS.path_history = ".";

    chat.PROPS.use_thinking = false;
    chat.PROPS.model = "qwen3:8b"; 
    chat.open();

    AUDIO_CONTROL_CLASS audio_control(setings_vars.get_settings_path());
    audio_control.thread_start();

    //
    KEYBOARD_INPUT key_input;
    key_input.PROPS.path_input = setings_vars.get_settings_path() / "input";
    key_input.PROPS.lira_control_file = setings_vars.get_settings_path() / "lira_control.json";

    //
    std::cout << "\n--- Chat Started (Type 'bye' or 'quit' or 'Goodbye.' to stop) ---\n" << std::endl;
    std::cout << "You: " << std::flush;

    //
    // Main loop: runs until user types 'bye' or 'quit' or 'Goodbye.'
    while (chat.running) 
    {
        // 1. Non-blocking check for user input

        // process text from keyboard
        key_input.keyboard_input();

        chat.input(key_input);
        chat.process();
        chat.consolidate_check(key_input);

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::cout << "\n--- Chat Ended ---" << std::endl;

    audio_control.thread_stop();
    setings_vars.save_settings();

    return 0;
}



#endif