#ifndef tools_info_h
#define tools_info_h

// "get_weather" and "get_news" - two small internal tools that read public
// information sites and hand the model clean, structured text, so it does not
// have to guess from web_search snippets or scrape a page full of script.
//
//   get_weather  Open-Meteo (https://open-meteo.com): free, no account or key,
//                one place-name lookup + one forecast request.
//   get_news     RSS feeds (NPR and BBC by default). Headlines + a short
//                summary; every story's link is shown to the user as a link.
//
// Both are written to stand on their own: everything they need to know comes
// from info_settings.json in the profile folder and from the plain functions
// declared here (no ai_root internals beyond the normal tool interface), so
// that when olli gets a remote-tool protocol that can carry links they can be
// lifted into one standalone program unchanged (see TODO.md, 2026-11-19).
// All network access goes through TOOL_WEB_SEARCH::curl_get(..., lock_down =
// true): http/https only, public internet addresses only, size-capped.

#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "tools.h"

// ---- settings: <profile>/info_settings.json (created with these defaults if missing) ----

struct INFO_SETTINGS
{
    std::string home_location = "Eunice, Louisiana";   // used when the user does not name a place
    std::string temperature_unit = "fahrenheit";       // "fahrenheit" (also mph, inches) or "celsius" (km/h, mm)
    std::string default_news_feed = "top";
    // name -> RSS 2.0 URL, in the order they are listed to the model
    std::vector<std::pair<std::string, std::string>> news_feeds = {
        {"top",        "https://feeds.npr.org/1001/rss.xml"},
        {"us",         "https://feeds.npr.org/1003/rss.xml"},
        {"world",      "https://feeds.bbci.co.uk/news/world/rss.xml"},
        {"technology", "https://feeds.bbci.co.uk/news/technology/rss.xml"},
        {"business",   "https://feeds.bbci.co.uk/news/business/rss.xml"},
        {"science",    "https://feeds.bbci.co.uk/news/science_and_environment/rss.xml"},
    };
    int cache_seconds = 600;                            // the same answer is reused for this long
};

// Reads <profile_dir>/info_settings.json. A missing file is written out with the
// defaults above (so there is something to edit); a bad or partly missing file
// falls back to the defaults for whatever could not be read.
INFO_SETTINGS load_info_settings(const std::filesystem::path& profile_dir);

// ---- pure helpers (no network, no olli state) - declared here so tests can reach them ----

// WMO weather code -> words ("Clear sky", "Light rain", ...).
std::string weather_code_text(int code);

// 0-360 degrees -> "N", "NNE", ... (16 points).
std::string compass_point(double degrees);

// A place found by Open-Meteo's geocoder.
struct INFO_PLACE
{
    std::string name, admin1, country, country_code, timezone;
    double latitude = 0.0, longitude = 0.0;
    std::string label() const;   // "Eunice, Louisiana, United States"
};

// Picks one place out of a geocoding response ({"results":[...]}). 'hint' is
// whatever followed the comma in "Eunice, Louisiana" - the result whose state/
// country matches it is preferred; otherwise the first (most populous) result.
// {false, {}} if there are no results.
std::pair<bool, INFO_PLACE> pick_place(const nlohmann::json& geocode_response, const std::string& hint);

// Open-Meteo forecast JSON -> the text handed to the model. {false, reason} if
// the JSON is not in the expected shape.
std::pair<bool, std::string> format_forecast(const nlohmann::json& forecast, const std::string& place_label, const std::string& temperature_unit);

struct NEWS_ITEM
{
    std::string title, description, link, published, source;   // source = the feed's own name, e.g. "BBC News"
};

// RSS 2.0 text -> up to max_items stories (title, short plain-text summary,
// link, date). Handles CDATA, XML entities and HTML inside descriptions. An
// empty vector means "no <item> found" (not RSS, or an empty feed).
std::vector<NEWS_ITEM> parse_rss(const std::string& xml, size_t max_items);

// XML/HTML entities (&amp; &#39; &#x2019; ...) -> UTF-8.
std::string decode_xml_entities(const std::string& text);

// ---- the tools ----

class TOOL_WEATHER : public TOOL_BASE
{
    private:
        std::filesystem::path profile_dir;

        // lower-cased location text -> resolved place, so a repeat question skips the lookup
        std::map<std::string, INFO_PLACE> place_cache;
        // lower-cased location text -> {when fetched, finished text}
        std::map<std::string, std::pair<time_t, std::string>> result_cache;

        std::pair<bool, std::string> get_weather(const std::string& location);
        void handle_tool(ai_root& chat, TOOL_WORKER_CLASS* tool_worker, COMMS& comms, const json& args, const std::string& tc_id);

    public:
        void configure(ai_root& chat) override;
        void register_tool(ai_root& chat, json& tools) override;
        bool check(IO_WORKER_CLASS& io_worker, ai_root& chat, CLASS_SYSTEM* system, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, TOOL_WORKER_CLASS* tool_worker, COMMS& comms, const ToolCall& tc) override;
        void monitor_tool(ai_root& chat, CLASS_SYSTEM* system, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, TOOL_WORKER_CLASS* tool_worker, COMMS& comms) override;
};

class TOOL_NEWS : public TOOL_BASE
{
    private:
        std::filesystem::path profile_dir;

        // feed name -> {when fetched, stories}
        std::map<std::string, std::pair<time_t, std::vector<NEWS_ITEM>>> cache;

        std::pair<bool, std::string> get_news(const std::string& feed, int count, COMMS& comms);
        void handle_tool(ai_root& chat, TOOL_WORKER_CLASS* tool_worker, COMMS& comms, const json& args, const std::string& tc_id);

    public:
        void configure(ai_root& chat) override;
        void register_tool(ai_root& chat, json& tools) override;
        bool check(IO_WORKER_CLASS& io_worker, ai_root& chat, CLASS_SYSTEM* system, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, TOOL_WORKER_CLASS* tool_worker, COMMS& comms, const ToolCall& tc) override;
        void monitor_tool(ai_root& chat, CLASS_SYSTEM* system, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, TOOL_WORKER_CLASS* tool_worker, COMMS& comms) override;
};

#endif
