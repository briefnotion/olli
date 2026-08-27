#ifndef tools_helper_h
#define tools_helper_h

#include <string>
#include <chrono>
#include <vector>
#include <fstream>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct TASK_SIMPLE
{
    public:
        std::string TASK_PHRASE = "";
        std::string TASK_PURPOSE = "";
        std::string TASK_DIRECTORY = "";
        std::vector<std::string> COMMANDS;
        void clear();
};

class TASK_SIMPLE_MANAGER
{
    public:
        std::vector<TASK_SIMPLE> TASK_LIST;
        TASK_SIMPLE_MANAGER();
        void load_all_task();
};

// HUE_SCENE/LightState/HUE_LIGHT_CLASS used to live here - moved to
// tools/hue/hue.cpp as part of porting Hue support out to a remote tool
// (see tools/PROTOCOL.md and tools/hue/README.md).

#endif