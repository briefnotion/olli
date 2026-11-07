#ifndef ai_systems_h
#define ai_systems_h

// Shared data types for every AI backend (ai_root today, gemini_system
// next, whatever comes after). Nothing in here knows or cares which backend
// produced or will consume it - history entries, tool calls, the result of one
// send(), and the status summary. Moved here verbatim from olla.h; no
// behaviour changed.

#include <string>
#include <vector>
#include <map>
#include <atomic>
#include <sstream>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Declared before Message - Message::tool_calls (below) holds a vector of
// these, and needs the type (and its own JSON (de)serialization) already
// available.
struct ToolCall {
    std::string id;
    std::string name;
    json arguments;

    // .value() throughout, not .at() - unlike Message's own to_json/
    // from_json below, there's no legacy history.json to stay compatible
    // with here (this type is new), but a malformed/unexpected entry
    // should still degrade to empty fields rather than throw and take an
    // entire Message (and everything after it in a from_json array walk)
    // down with it.
    friend void to_json(json& j, const ToolCall& tc) {
        j = json{
            {"id", tc.id},
            {"name", tc.name},
            {"arguments", tc.arguments}
        };
    }

    friend void from_json(const json& j, ToolCall& tc) {
        tc.id = j.value("id", "");
        tc.name = j.value("name", "");
        tc.arguments = j.value("arguments", json::object());
    }
};

// How sure the model was of its own words, summarized from Ollama's logprobs
// (send(), olla.cpp). Only filled for an assistant reply's content tokens -
// not thinking, not tool calls. A probability here is exp(logprob), so 0.65
// means 65%. It is the model's own certainty about each word it picked, not
// a measure of whether the statement is true. Declared before Message for the
// same reason ToolCall is.
struct Confidence {
    bool   present    = false; // false = no data (user/system/tool messages, old history, tool-call-only replies, structured-output replies)
    int    tokens     = 0;     // how many reply tokens this covers
    double mean       = 0.0;   // average probability, 0.0-1.0
    double lowest     = 0.0;   // the single least-sure token, 0.0-1.0
    int    below_half = 0;     // tokens the model gave under 50%

    // .value() throughout, same reasoning as ToolCall above - and every
    // history.json saved before this existed simply has no "confidence" key.
    friend void to_json(json& j, const Confidence& c) {
        j = json{
            {"present", c.present},
            {"tokens", c.tokens},
            {"mean", c.mean},
            {"lowest", c.lowest},
            {"below_half", c.below_half}
        };
    }

    friend void from_json(const json& j, Confidence& c) {
        c.present = j.value("present", false);
        c.tokens = j.value("tokens", 0);
        c.mean = j.value("mean", 0.0);
        c.lowest = j.value("lowest", 0.0);
        c.below_half = j.value("below_half", 0);
    }
};

// One generated token and the probability the model gave it (0.0-1.0).
struct TokenProb {
    std::string token;
    double probability = 0.0;
};

/**
 * @brief Structure representing a single chat message in the history.
 */
struct Message {
    std::string role = "";
    std::string content = "";
    std::string tool_call_id = "";
    int consolidation_level = 0;

    // Summary of the model's confidence in this message's words - see
    // Confidence above. Only an assistant reply that went through send()
    // has it; everything else (and any history saved before this existed)
    // is present == false.
    Confidence confidence;

    // The assistant's own tool_calls request for this message, if any -
    // see ai_root::send()'s comment on why this is recorded now
    // (it used to not exist anywhere, even transiently: only the tool's
    // *result* and a persona-narrated version of it ever made it into
    // history, never the actual structured call the model itself made).
    // Empty for every message that isn't an assistant turn that called a
    // tool.
    std::vector<ToolCall> tool_calls;

    // Helper to convert a Message object to a JSON object
    // This allows nlohmann::json to handle the struct automatically
    friend void to_json(json& j, const Message& m) {
        j = json{
            {"role", m.role},
            {"content", m.content},
            {"tool_call_id", m.tool_call_id},
            {"consolidation_level", m.consolidation_level},
            {"tool_calls", m.tool_calls}
        };
        // Only written when there is something to say - keeps every other
        // message in history.json exactly as it was.
        if (m.confidence.present) j["confidence"] = m.confidence;
    }

    // Helper to convert a JSON object back into a Message object
    friend void from_json(const json& j, Message& m) {
        j.at("role").get_to(m.role);
        j.at("content").get_to(m.content);
        j.at("tool_call_id").get_to(m.tool_call_id);
        j.at("consolidation_level").get_to(m.consolidation_level);
        // .value(), not .at() - every history.json saved before this field
        // existed is missing this key entirely; .at() would throw loading
        // any of them. This is the one field here that must tolerate
        // being absent.
        m.tool_calls = j.value("tool_calls", json::array()).get<std::vector<ToolCall>>();
        // Same reasoning: absent in every history.json saved before it existed.
        m.confidence = j.value("confidence", json::object()).get<Confidence>();
    }
};

// A three-way answer: yes, no, or maybe. Filled in by send() (olla.cpp) only
// when it was called with YES_NO_MAYBE_FORMAT as its response_format; for any
// other call it stays 0. Read it as:
//     if (last_received.answer.value == 1) { /* yes */ }
//     else if (last_received.answer.value == -1) { /* no */ }
//     else { /* maybe */ }
struct YesNoMaybe {
    int value = 0; // 1 = yes, -1 = no, 0 = maybe (also what you get if the reply couldn't be read)
};

// The response_format that asks for one of those three words - pass it to
// send() and last_received.answer is filled in. Defined in olla.cpp.
extern const json YES_NO_MAYBE_FORMAT;

struct ChatResult {
    std::string response;
    std::string thinking;
    std::vector<ToolCall> tool_calls;
    bool complete = false;

    // Per-token probabilities from Ollama's logprobs, split the same way
    // response/thinking are. Not saved anywhere - for whatever wants the
    // exact token (e.g. a yes/no answer) while this result is still current.
    std::vector<TokenProb> response_tokens;
    std::vector<TokenProb> thinking_tokens;

    // Summary of response_tokens - what send() copies onto the assistant
    // Message it pushes to history.
    Confidence confidence;

    // See YesNoMaybe above. Not saved anywhere, reset at the start of every send().
    YesNoMaybe answer;
};

/**
 * Simple data struct to hold metrics.
 */
struct ollama_system_status {
    bool is_active = false;
    // The interrupt flag. Set this to true from another thread to stop 'send'
    std::atomic<bool> interrupt_signal{false};

    int total_messages = 0;
    std::map<int, int> level_counts;
    int max_level = 0;

    // Helper for easy printing
    std::string to_string() const {
        if (total_messages == 0) return "History Empty";
        std::stringstream ss;
        ss << "Total: " << total_messages << " | ";
        for (int i = 0; i <= max_level; ++i) {
            auto it = level_counts.find(i);
            int count = (it != level_counts.end()) ? it->second : 0;
            ss << "L" << i << ": " << count;
            if (i < max_level) ss << ", ";
        }
        return ss.str();
    }
};

#endif
