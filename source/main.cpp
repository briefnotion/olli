#ifndef main_cpp
#define main_cpp

#include "main.h"
#include <atomic>
#include <thread>
#include <filesystem>



int main() {
    //CLASS_SYSTEM system;

    ollama_system chat;
    TOOL_SYSTEM_CLASS tools;
    
    Settings setings_vars;
    setings_vars.load_settings();
    tools.web.apiKey = setings_vars.tool_web_search_apiKey;
    tools.hue.set_credentials(setings_vars.tool_hue_lights_bridge_ip, setings_vars.tool_hue_lights_apiKey, (setings_vars.get_settings_path() / "scenes.json").string());

    std::filesystem::create_directories(setings_vars.get_settings_path() / "output");
    std::filesystem::create_directories(setings_vars.get_settings_path() / "input");
    chat.PROPS.path_output = setings_vars.get_settings_path() / "output";
    chat.PROPS.path_history = ".";

    chat.PROPS.use_thinking = false;
    chat.PROPS.model = "qwen3:8b"; 
    chat.open();
    tools.current_time.register_tool(chat);
    tools.timer.register_tool(chat); 
    tools.hue.register_tool(chat);
    tools.thinking.register_tool(chat);
    tools.web.register_tool(chat);
    tools.delegator.register_tool(chat);
    tools.task_runner.register_tool(chat);

    //

    //
    KEYBOARD_INPUT key_input;
    key_input.PROPS.path_input = setings_vars.get_settings_path() / "input";
    key_input.PROPS.lira_control_file = setings_vars.get_settings_path() / "lira_control.json";

    //
    std::cout << "\n--- Chat Started (Type 'bye' or 'quit' or 'Goodbye.' to stop) ---\n" << std::endl;
    std::cout << "You: " << std::flush;

    //

    while (chat.running) 
    {
        // 1. Non-blocking check for user input

        // process text from keyboard
        key_input.keyboard_input();

        chat.input(key_input);
        tools.process(chat, key_input);

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::cout << "\n--- Chat Ended ---" << std::endl;

    setings_vars.save_settings();

    return 0;
}



#endif