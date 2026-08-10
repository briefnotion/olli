#ifndef tool_name_h
#define tool_name_h

#include <string>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Forward declaration (matches the other TOOL_* classes in olla.h)
class ollama_system;

/**
 * TOOL_NAME
 * Blank template for a new tool. Copy this file (and tool_name.cpp) to
 * source/, rename NAME to the tool's real name (class, file, guard),
 * fill in the three methods, then wire it up in olla.h / olla.cpp:
 *
 *   1. olla.h    -> add `TOOL_NAME your_member_name;` to ollama_system's
 *                   private members (see TOOL_TIMER for reference).
 *   2. olla.h    -> add a bool to TOOL_PERMISSIONS_CLASS in tools_helper.h
 *                   if this tool should be togglable.
 *   3. olla.cpp  -> ollama_system::open(): register the tool
 *                   `if (TOOL_PERMISSIONS.YOUR_FLAG) your_member_name.register_tool(tools);`
 *   4. olla.cpp  -> handle_instance_tools(): dispatch the tool call by name
 *                   `if (tc.name == "your_tool_name") your_member_name.handle_tool(*this, tc.name, tc.arguments, tc.id);`
 *   5. olla.cpp  -> process(): if using monitor_tool, call it under
 *                   "PART 5: ACTIVE MONITORS"
 *                   `if (TOOL_PERMISSIONS.YOUR_FLAG) your_member_name.monitor_tool(*this);`
 *
 * Delete monitor_tool (declaration + definition + wiring) if the tool has
 * no background/polling behavior — not every tool needs one
 * (see TOOL_GET_CURRENT_TIME or TOOL_WEB_SEARCH for examples without it).
 */
class TOOL_NAME
{
    public:
        /**
         * @brief Registers this tool's function(s) with the model.
         * Build the JSON parameter schema(s) and call add_tool() for each.
         */
        void register_tool(json& tools);

        /**
         * @brief Handles the execution of the tool call.
         * Read args, do the work, then report back via
         * chat.send_tool_result(tc_id, result) and, if the persona should
         * speak about it, chat.integrate_tool_result("", result).
         */
        void handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id);

        /**
         * @brief Background monitor hook, called every loop tick regardless
         * of tool calls. Use for timers, polling, or state sync. Delete if
         * not needed.
         */
        void monitor_tool(ollama_system& chat);
};

#endif
