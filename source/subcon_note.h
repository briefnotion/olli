#ifndef SUBCON_NOTE_H
#define SUBCON_NOTE_H

// The pure half of subcon's quick note (history slot -3, SIDETRACK_CLASS::run_subcon_note()): reading subcon's
// peek_subcon_notes answer and writing the nudge text. No olli state, no clock, no files - so it can be tested on
// its own (source/tests/subcon_note_test.cpp).

#include <algorithm>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace subcon_note
{
    // Is a tool with this name among the definitions tool_worker::get_registered_tool_defs() returns? Those are in the
    // model-facing shape add_tool() builds - {"type": "function", "function": {"name": ..., ...}} - NOT a bare
    // {"name": ...} (the first version of run_subcon_note() looked for the bare shape, never found the tool, and silently
    // never made a call - found live 2026-11-21). Anything of the wrong shape is skipped, never an error.
    inline bool tool_offered(const nlohmann::json& definitions, const std::string& name)
    {
        if (!definitions.is_array()) return false;
        for (const auto& definition : definitions)
        {
            if (!definition.is_object() || !definition.contains("function") || !definition["function"].is_object()) continue;
            const auto& function = definition["function"];
            if (function.contains("name") && function["name"].is_string() && function["name"].get<std::string>() == name) return true;
        }
        return false;
    }

    struct PEEK
    {
        int pending = -1;                  // -1 = the answer could not be read
        std::vector<std::string> topics;   // task labels, each once, in the order subcon listed them
        bool any_urgent = false;
    };

    // {"pending": 2, "notes": [{"label": "Weather", "urgency": "urgent", ...}, ...]} -> PEEK. Anything else - an error
    // text, the wrong types, not JSON at all - gives pending = -1. Never throws.
    inline PEEK parse_peek(const std::string& response)
    {
        PEEK peek;
        try
        {
            nlohmann::json answer = nlohmann::json::parse(response);
            if (!answer.is_object() || !answer.contains("pending") || !answer["pending"].is_number_integer()) return peek;
            const long long pending = answer["pending"].get<long long>();
            if (pending < 0 || pending > 100000) return peek;
            peek.pending = static_cast<int>(pending);
            if (answer.contains("notes") && answer["notes"].is_array())
            {
                for (const auto& note : answer["notes"])
                {
                    if (!note.is_object()) continue;
                    const nlohmann::json& label = note.contains("label") ? note["label"] : nlohmann::json();
                    if (label.is_string())
                    {
                        std::string text = label.get<std::string>();
                        if (text.size() > 60) text.resize(60);   // a label is a short task name; never let a long one swell the slot
                        if (!text.empty() && std::find(peek.topics.begin(), peek.topics.end(), text) == peek.topics.end()) peek.topics.push_back(text);
                    }
                    if (note.contains("urgency") && note["urgency"].is_string() && note["urgency"].get<std::string>() == "urgent") peek.any_urgent = true;
                }
            }
        }
        catch (...)
        {
            peek = PEEK{};
        }
        return peek;
    }

    // The text of the slot. Only topics and a count - never the facts (those reach the user only through
    // get_subcon_notes, word for word, which also uses the waiting notes up and so removes this slot).
    inline std::string build_nudge(const PEEK& peek)
    {
        std::string topic_list;
        for (const std::string& topic : peek.topics) topic_list += (topic_list.empty() ? "" : ", ") + topic;

        return "Subcon quick note: your background thinking has " + std::to_string(peek.pending) + (peek.pending == 1 ? " thing" : " things") +
               " waiting for the user" + (topic_list.empty() ? std::string() : " (topics: " + topic_list + ")") +
               (peek.any_urgent ? ", and at least one is urgent" : "") + ". " +
               (peek.any_urgent ? "Tell the user soon. " : "") +
               "At the first natural moment - for example right after you answer what they just asked, or when they greet you - call "
               "get_subcon_notes and tell them what it says. Do this once; it uses the waiting notes up. Do not guess what the notes say.";
    }
}

#endif // SUBCON_NOTE_H
