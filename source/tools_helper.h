#ifndef tools_helper_h
#define tools_helper_h

#include <string>
#include <chrono>
#include <vector>
#include <fstream>
#include <filesystem>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct TASK_SIMPLE
{
    public:
        std::string TASK_NAME = "";
        std::string TASK_PURPOSE = "";
        std::string TASK_DIRECTORY = "";

        // Optional "DELAY_TOOL_RETURNS: false" header line - defaults true
        // since a stray remote-tool event/result landing mid-script (a
        // timer firing, presence changing) used to get absorbed into this
        // script's own throwaway instance and narrated under its system
        // prompt instead of ever reaching the real conversation - true
        // unless a script has a specific reason to see live returns as it
        // runs, so an unwritten script can't regress into that by default.
        // See TOOL_TASK_RUNNER::handle_tool()'s own use of this
        // (tools.cpp) for where it's actually applied.
        bool delay_tool_returns = true;

        std::vector<std::string> COMMANDS;
        void clear();
};

class TASK_SIMPLE_MANAGER
{
    public:
        std::vector<TASK_SIMPLE> TASK_LIST;

        // Reads every *.task file directly under scripts_dir into TASK_LIST,
        // replacing whatever was loaded before. Called from
        // TOOL_TASK_RUNNER::configure() (tools.cpp) once OLLI_DIRECTORY is
        // known - not from a constructor here, since this class has no way
        // to know the profile's directory on its own.
        void load_all_task(const std::filesystem::path& scripts_dir);
};

// HUE_SCENE/LightState/HUE_LIGHT_CLASS used to live here - moved to
// tools/hue/hue.cpp as part of porting Hue support out to a remote tool
// (see tools/PROTOCOL.md and tools/hue/README.md).

#endif