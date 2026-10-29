// A minimal blocking client for Ollama's /api/chat, over libcurl. Only ever
// called from job_worker's thread (a model request is slow and blocking -
// seconds to minutes - and must never run on the thread that services the
// network). Request/response shapes follow source/olla.cpp's send(): the
// same body fields (model, messages, stream:false, think, keep_alive,
// options.num_ctx/repeat_penalty, tools, format) and the same reply fields
// (message.content / .thinking / .tool_calls, done).

#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <string>
#include <vector>

// Defaults match olli's own (source/olla.h OLLAMA_SYSTEM_PROPERTIES, and
// chat.PROPS.model in main.cpp), as subcon used to inherit them from the
// real chat's PROPS.
struct OLLAMA_SETTINGS {
    std::string host = "localhost";
    int port = 11434;
    std::string model = "qwen3:8b";
    int num_ctx = 8192;
    double repeat_penalty = 1.3;
    int keep_alive_seconds = -1;
    // Subcon always reasoned with Qwen3's thinking on (subcon_worker.cpp:
    // "the subconscious wants its own reasoning regardless of [the chat's
    // setting]") - kept, but switchable in the settings file.
    bool think = true;
};

struct OLLAMA_TOOL_CALL {
    std::string name;
    nlohmann::json arguments = nlohmann::json::object();
};

struct OLLAMA_REPLY {
    bool ok = false;          // transport + HTTP 200 + parseable reply + done
    std::string error;        // why not, when !ok
    std::string content;
    std::string thinking;
    nlohmann::json message;   // the raw assistant message, for echoing back into the conversation
    std::vector<OLLAMA_TOOL_CALL> tool_calls;
};

// tools: Ollama's wrapped schema ([{"type":"function","function":{...}}]) or
// null/empty for none. format: a JSON schema for structured output, or
// null/empty for free text. 'abort' is polled while the request is in
// flight (libcurl's progress callback), so setting it cuts a long model
// call short in well under a second - that is what makes shutdown prompt.
OLLAMA_REPLY ollama_chat(const OLLAMA_SETTINGS& settings, const nlohmann::json& messages,
                         const nlohmann::json& tools, const nlohmann::json& format,
                         const std::atomic<bool>& abort);
