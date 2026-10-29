#include "internal_tools.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <utility>

#include <curl/curl.h>

using json = nlohmann::json;

namespace {
    // Same numbers as source/tools.h's TOOL_WEB_SEARCH: the 2026-09-16 fix
    // raised the original flat timeout, added the connect-timeout (bounds
    // DNS+connect+TLS separately from the transfer) and the retry below.
    constexpr long SEARCH_TIMEOUT_SECONDS = 20;
    constexpr long FETCH_TIMEOUT_SECONDS = 25;
    constexpr long CONNECT_TIMEOUT_SECONDS = 5;

    constexpr size_t FETCH_RESULT_MAX_BYTES = 4000;      // same as the original
    constexpr size_t DOWNLOAD_MAX_BYTES = 2 * 1024 * 1024; // beyond this the rest of a page is discarded, not buffered

    // serpapi key placeholder olli's settings.json starts with (source/helper_olli.h).
    const char* const UNSET_API_KEY = "Enter_API_key_for_serpapi.com";

    struct DOWNLOAD { std::string body; };

    size_t write_capped(void* contents, size_t size, size_t nmemb, void* userp)
    {
        DOWNLOAD* d = static_cast<DOWNLOAD*>(userp);
        size_t n = size * nmemb;
        if (d->body.size() < DOWNLOAD_MAX_BYTES)
            d->body.append(static_cast<char*>(contents), std::min(n, DOWNLOAD_MAX_BYTES - d->body.size()));
        return n; // always "consumed", so an oversized page is discarded rather than failing the transfer
    }

    int progress_check(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
    {
        return static_cast<const std::atomic<bool>*>(clientp)->load() ? 1 : 0;
    }

    // Same shape as TOOL_WEB_SEARCH::curl_get(): only a timeout gets a second
    // attempt - a slow DNS/TLS/server hiccup can resolve itself, but a bad
    // URL / refused connection / unknown host fails identically again, so
    // retrying those would only double the wait. The error text is built to
    // be passed on to the model as-is (it explains failures in its own
    // words rather than narrating raw curl output as data).
    std::pair<bool, std::string> curl_get(const std::string& url, long timeout_seconds, const std::string& user_agent,
                                          bool fail_on_http_error, const std::atomic<bool>& abort)
    {
        for (int attempt = 0; attempt < 2; ++attempt) {
            CURL* curl = curl_easy_init();
            if (!curl) return {false, "Error: Could not initialize libcurl."};

            DOWNLOAD download;
            char errbuf[CURL_ERROR_SIZE];
            errbuf[0] = '\0';

            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_capped);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT_SECONDS);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            // A 404/500 on a page fetch is a failure, not "page content":
            // without this the server's error page came back as a successful
            // fetch (the original behavior) and could be mistaken for the
            // real article. Not for the search API, whose error body
            // ("Invalid API key", quota) is what explains the failure.
            // (Added in this copy.)
            curl_easy_setopt(curl, CURLOPT_FAILONERROR, fail_on_http_error ? 1L : 0L);
            // The model chooses the URL for fetch_website_content: without
            // this libcurl would happily read file:///... or other schemes.
            // (Added in this copy; the original in source/tools.cpp has no
            // such limit.)
            curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
            curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
            curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // non-main thread
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_check);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &abort);
            if (!user_agent.empty()) curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());

            CURLcode res = curl_easy_perform(curl);
            long http_status = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
            curl_easy_cleanup(curl);

            if (res == CURLE_OK) return {true, std::move(download.body)};
            if (res == CURLE_ABORTED_BY_CALLBACK) return {false, "Error: cancelled."};
            if (res == CURLE_OPERATION_TIMEDOUT && attempt == 0) continue;

            std::string detail = errbuf[0] != '\0' ? std::string(": ") + errbuf : "";
            return {false, "Error: libcurl failed (" + std::string(curl_easy_strerror(res)) + ")" + detail};
        }
        return {false, "Error: libcurl failed (unreachable)."};
    }

    // Removes <script>/<style> blocks, then every tag, then collapses
    // whitespace. A single linear pass instead of the original's std::regex
    // replaces (source/tools.cpp strip_html_tags): libstdc++'s regex
    // recurses per character matched, so a multi-megabyte page can overflow
    // the stack - on this worker thread that is a crash of the whole tool.
    std::string strip_html_tags(const std::string& html)
    {
        std::string lower = html;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        std::string out;
        out.reserve(html.size() / 2);
        size_t i = 0;
        while (i < html.size()) {
            if (html[i] != '<') { out += html[i++]; continue; }

            size_t skip_to = std::string::npos;
            if (lower.compare(i, 7, "<script") == 0) skip_to = lower.find("</script", i);
            else if (lower.compare(i, 6, "<style") == 0) skip_to = lower.find("</style", i);
            if (skip_to == std::string::npos) skip_to = i; // not a script/style block: just this tag

            size_t close = html.find('>', skip_to);
            if (close == std::string::npos) break; // unterminated tag: drop the remainder
            i = close + 1;
            out += ' ';
        }

        std::string collapsed;
        collapsed.reserve(out.size());
        bool in_space = false;
        for (char c : out) {
            bool space = std::isspace(static_cast<unsigned char>(c)) != 0;
            if (space) { in_space = true; continue; }
            if (in_space && !collapsed.empty()) collapsed += ' ';
            in_space = false;
            collapsed += c;
        }
        return collapsed;
    }

    INTERNAL_TOOL_RESULT web_search(const std::string& query, const std::string& api_key, const std::atomic<bool>& abort)
    {
        if (api_key.empty() || api_key == UNSET_API_KEY)
            return {false, "Error: web search is not configured (no search API key is set for subcon)."};

        CURL* escape_handle = curl_easy_init();
        if (!escape_handle) return {false, "Error: Could not initialize libcurl."};
        char* output = curl_easy_escape(escape_handle, query.c_str(), static_cast<int>(query.length()));
        std::string encoded = output ? output : "";
        curl_free(output);
        curl_easy_cleanup(escape_handle);

        std::string url = "https://serpapi.com/search.json?q=" + encoded + "&api_key=" + api_key;
        auto [ok, body] = curl_get(url, SEARCH_TIMEOUT_SECONDS, "", false, abort);
        if (!ok) return {false, body};

        try {
            json data = json::parse(body);
            if (data.contains("error"))
                return {false, "Search API Error: " + (data["error"].is_string() ? data["error"].get<std::string>() : data["error"].dump())};

            std::string summary = "SEARCH_RESULTS_START\n";
            if (data.contains("organic_results") && data["organic_results"].is_array()) {
                int count = 0;
                for (const json& item : data["organic_results"]) {
                    if (count++ >= 3) break;
                    summary += "RESULT_ITEM:\n";
                    summary += "[TITLE]: " + item.value("title", std::string("No Title")) + "\n";
                    summary += "[SNIPPET]: " + item.value("snippet", std::string("No description")) + "\n";
                    summary += "[SOURCE_URL]: " + item.value("link", std::string()) + "\n\n";
                }
            } else {
                summary = "No specific snippets found.";
            }
            summary += "SEARCH_RESULTS_END";
            return {true, summary};
        } catch (const std::exception& e) {
            return {false, "Error: Failed to parse search engine response: " + std::string(e.what())};
        }
    }

    INTERNAL_TOOL_RESULT fetch_website(const std::string& url, const std::atomic<bool>& abort)
    {
        auto [ok, body] = curl_get(url, FETCH_TIMEOUT_SECONDS, "Mozilla/5.0 (Windows NT 10.0; Win64; x64)", true, abort);
        if (!ok) return {false, body};
        return {true, "Cleaned Page Content from " + url + ":\n" + truncate_utf8(strip_html_tags(body), FETCH_RESULT_MAX_BYTES)};
    }

    INTERNAL_TOOL_RESULT clock_time(const std::string& format)
    {
        std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        char buffer[256];
        size_t n = std::strftime(buffer, sizeof(buffer), format.empty() ? "%A %Y-%m-%d %H:%M:%S %Z" : format.c_str(), &local);
        if (n == 0) return {false, "Error: that is not a usable time format."};
        return {true, std::string(buffer, n)};
    }
}

