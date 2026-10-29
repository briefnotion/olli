#include "ollama_client.hpp"

#include <curl/curl.h>

using json = nlohmann::json;

namespace {
    // A cold model load plus a thinking-mode answer on a small GPU can take
    // minutes; olli's own read timeout is 120s *per read* (httplib), and with
    // stream:false the whole answer is one read, so this is the same
    // ceiling made explicit and a little more generous.
    constexpr long REQUEST_TIMEOUT_SECONDS = 240;
    constexpr long CONNECT_TIMEOUT_SECONDS = 5;

    size_t write_to_string(void* contents, size_t size, size_t nmemb, void* userp)
    {
        static_cast<std::string*>(userp)->append(static_cast<char*>(contents), size * nmemb);
        return size * nmemb;
    }

    // Returning non-zero from the progress callback makes libcurl abort the
    // transfer with CURLE_ABORTED_BY_CALLBACK. Called roughly once a second
    // while waiting, which is the shutdown latency bound for a model call.
    int progress_check(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
    {
        return static_cast<const std::atomic<bool>*>(clientp)->load() ? 1 : 0;
    }
}

OLLAMA_REPLY ollama_chat(const OLLAMA_SETTINGS& settings, const json& messages, const json& tools,
                         const json& format, const std::atomic<bool>& abort)
{
    OLLAMA_REPLY reply;

    json body = {
        {"model", settings.model},
        {"messages", messages},
        {"stream", false},
        {"think", settings.think},
        {"keep_alive", settings.keep_alive_seconds},
        {"options", {
            {"num_ctx", settings.num_ctx},
            {"repeat_penalty", settings.repeat_penalty}
        }}
    };
    if (tools.is_array() && !tools.empty()) body["tools"] = tools;
    if (format.is_object() && !format.empty()) body["format"] = format;

    // error_handler_t::replace: messages carry live tool output (web pages,
    // rag excerpts) and model text. A strict dump() throws on one bad UTF-8
    // byte - the failure that killed the old in-olli subcon three times
    // (found live 2026-09-26) and made fetched pages cut mid-character
    // unserialisable (SUBCON.md, "Known limitations").
    std::string request_body;
    try { request_body = body.dump(-1, ' ', false, json::error_handler_t::replace); }
    catch (const std::exception& e) { reply.error = std::string("couldn't build the request: ") + e.what(); return reply; }

    CURL* curl = curl_easy_init();
    if (!curl) { reply.error = "couldn't initialise libcurl"; return reply; }

    std::string url = "http://" + settings.host + ":" + std::to_string(settings.port) + "/api/chat";
    std::string response_body;
    char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = '\0';

    curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request_body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, REQUEST_TIMEOUT_SECONDS);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT_SECONDS);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // we're on a non-main thread; libcurl must not use signals for timeouts
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_check);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &abort);

    CURLcode res = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (res == CURLE_ABORTED_BY_CALLBACK) { reply.error = "cancelled"; return reply; }
    if (res != CURLE_OK) {
        reply.error = std::string("couldn't reach Ollama at ") + settings.host + ":" + std::to_string(settings.port) +
                      " (" + curl_easy_strerror(res) + (errbuf[0] ? std::string(": ") + errbuf : "") + ")";
        return reply;
    }
    if (http_status != 200) {
        reply.error = "Ollama answered HTTP " + std::to_string(http_status) + ": " + response_body.substr(0, 300);
        return reply;
    }

    try {
        json parsed = json::parse(response_body);
        if (!parsed.value("done", false)) { reply.error = "Ollama's reply was not marked done"; return reply; }
        if (!parsed.contains("message") || !parsed["message"].is_object()) { reply.error = "Ollama's reply had no message"; return reply; }

        reply.message = parsed["message"];
        reply.content = reply.message.value("content", "");
        reply.thinking = reply.message.value("thinking", "");

        // Same lenient shape as olli's parse_tool_calls() (olla.cpp): a
        // malformed entry is skipped, never allowed to take the rest down.
        if (reply.message.contains("tool_calls") && reply.message["tool_calls"].is_array()) {
            for (const json& tc : reply.message["tool_calls"]) {
                if (!tc.is_object() || !tc.contains("function") || !tc["function"].is_object()) continue;
                const json& fn = tc["function"];
                OLLAMA_TOOL_CALL call;
                // is_string() first: value() throws on a name that exists
                // but isn't text (found in review 2026-10-29), which failed
                // the whole reply instead of skipping this one entry.
                if (!fn.contains("name") || !fn["name"].is_string()) continue;
                call.name = fn["name"].get<std::string>();
                if (call.name.empty()) continue;
                if (fn.contains("arguments") && fn["arguments"].is_object()) call.arguments = fn["arguments"];
                else if (fn.contains("arguments") && fn["arguments"].is_string()) {
                    // Some models hand arguments back as a JSON string.
                    try { json a = json::parse(fn["arguments"].get<std::string>()); if (a.is_object()) call.arguments = a; }
                    catch (const std::exception&) {}
                }
                reply.tool_calls.push_back(std::move(call));
            }
        }
        reply.ok = true;
    } catch (const std::exception& e) {
        reply.error = std::string("couldn't parse Ollama's reply: ") + e.what();
    }
    return reply;
}
