#include "gemini_system.h"
#include "tool_worker.h"
#include "helper_olli.h"
#include <curl/curl.h>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <thread>

// The Google Gemini backend: everything here is specific to talking to
// Google's Gemini API (generativelanguage.googleapis.com). Ported from the
// olli_gemi experiment's olla.cpp; history, tools, consolidation and the head
// and tail of send() are ai_root (ai_root.cpp).

// The Gemini API key: <profile>/gemini_key.txt, else $GEMINI_API_KEY. Read on
// every request (a tiny file) so a key can be fixed without restarting, and
// never stored in PROPS, logged, or put anywhere git can see it.
static std::string load_gemini_key(const std::filesystem::path& directory)
{
    std::string key;
    if (!directory.empty()) {
        std::ifstream file(directory / "gemini_key.txt");
        if (file) std::getline(file, key);
    }
    if (key.empty()) {
        const char* env = std::getenv("GEMINI_API_KEY");
        if (env) key = env;
    }
    size_t first = key.find_first_not_of(" \t\r\n");
    size_t last = key.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : key.substr(first, last - first + 1);
}

// Gemini accepts only a subset of JSON Schema. Olli's tool schemas and
// response formats are plain JSON Schema, so this copies only the keys Gemini
// knows (anything else, e.g. "additionalProperties" or "$schema", is a 400),
// turns enum values into strings, and drops "required" names that aren't in
// "properties".
static json gemini_schema(const json& in)
{
    if (!in.is_object()) return in;

    static const std::set<std::string> allowed = {"type", "format", "description", "nullable", "enum", "items",
                                                  "properties", "required", "minimum", "maximum", "minItems", "maxItems"};
    json out = json::object();
    for (auto it = in.begin(); it != in.end(); ++it) {
        const std::string& key = it.key();
        const json& value = it.value();
        if (!allowed.count(key)) continue;

        if (key == "properties" && value.is_object()) {
            json props = json::object();
            for (auto p = value.begin(); p != value.end(); ++p) props[p.key()] = gemini_schema(p.value());
            out[key] = props;
        } else if (key == "items") {
            out[key] = gemini_schema(value);
        } else if (key == "type" && value.is_array()) {
            // ["string", "null"] -> "string" + nullable
            for (const auto& t : value) {
                if (t.is_string() && t.get<std::string>() != "null") { out["type"] = t; break; }
            }
            out["nullable"] = true;
        } else if (key == "enum" && value.is_array()) {
            json values = json::array();
            for (const auto& e : value) values.push_back(e.is_string() ? e : json(e.dump()));
            out[key] = values;
        } else {
            out[key] = value;
        }
    }

    if (out.contains("required") && out["required"].is_array()) {
        json kept = json::array();
        for (const auto& name : out["required"])
            if (name.is_string() && out.contains("properties") && out["properties"].contains(name.get<std::string>())) kept.push_back(name);
        if (kept.empty()) out.erase("required"); else out["required"] = kept;
    }
    return out;
}

// tool_worker hands back Ollama-shaped tool definitions
// ([{"type":"function","function":{name,description,parameters}}, ...]);
// Gemini wants [{"functionDeclarations":[{name,description,parameters}, ...]}].
// A tool with no parameters must leave "parameters" out (an empty object
// schema is rejected).
static json gemini_tools_from_ollama(const json& ollama_tools)
{
    json declarations = json::array();
    if (ollama_tools.is_array()) {
        for (const auto& tool : ollama_tools) {
            const json& f = (tool.is_object() && tool.contains("function")) ? tool["function"] : tool;
            if (!f.is_object() || !f.contains("name") || !f["name"].is_string()) continue;

            json decl = json::object();
            decl["name"] = f["name"];
            decl["description"] = (f.contains("description") && f["description"].is_string()) ? f["description"] : json("");
            if (f.contains("parameters")) {
                json params = gemini_schema(f["parameters"]);
                if (params.contains("properties") && params["properties"].is_object() && !params["properties"].empty())
                    decl["parameters"] = params;
            }
            declarations.push_back(decl);
        }
    }
    json out = json::array();
    if (!declarations.empty()) {
        json group = json::object();
        group["functionDeclarations"] = declarations;
        out.push_back(group);
    }
    return out;
}

