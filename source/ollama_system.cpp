#include "ollama_system.h"
#include "tool_worker.h"
#include "helper_olli.h"
#include <cmath>

// The Ollama backend: everything here is specific to talking to an Ollama
// server (its /api/chat request shape, its NDJSON stream, its logprobs and
// "think"/"keep_alive"/"format" options). Moved out of the old olla.cpp's
// send() with no change in behaviour - the shared head and tail of send()
// are ai_root::send() (ai_root.cpp).

// Parses one "message" chunk's "tool_calls" array (if present) into out,
// appending to whatever's already there - shared by send()'s streaming and
// non-streaming paths below, which used to each hand-roll this with
// tc["function"].value("name", "") / tc["function"]["arguments"]: plain
// operator[] on a possibly-missing key, which nlohmann::json auto-vivifies
// to null rather than failing loudly, and calling .value() on that null
// throws json::type_error. That throw used to propagate into either the
// streaming callback's blanket 'catch (...) {}' (silently dropping that
// tool call, and every later one in the same chunk, with zero trace) or
// past the non-streaming path's narrower 'catch (const json::parse_error&)'
// (which doesn't even match json::type_error) into chat_thread's own
// outermost catch-all - same silent loss, different route. Confirmed
// exploitable: is_ready_for_tools (tools.cpp) only dispatches when
// last_received.tool_calls is non-empty, so a swallowed parse failure here
// was indistinguishable from "the model chose not to call anything."
// .at()/.value() throughout instead - never auto-vivifies - and a per-entry
// try/catch with an actual log line, so one malformed entry can't take out
// the rest of the array, and a real failure is visible instead of silent.
static void parse_tool_calls(const json& msg_chunk, std::vector<ToolCall>& out)
{
    if (!msg_chunk.contains("tool_calls")) return;

    for (const auto& tc : msg_chunk["tool_calls"]) {
        try {
            json function = tc.at("function");
            out.push_back({
                tc.value("id", ""),
                function.value("name", ""),
                function.value("arguments", json::object())
            });
        } catch (const std::exception& e) {
            std::cerr << "[Error] Skipping malformed tool_calls entry from Ollama (" << e.what() << "): "
                      << tc.dump() << std::endl;
        }
    }
}

// Reads Ollama's "logprobs" array (one {token, logprob, ...} per generated
// token; a sibling of "message" in a response or stream chunk, not inside it)
// and appends each entry to 'out' as a plain probability, exp(logprob).
// Per-entry try/catch, same reasoning as parse_tool_calls() above - one
// malformed entry must not cost the rest of the array, or the reply itself.
static void append_token_probs(const json& logprobs, std::vector<TokenProb>& out)
{
    if (!logprobs.is_array()) return;

    for (const auto& entry : logprobs) {
        try {
            out.push_back({entry.value("token", ""), std::exp(entry.at("logprob").get<double>())});
        } catch (const std::exception&) {
            // skipped - a missing or non-numeric logprob is just no data
        }
    }
}

std::unique_ptr<ai_root> ollama_system::make_sibling() const
{
    return std::make_unique<ollama_system>();
}

