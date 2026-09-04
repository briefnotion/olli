#ifndef tools_helper_cpp
#define tools_helper_cpp

#include "tools_helper.h"

// TIMER_SIMPLE used to live here - moved to tools/clock/clock.cpp along
// with TOOL_TIMER itself (see the comment where that class used to be in
// source/tools.cpp).

// ----

void TASK_SIMPLE::clear()
{
    TASK_NAME = "";
    COMMANDS.clear();
}

// ----

namespace {
    // "KEY: value" -> value, only if line actually starts with that key.
    bool parse_header_line(const std::string& line, const std::string& key, std::string& value)
    {
        std::string prefix = key + ": ";
        if (line.rfind(prefix, 0) != 0) return false;
        value = line.substr(prefix.size());
        return true;
    }
}

void TASK_SIMPLE_MANAGER::load_all_task(const std::filesystem::path& scripts_dir)
{
    TASK_LIST.clear();

    if (!std::filesystem::exists(scripts_dir))
    {
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(scripts_dir))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".task")
        {
            continue;
        }

        std::ifstream file(entry.path());
        if (!file.is_open())
        {
            continue;
        }

        TASK_SIMPLE tmp_task;
        tmp_task.clear();

        bool in_commands = false;
        std::string line;

        while (std::getline(file, line))
        {
            if (!in_commands)
            {
                if (line == "---")
                {
                    in_commands = true;
                    continue;
                }

                std::string value;
                if (parse_header_line(line, "NAME", value))
                    tmp_task.TASK_NAME = value;
                else if (parse_header_line(line, "PURPOSE", value))
                    tmp_task.TASK_PURPOSE = value;
                else if (parse_header_line(line, "DIRECTORY", value))
                    tmp_task.TASK_DIRECTORY = value;
            }
            else if (!line.empty())
            {
                tmp_task.COMMANDS.push_back(line);
            }
        }

        if (!tmp_task.TASK_NAME.empty())
        {
            TASK_LIST.push_back(tmp_task);
        }
    }
}

// HUE_SCENE's to_json/from_json and every HUE_LIGHT_CLASS method used to
// live here - moved to tools/hue/hue.cpp along with the rest of Hue
// support (see the comment where HUE_LIGHT_CLASS used to be declared in
// tools_helper.h, and tools/PROTOCOL.md).

// ----

#endif