static json gemini_text_part(const std::string& text)
{
    json part = json::object();
    part["text"] = text;
    return part;
}

static bool has_visible_text(const std::string& text)
{
    return text.find_first_not_of(" \t\r\n") != std::string::npos;
}

// Turns olli's history into Gemini's "contents" (+ the system prompt).
//  - protected system messages (consolidation_level < 0: the persona, a review
//    pass's own instructions) become systemInstruction; any other system
//    message (DIRECTOR_NOTE prompts, consolidation summaries) becomes a user
//    turn, since Gemini has no system role mid-conversation;
//  - assistant -> "model"; a tool call is sent as a functionCall ONLY when its
//    result exists, and the result is placed in the user turn right after it
//    (Gemini rejects a call without an immediate answer, and olli's results
//    can land later than the messages after them). A call with no result yet
//    is still sent, with a placeholder answer; a result whose call is gone (consolidated away, or a
//    system-injected action) is kept as plain text so the information is not lost;
//  - neighbouring turns with the same role are merged, and the first turn is
//    always a user turn.
static void build_gemini_contents(const std::vector<Message>& history, std::string& system_text, json& contents)
{
    system_text.clear();
    contents = json::array();

    std::map<std::string, size_t> result_index;
    for (size_t i = 0; i < history.size(); ++i)
        if (history[i].role == "tool" && !history[i].tool_call_id.empty() && !result_index.count(history[i].tool_call_id))
            result_index[history[i].tool_call_id] = i;
    std::set<size_t> consumed;

    auto add_turn = [&contents](const char* role, const json& parts) {
        if (!parts.is_array() || parts.empty()) return;
        if (!contents.empty() && contents.back()["role"] == role) {
            for (const auto& part : parts) contents.back()["parts"].push_back(part);
        } else {
            json turn = json::object();
            turn["role"] = role;
            turn["parts"] = parts;
            contents.push_back(turn);
        }
    };
    auto text_turn = [&](const char* role, const std::string& text) {
        if (!has_visible_text(text)) return;
        json parts = json::array();
        parts.push_back(gemini_text_part(text));
        add_turn(role, parts);
    };

    for (size_t i = 0; i < history.size(); ++i) {
        const Message& m = history[i];

        if (m.role == "system") {
            if (m.consolidation_level < 0) {
                if (has_visible_text(m.content)) system_text += (system_text.empty() ? "" : "\n\n") + m.content;
            } else {
                text_turn("user", m.content);
            }
        } else if (m.role == "user") {
            text_turn("user", m.content);
        } else if (m.role == "assistant") {
            json model_parts = json::array();
            json response_parts = json::array();
            if (has_visible_text(m.content)) model_parts.push_back(gemini_text_part(m.content));

            bool any_signature = false;
            for (const ToolCall& tc : m.tool_calls) if (!tc.signature.empty()) any_signature = true;
            bool first_call_done = false;

            for (const ToolCall& tc : m.tool_calls) {
                if (tc.name.empty()) continue;

                // A call is "answered" when its result message exists later in
                // history. One that isn't (a remote tool's result hasn't come
                // back yet, e.g. a task-runner step that already moved on) is
                // STILL sent, with a placeholder answer: Gemini rejects a call
                // with no answer right after it, but leaving the call out made
                // the model forget it had already asked and ask again (seen
                // live: set_hue_light "off" re-issued on nearly every step).
                auto found = tc.id.empty() ? result_index.end() : result_index.find(tc.id);
                bool answered = found != result_index.end() && found->second > i && !consumed.count(found->second);

                json call = json::object();
                call["name"] = tc.name;
                call["args"] = tc.arguments.is_object() ? tc.arguments : json::object();
                if (!tc.id.empty()) call["id"] = tc.id;
                json call_part = json::object();
                call_part["functionCall"] = call;
                if (!tc.signature.empty()) call_part["thoughtSignature"] = tc.signature;
                else if (!any_signature && !first_call_done) call_part["thoughtSignature"] = "skip_thought_signature_validator";
                first_call_done = true;
                model_parts.push_back(call_part);

                json response = json::object();
                response["result"] = answered ? history[found->second].content
                                              : std::string("(no result has come back for this call yet - it was already requested, do not request it again)");
                json answer = json::object();
                answer["name"] = tc.name;
                if (!tc.id.empty()) answer["id"] = tc.id;
                answer["response"] = response;
                json answer_part = json::object();
                answer_part["functionResponse"] = answer;
                response_parts.push_back(answer_part);
                if (answered) consumed.insert(found->second);
            }
            add_turn("model", model_parts);
            add_turn("user", response_parts);
        } else if (m.role == "tool") {
            if (!consumed.count(i)) text_turn("user", "[Tool result] " + m.content);
        }
    }

    if (contents.empty() || contents[0]["role"] != "user") {
        json parts = json::array();
        parts.push_back(gemini_text_part("(earlier conversation not shown)"));
        json turn = json::object();
        turn["role"] = "user";
        turn["parts"] = parts;
        contents.insert(contents.begin(), turn);
    }
}

