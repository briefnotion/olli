#ifndef tool_name_cpp
#define tool_name_cpp

#include "tool_name.h"
#include "olla.h"

/**
 * @brief Registers this tool's function(s) with the model.
 */
void TOOL_NAME::register_tool(json& tools) {
    // Define the parameter schema for the tool. Mirror the shape used by
    // the other tools (type/properties/description/required). Example:
    //
    // json example_params = {
    //     {"type", "object"},
    //     {"properties", {
    //         {"some_arg", {{"type", "string"}, {"description", "What this argument is for"}}}
    //     }},
    //     {"required", {"some_arg"}}
    // };
    //
    // add_tool(tools, "your_tool_name", "What this tool does.", example_params);
}

/**
 * @brief Handles the execution of the tool call.
 */
void TOOL_NAME::handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id) {
    if (name == "your_tool_name") {
        // 1. Read arguments
        // std::string some_arg = args["some_arg"];

        // 2. Do the work
        std::string res = "";

        // 3. Silent history record
        chat.send_tool_result(tc_id, res);

        // 4. Persona integration (optional - lets the assistant speak about the result)
        chat.integrate_tool_result("", res);
    }
    else {
        std::string error_msg = "Error: Tool '" + name + "' not recognized by TOOL_NAME.";
        chat.send_tool_result(tc_id, error_msg);
    }
}

/**
 * @brief Background monitor hook. Delete if not needed.
 */
void TOOL_NAME::monitor_tool(ollama_system& chat) {
    if (!chat.is_processing) {
        // Check/update background state here, e.g. poll for a completed
        // event and call chat.integrate_tool_result("", event_msg) to
        // have the persona announce it.
    }
}

#endif