void ollama_system::backend_generate(TOOL_WORKER_CLASS* tool_worker, COMMS& comms, const json& response_format, std::string& accumulated_content, std::string& accumulated_thinking)
{
    json messages_json = json::array();
    {
        std::lock_guard<std::mutex> lock(history_mutex);
        for (const auto& msg : history) {
            json m = {{"role", msg.role}, {"content", msg.content}};
            if (!msg.tool_call_id.empty()) m["tool_call_id"] = msg.tool_call_id;
            // Echoed back in the same shape parse_tool_calls() (below)
            // parses it out of Ollama's own responses in - see
            // Message::tool_calls' comment (olla.h) for why this exists:
            // without it, the model's own past tool invocations were
            // invisible in its own context, only ever their results were.
            if (!msg.tool_calls.empty()) {
                json tc_array = json::array();
                for (const auto& tc : msg.tool_calls) {
                    tc_array.push_back({
                        {"id", tc.id},
                        {"type", "function"},
                        {"function", {
                            {"name", tc.name},
                            {"arguments", tc.arguments}
                        }}
                    });
                }
                m["tool_calls"] = tc_array;
            }
            messages_json.push_back(m);
        }
    }

    // Rebuilt fresh every call (see the comment in open()) rather than
    // fixed once at startup, so a remote tool connecting mid-session shows
    // up on the very next request instead of never. tool_worker already
    // hands this back in Ollama's final wrapped schema shape (its own
    // registered_tool_defs, refreshed every tick on its own thread -
    // tool_worker.h/.cpp) - nothing left to convert here. Includes the
    // built-ins too (main.cpp seeds them into tool_worker's own registry at
    // startup via add_manual_tool_def()), so this is only ever empty if
    // tool_worker itself is null.
    tools = tool_worker ? tool_worker->get_registered_tool_defs() : json::array();

    // Ollama's own streaming API is what makes either channel show up live
    // at all - needed whenever EITHER comms.INPUT_FROM_LLM (stream_output)
    // or comms.INPUT_FROM_THINKING (stream_thinking) wants that, not just
    // stream_output alone (see their shared comment, olla.h).
    bool use_streaming_request = PROPS.stream_output || PROPS.stream_thinking;

    json body = {
        {"model", PROPS.model},
        {"messages", messages_json},
        {"stream", use_streaming_request},
        {"think", PROPS.use_thinking},
        {"keep_alive", PROPS.keep_alive_seconds},
        {"options", {
            {"num_ctx", PROPS.num_ctx},
            {"repeat_penalty", PROPS.repeat_penalty},
            //{"temperature", 0}
        }}
    };

    if (!tools.empty()) {
        body["tools"] = tools;
    }

    if (!response_format.empty()) {
        body["format"] = response_format;
    }

    // Asks Ollama to send back the probability of every token it generates
    // (top choice only, no runner-ups) - it is computed anyway, this just
    // stops it being thrown away. See Confidence (olla.h).
    body["logprobs"] = true;

    httplib::Client cli(PROPS.host, PROPS.port);
    cli.set_read_timeout(120);

    bool stream_received_done_flag = false;

    httplib::Headers headers = { {"Content-Type", "application/json"} };
    std::string json_body = body.dump();

    if (use_streaming_request) {
        auto res = cli.Post(
            "/api/chat",
            headers,
            json_body,
            "application/json",
            [&](const char *data, size_t data_length) {
                if (status.interrupt_signal.load()) {
                    return false;
                }

                std::string chunk(data, data_length);
                try {
                    auto j_chunk = json::parse(chunk);

                    if (j_chunk.value("done", false)) {
                        stream_received_done_flag = true;
                    }

                    if (j_chunk.contains("message")) {
                        auto msg_chunk = j_chunk["message"];

                        // Independent ifs, not if/else if - a chunk that
                        // happens to carry both a "thinking" key (even an
                        // empty one) and real "content" used to have its
                        // content silently dropped, since checking key
                        // *presence* (not whether thinking is non-empty)
                        // made the content branch unreachable whenever
                        // "thinking" was present at all. The non-streaming
                        // path below never had this restriction - it reads
                        // both fields independently via .value() - so this
                        // was an inconsistency between the two, not a
                        // deliberate choice.
                        if (msg_chunk.contains("thinking")) {
                            std::string t = msg_chunk["thinking"];
                            accumulated_thinking += t;
                            // accumulated_thinking (-> last_received.thinking)
                            // always gets it regardless - only whether it
                            // also goes to comms live is gated.
                            if (PROPS.stream_thinking) {
                                std::lock_guard<std::mutex> lock(output_buffer_mutex);
                                comms.INPUT_FROM_THINKING.add_to(t);
                            }
                        }
                        if (msg_chunk.contains("content")) {
                            std::string c = msg_chunk["content"];
                            accumulated_content += c;
                            // TTS reads from its own comms_stt_tts copy
                            // (io_worker.cpp), fed from comms.INPUT_FROM_LLM
                            // via exchange() into comms_buffer, then
                            // thread_main()'s own tick copying that into
                            // comms_stt_tts - no separate append needed
                            // here. accumulated_content (-> last_received.
                            // response) always gets it regardless - only
                            // whether it also goes to comms live is gated.
                            if (PROPS.stream_output) {
                                std::lock_guard<std::mutex> lock(output_buffer_mutex);
                                comms.INPUT_FROM_LLM.add_to(c);
                            }
                        }

                        parse_tool_calls(msg_chunk, last_received.tool_calls);
                    }

                    // "logprobs" is a sibling of "message" in a chunk, not inside
                    // it, and carries one entry per token - a chunk is one token
                    // of either thinking or reply text. A chunk with no text of
                    // either kind (a tool call, say) is skipped, so tool-call
                    // tokens never count towards a reply's confidence.
                    if (j_chunk.contains("logprobs") && j_chunk.contains("message")) {
                        const auto& msg_for_probs = j_chunk["message"];
                        if (!msg_for_probs.value("content", std::string()).empty()) {
                            append_token_probs(j_chunk["logprobs"], last_received.response_tokens);
                        } else if (!msg_for_probs.value("thinking", std::string()).empty()) {
                            append_token_probs(j_chunk["logprobs"], last_received.thinking_tokens);
                        }
                    }
                } catch (...) {}
                return true;
            }
        );

        if (status.interrupt_signal.load()) {
            //log("\n[System: Response Interrupted by User]\n");
        } else if (!res || res->status != 200) {
            std::cerr << "\n[Error] Stream failed: " << (res ? std::to_string(res->status) : "Connection error") << std::endl;
        }

        last_received.complete = stream_received_done_flag && !status.interrupt_signal.load();

    } else {
        // Updated non-streaming logic to be as robust as send_task
        auto res = cli.Post("/api/chat", headers, json_body, "application/json");
        if (res && res->status == 200) {
            try {
                auto j_res = json::parse(res->body);
                last_received.complete = j_res.value("done", false);

                if (j_res.contains("message")) {
                    auto msg_obj = j_res["message"];
                    accumulated_content = msg_obj.value("content", "");
                    accumulated_thinking = msg_obj.value("thinking", "");

                    parse_tool_calls(msg_obj, last_received.tool_calls);
                }

                // The whole reply's tokens arrive as one array here, thinking
                // first and then the reply (the streaming path above gets
                // them split per chunk). Tell them apart by walking until the
                // thinking text's length has been used up.
                if (j_res.contains("logprobs")) {
                    std::vector<TokenProb> all_tokens;
                    append_token_probs(j_res["logprobs"], all_tokens);

                    size_t thinking_left = accumulated_thinking.size();
                    for (const TokenProb& t : all_tokens) {
                        if (thinking_left > 0) {
                            last_received.thinking_tokens.push_back(t);
                            thinking_left = (t.token.size() >= thinking_left) ? 0 : thinking_left - t.token.size();
                        } else {
                            last_received.response_tokens.push_back(t);
                        }
                    }
                }
            } catch (const json::parse_error& e) {
                last_received.complete = false;
                std::cerr << "[Error] JSON Parse failed in send: " << e.what() << std::endl;
            }
        } else {
            last_received.complete = false;
            std::cerr << "[Error] Request failed in send: " 
                      << (res ? std::to_string(res->status) : "Connection error") << std::endl;
        }
    }
}

void ollama_system::unload_model()
{
    // /api/generate with no "prompt" and keep_alive: 0 is Ollama's
    // documented way to unload a model on demand, independent of whatever
    // keep_alive value normal requests use (see PROPS.keep_alive_seconds).
    json body = {
        {"model", PROPS.model},
        {"keep_alive", 0}
    };

    httplib::Headers headers = { {"Content-Type", "application/json"} };
    httplib::Client cli(PROPS.host, PROPS.port);
    cli.set_read_timeout(10); // just an unload signal - no generation happens, should return almost instantly
    cli.Post("/api/generate", headers, body.dump(), "application/json");
}