// One streaming POST to Gemini (server-sent events). Calls on_event() with
// each decoded "data:" JSON object as it arrives. A non-200 reply isn't SSE:
// its body is collected into error_body instead. should_abort() is polled while
// bytes arrive AND while waiting for the first byte (a long think), so an
// interrupt isn't stuck behind the network. Returns the libcurl result code.
struct GeminiStreamState {
    CURL* curl = nullptr;
    std::string line_buffer;
    std::string error_body;
    std::function<void(const json&)>* on_event = nullptr;
    std::function<bool()>* should_abort = nullptr;
    bool aborted = false;
};

static size_t gemini_write_callback(char* data, size_t size, size_t count, void* user)
{
    GeminiStreamState* state = static_cast<GeminiStreamState*>(user);
    size_t length = size * count;
    if ((*state->should_abort)()) { state->aborted = true; return 0; }

    long http_code = 0;
    curl_easy_getinfo(state->curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        if (state->error_body.size() < 8000) state->error_body.append(data, length);
        return length;
    }

    state->line_buffer.append(data, length);
    size_t newline;
    while ((newline = state->line_buffer.find('\n')) != std::string::npos) {
        std::string line = state->line_buffer.substr(0, newline);
        state->line_buffer.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 5, "data:") != 0) continue;
        std::string payload = line.substr(5);
        size_t first = payload.find_first_not_of(' ');
        if (first == std::string::npos || payload.compare(first, 6, "[DONE]") == 0) continue;
        try {
            (*state->on_event)(json::parse(payload.substr(first)));
        } catch (const std::exception& e) {
            std::cerr << "[Error] Skipping unreadable Gemini stream event (" << e.what() << ")" << std::endl;
        }
    }
    return length;
}

static int gemini_progress_callback(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    GeminiStreamState* state = static_cast<GeminiStreamState*>(user);
    if ((*state->should_abort)()) { state->aborted = true; return 1; }
    return 0;
}

static CURLcode gemini_post_stream(const std::string& model, const std::string& api_key, const std::string& body,
                                   std::function<void(const json&)> on_event, std::function<bool()> should_abort,
                                   long& http_code, std::string& curl_error, std::string& error_body)
{
    http_code = 0;
    curl_error.clear();
    error_body.clear();

    CURL* curl = curl_easy_init();
    if (!curl) { curl_error = "could not start libcurl"; return CURLE_FAILED_INIT; }

    GeminiStreamState state;
    state.curl = curl;
    state.on_event = &on_event;
    state.should_abort = &should_abort;

    std::string url = "https://generativelanguage.googleapis.com/v1beta/models/" + model + ":streamGenerateContent?alt=sse";
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("x-goog-api-key: " + api_key).c_str());
    char error_text[CURL_ERROR_SIZE] = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, gemini_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, gemini_progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 240L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_text);

    CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (result != CURLE_OK) curl_error = state.aborted ? "interrupted" : (error_text[0] ? error_text : curl_easy_strerror(result));
    error_body = state.error_body;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}

