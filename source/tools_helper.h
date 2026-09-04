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