std::string truncate_utf8(const std::string& text, size_t max_bytes)
{
    if (text.size() <= max_bytes) return text;
    size_t cut = max_bytes;
    // text[cut] is the first byte being dropped; if it is a continuation
    // byte (10xxxxxx) the character it belongs to started before it, so back
    // up to that character's lead byte and drop the whole character.
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return text.substr(0, cut) + "... [truncated]";
}

json internal_tool_defs()
{
    // Descriptions follow source/tools.cpp's register_tool(), including the
    // "no raw URL in your answer" nudge (irrelevant for a finding that is
    // read back by code, harmless, and kept so the copy stays recognisable).
    return json::array({
        {
            {"name", "web_search"},
            {"description", "Searches the internet. Results include titles, snippets, and URLs."},
            {"parameters", {
                {"type", "object"},
                {"properties", {{"query", {{"type", "string"}, {"description", "The search terms."}}}}},
                {"required", json::array({"query"})}
            }}
        },
        {
            {"name", "fetch_website_content"},
            {"description", "Reads the text from a specific URL for deep research. Use this to summarize an article."},
            {"parameters", {
                {"type", "object"},
                {"properties", {{"url", {{"type", "string"}, {"description", "The URL to read content from."}}}}},
                {"required", json::array({"url"})}
            }}
        },
        {
            {"name", "get_clock_time"},
            {"description", "Returns the current local date and time. Always execute this tool call rather than assuming the time."},
            {"parameters", {
                {"type", "object"},
                {"properties", {{"format", {{"type", "string"}, {"description", "Optional strftime format string, e.g. '%H:%M:%S'"}}}}},
                {"required", json::array()}
            }}
        }
    });
}

bool is_internal_tool(const std::string& name)
{
    return name == "web_search" || name == "fetch_website_content" || name == "get_clock_time";
}

INTERNAL_TOOL_RESULT run_internal_tool(const std::string& name, const json& arguments, const std::string& api_key,
                                       const std::atomic<bool>& abort)
{
    auto string_arg = [&](const char* key) -> std::string {
        return arguments.is_object() && arguments.contains(key) && arguments[key].is_string() ? arguments[key].get<std::string>() : "";
    };

    if (name == "web_search") {
        std::string query = string_arg("query");
        if (query.empty()) return {false, "Error: Missing query."};
        return web_search(query, api_key, abort);
    }
    if (name == "fetch_website_content") {
        std::string url = string_arg("url");
        if (url.empty()) return {false, "Error: Missing URL."};
        return fetch_website(url, abort);
    }
    if (name == "get_clock_time") return clock_time(string_arg("format"));
    return {false, "Error: Unknown tool."};
}