gemini_system::gemini_system()
{
    // Gemini model names differ from Ollama's; whoever creates this can still
    // override PROPS.model afterwards. Lite: the full flash models allow only
    // 20 requests/day on the free tier (olli makes 2-3 per message), lite
    // allows 500/day and 15/min.
    PROPS.model = "gemini-3.5-flash-lite";
}

std::unique_ptr<ai_root> gemini_system::make_sibling() const
{
    auto sibling = std::make_unique<gemini_system>();
    return sibling;
}

void gemini_system::backend_generate(TOOL_WORKER_CLASS* tool_worker, COMMS& comms, const json& response_format, std::string& accumulated_content, std::string& accumulated_thinking)
{
    // ---- Build the Gemini request from history (see build_gemini_contents) ----
    std::string system_text;
    json contents;
    {
        std::lock_guard<std::mutex> lock(history_mutex);
        build_gemini_contents(history, system_text, contents);
    }

    // Rebuilt fresh every call (see the comment in open()) so a remote tool
    // connecting mid-session shows up on the very next request. A call asking
    // for a structured reply (response_format) is an answer-only call: tools
    // are left out, as Ollama's constrained decoding couldn't call one either.
    tools = tool_worker ? tool_worker->get_registered_tool_defs() : json::array();
    json gemini_tools = response_format.empty() ? gemini_tools_from_ollama(tools) : json::array();

    json body = json::object();
    body["contents"] = contents;
    if (!system_text.empty()) {
        json parts = json::array();
        parts.push_back(gemini_text_part(system_text));
        json instruction = json::object();
        instruction["parts"] = parts;
        body["systemInstruction"] = instruction;
    }
    if (!gemini_tools.empty()) body["tools"] = gemini_tools;

    json generation = json::object();
    json thinking = json::object();
    if (PROPS.use_thinking) thinking["includeThoughts"] = true;   // thought summaries stream as parts flagged "thought"
    else thinking["thinkingLevel"] = "minimal";                    // verified: no thought tokens at all
    generation["thinkingConfig"] = thinking;
    if (!response_format.empty()) {
        generation["responseMimeType"] = "application/json";
        generation["responseSchema"] = gemini_schema(response_format);
    }
    body["generationConfig"] = generation;

    // replace: a bad UTF-8 byte in live text must not throw out of dump() (same lesson as elsewhere in olli)
    std::string json_body = body.dump(-1, ' ', false, json::error_handler_t::replace);

    bool got_finish = false;
    bool anything_received = false;
    unsigned long tool_call_counter = 0;

    // One decoded stream event: thought text -> thinking channel, reply text
    // -> content channel (each only forwarded live when its stream_* flag is
    // on; accumulated either way), functionCall -> last_received.tool_calls.
    auto on_event = [&](const json& event) {
        anything_received = true;

        if (event.contains("promptFeedback") && event["promptFeedback"].is_object() && event["promptFeedback"].contains("blockReason"))
            DEBUG_LOG_CLASS::instance().log_event(debug_label, "gemini blocked the prompt: " + event["promptFeedback"]["blockReason"].dump());

        if (!event.contains("candidates") || !event["candidates"].is_array() || event["candidates"].empty()) return;
        const json& candidate = event["candidates"][0];

        if (candidate.contains("content") && candidate["content"].is_object() && candidate["content"].contains("parts") && candidate["content"]["parts"].is_array()) {
            for (const json& part : candidate["content"]["parts"]) {
                if (part.contains("functionCall") && part["functionCall"].is_object()) {
                    const json& call = part["functionCall"];
                    ToolCall tc;
                    tc.name = (call.contains("name") && call["name"].is_string()) ? call["name"].get<std::string>() : "";
                    tc.arguments = (call.contains("args") && call["args"].is_object()) ? call["args"] : json::object();
                    tc.id = (call.contains("id") && call["id"].is_string()) ? call["id"].get<std::string>() : "";
                    if (tc.id.empty())
                        tc.id = "gemini_call_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(tool_call_counter);
                    ++tool_call_counter;
                    if (part.contains("thoughtSignature") && part["thoughtSignature"].is_string()) tc.signature = part["thoughtSignature"].get<std::string>();
                    last_received.tool_calls.push_back(tc);
                } else if (part.contains("text") && part["text"].is_string()) {
                    std::string text = part["text"].get<std::string>();
                    if (text.empty()) continue;
                    bool is_thought = part.contains("thought") && part["thought"].is_boolean() && part["thought"].get<bool>();
                    if (is_thought) {
                        accumulated_thinking += text;
                        if (PROPS.stream_thinking) {
                            std::lock_guard<std::mutex> lock(output_buffer_mutex);
                            comms.INPUT_FROM_THINKING.add_to(text);
                        }
                    } else {
                        accumulated_content += text;
                        if (PROPS.stream_output) {
                            std::lock_guard<std::mutex> lock(output_buffer_mutex);
                            comms.INPUT_FROM_LLM.add_to(text);
                        }
                    }
                }
            }
        }

        if (candidate.contains("finishReason") && candidate["finishReason"].is_string()) {
            got_finish = true;
            std::string reason = candidate["finishReason"].get<std::string>();
            if (reason != "STOP")
                DEBUG_LOG_CLASS::instance().log_event(debug_label, "gemini finishReason: " + reason);
        }
    };
    auto should_abort = [this]() { return status.interrupt_signal.load(); };

    // ---- Send, with a few patient retries ----
    // The free tier answers 429 (rate limit) and 503 ("high demand") fairly
    // often; both are normal and usually clear in seconds. Retried only while
    // nothing has been shown yet (a half-streamed reply can't be redone), at
    // 3 s, 8 s and 15 s, all on the same model (one fixed model, by design).
    std::string api_key = load_gemini_key(PROPS.OLLI_DIRECTORY);
    std::string failure;
    bool request_ok = false;

    if (api_key.empty()) {
        failure = "no Gemini API key - put it in " + (PROPS.OLLI_DIRECTORY / "gemini_key.txt").string() + " (or set GEMINI_API_KEY)";
    } else {
        for (int attempt = 0; attempt < 4 && !status.interrupt_signal.load(); ++attempt) {
            const std::string& model = PROPS.model;
            // The model name goes into a URL: letters, digits . _ - only.
            if (model.empty() || model.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos) {
                failure = "the model name \"" + model + "\" is not a valid Gemini model name";
                break;
            }

            long http_code = 0;
            std::string curl_error, error_body;
            CURLcode result = gemini_post_stream(model, api_key, json_body, on_event, should_abort, http_code, curl_error, error_body);

            if (result == CURLE_OK && http_code == 200) { request_ok = true; break; }
            if (status.interrupt_signal.load()) break;

            // Pull Google's own message out of the error body when there is one.
            std::string detail = curl_error;
            if (http_code != 0 && http_code != 200) {
                detail = "HTTP " + std::to_string(http_code);
                try {
                    json err = json::parse(error_body);
                    if (err.contains("error") && err["error"].contains("message") && err["error"]["message"].is_string())
                        detail += ": " + err["error"]["message"].get<std::string>().substr(0, 300);
                } catch (...) {}
            }
            failure = "model " + model + " - " + detail;

            bool retryable = (http_code == 429 || http_code == 500 || http_code == 503 || http_code == 0);
            bool already_showing = anything_received || !accumulated_content.empty() || !accumulated_thinking.empty() || !last_received.tool_calls.empty();
            if (!retryable || already_showing || attempt == 3) break;

            DEBUG_LOG_CLASS::instance().log_event(debug_label, "gemini " + detail + " - retrying");
            int wait_ms = (attempt == 0) ? 3000 : (attempt == 1 ? 8000 : 15000);
            for (int waited = 0; waited < wait_ms && !status.interrupt_signal.load(); waited += 200)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    if (!request_ok && !status.interrupt_signal.load()) {
        std::cerr << "\n[Error] Gemini request failed: " << failure << std::endl;
        DEBUG_LOG_CLASS::instance().log_event(debug_label, "gemini request failed: " + failure);
    }
    last_received.complete = request_ok && got_finish && !status.interrupt_signal.load();
}
