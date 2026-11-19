// get_weather and get_news - see tools_info.h.

#include "tools_info.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

#include <curl/curl.h>

#include "ai_root.h"
#include "comms.h"
#include "tool_worker.h"

namespace fs = std::filesystem;
using ojson = nlohmann::ordered_json;   // keeps the feed list in the order the user wrote it

namespace {

    // Same user agent the page-fetch tool sends; a few sites refuse an empty one.
    const char* const USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)";
    constexpr long REQUEST_TIMEOUT_SECONDS = 8;   // both tools run on olli's main thread until they become a remote program
    constexpr size_t NEWS_SUMMARY_MAX_BYTES = 240;
    constexpr size_t NEWS_MAX_STORIES = 10;
    constexpr int    NEWS_DEFAULT_STORIES = 5;

    const std::string FAILURE_INSTRUCTION =
        "This attempt did NOT succeed - it is an error, not real data. Tell "
        "the user plainly, in your own words, that it failed and what "
        "likely went wrong. Do not present this as a real answer to their "
        "question.";

    std::string to_lower(std::string s) {
        for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        return s;
    }

    std::string trim_text(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    // Runs of whitespace (including newlines) -> one space, ends trimmed.
    std::string collapse_whitespace(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        bool pending_space = false;
        for (char c : s) {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') { pending_space = true; continue; }
            if (pending_space && !out.empty()) out.push_back(' ');
            pending_space = false;
            out.push_back(c);
        }
        return out;
    }

    // Cuts to at most max_bytes without splitting a UTF-8 character, adding "..." if it cut.
    std::string cut_utf8(const std::string& s, size_t max_bytes) {
        if (s.size() <= max_bytes) return s;
        size_t n = max_bytes;
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;   // s[n] is a continuation byte: back up
        return s.substr(0, n) + "...";
    }

    std::string url_escape(const std::string& text) {
        CURL* handle = curl_easy_init();
        if (!handle) return "";
        char* out = curl_easy_escape(handle, text.c_str(), static_cast<int>(text.size()));
        std::string result = out ? out : "";
        curl_free(out);
        curl_easy_cleanup(handle);
        return result;
    }

    void append_utf8(std::string& out, unsigned long cp) {
        if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return;
        if (cp < 0x80)         { out.push_back(static_cast<char>(cp)); }
        else if (cp < 0x800)   { out.push_back(static_cast<char>(0xC0 | (cp >> 6)));  out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { out.push_back(static_cast<char>(0xE0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else                   { out.push_back(static_cast<char>(0xF0 | (cp >> 18))); out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    }

    // "Thu" for "2026-11-19"; "" if the text is not a date.
    std::string weekday_name(const std::string& date) {
        int y = 0, m = 0, d = 0;
        if (std::sscanf(date.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return "";
        std::tm t{};
        t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d; t.tm_hour = 12;
        if (timegm(&t) == -1) return "";
        static const char* const names[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        return names[t.tm_wday];
    }

    std::string json_string(const nlohmann::json& obj, const char* key) {
        auto it = obj.find(key);
        return (it != obj.end() && it->is_string()) ? it->get<std::string>() : std::string();
    }

    double json_number(const nlohmann::json& obj, const char* key) {
        auto it = obj.find(key);
        return (it != obj.end() && it->is_number()) ? it->get<double>() : 0.0;
    }

    // A number from a forecast array/field as text, "?" when it is missing or null.
    std::string num_text(const nlohmann::json& v, int decimals = 0) {
        if (!v.is_number()) return "?";
        char buf[32];
        double x = v.get<double>();
        if (decimals == 0) {
            long r = std::lround(x);
            std::snprintf(buf, sizeof buf, "%ld", r);
        } else {
            std::snprintf(buf, sizeof buf, "%.*f", decimals, x);
        }
        return buf;
    }

    const nlohmann::json& element_at(const nlohmann::json& array, size_t i) {
        static const nlohmann::json null_value;
        return (array.is_array() && i < array.size()) ? array[i] : null_value;
    }

    // obj[key] without the undefined behaviour of operator[] on a const object when the key is missing.
    const nlohmann::json& field(const nlohmann::json& obj, const char* key) {
        static const nlohmann::json null_value;
        auto it = obj.find(key);
        return it != obj.end() ? *it : null_value;
    }

    // Small models fill an optional "location" with a word like "current" or "here" instead of leaving it
    // out (seen live: "current" was looked up as a town of that name). Those all mean "where the user lives".
    bool means_home(const std::string& location) {
        static const char* const words[] = {"current", "current location", "here", "home", "local", "locally", "nearby", "near me",
                                            "me", "my location", "my area", "my city", "my town", "where i am", "where i live",
                                            "default", "none", "null", "n/a", "unknown", "now", "today"};
        const std::string l = to_lower(trim_text(location));
        for (const char* w : words) if (l == w) return true;
        return false;
    }

    // ---- RSS pieces ----

    // Text between <tag ...> and </tag> inside 'xml', or "" if there is none. Sets is_cdata when
    // the content was wrapped in <![CDATA[ ... ]]> (which is returned unwrapped and NOT entity-decoded).
    std::string tag_content(const std::string& xml, const std::string& tag, bool& is_cdata) {
        is_cdata = false;
        const std::string open = "<" + tag;
        size_t pos = 0;
        while ((pos = xml.find(open, pos)) != std::string::npos) {
            size_t after = pos + open.size();
            char next = after < xml.size() ? xml[after] : '\0';
            if (next == '>' || next == ' ' || next == '\t' || next == '\r' || next == '\n' || next == '/') break;
            pos = after;   // "<titlefoo": not our tag
        }
        if (pos == std::string::npos) return "";

        size_t gt = xml.find('>', pos);
        if (gt == std::string::npos) return "";
        if (xml[gt - 1] == '/') return "";   // <tag/>
        size_t start = gt + 1;
        size_t end = xml.find("</" + tag + ">", start);
        if (end == std::string::npos) return "";

        std::string content = trim_text(xml.substr(start, end - start));
        static const std::string cdata_open = "<![CDATA[", cdata_close = "]]>";
        if (content.compare(0, cdata_open.size(), cdata_open) == 0) {
            is_cdata = true;
            content = content.substr(cdata_open.size());
            size_t close = content.rfind(cdata_close);
            if (close != std::string::npos) content = content.substr(0, close);
        }
        return content;
    }

    // Removes <...> pieces. A '<' with no '>' after it is kept as text.
    std::string strip_tags(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        size_t i = 0;
        while (i < s.size()) {
            if (s[i] == '<') {
                size_t gt = s.find('>', i + 1);
                if (gt != std::string::npos) { out.push_back(' '); i = gt + 1; continue; }
            }
            out.push_back(s[i++]);
        }
        return out;
    }

    // One text field of an <item>: unwrap, drop markup, decode entities, tidy, cut.
    std::string clean_field(const std::string& item, const std::string& tag, size_t max_bytes) {
        bool cdata = false;
        std::string raw = tag_content(item, tag, cdata);
        if (raw.empty()) return "";
        std::string text = cdata ? raw : decode_xml_entities(raw);
        text = strip_tags(text);
        if (cdata) text = decode_xml_entities(text);
        text = collapse_whitespace(text);
        return max_bytes ? cut_utf8(text, max_bytes) : text;
    }
}

// ---- settings ----

INFO_SETTINGS load_info_settings(const fs::path& profile_dir) {
    INFO_SETTINGS s;
    if (profile_dir.empty()) return s;

    const fs::path file = profile_dir / "info_settings.json";
    std::error_code ec;

    if (!fs::exists(file, ec)) {
        ojson j;
        j["home_location"] = s.home_location;
        j["temperature_unit"] = s.temperature_unit;
        j["default_news_feed"] = s.default_news_feed;
        ojson feeds = ojson::object();
        for (const auto& [name, url] : s.news_feeds) feeds[name] = url;
        j["news_feeds"] = feeds;
        j["cache_seconds"] = s.cache_seconds;

        fs::create_directories(profile_dir, ec);
        std::ofstream out(file);
        if (out.is_open()) out << j.dump(4) << "\n";
        return s;
    }

    try {
        std::ifstream in(file);
        ojson j = ojson::parse(in);

        if (j.contains("home_location") && j["home_location"].is_string() && !trim_text(j["home_location"].get<std::string>()).empty())
            s.home_location = trim_text(j["home_location"].get<std::string>());
        if (j.contains("temperature_unit") && j["temperature_unit"].is_string()) {
            std::string u = to_lower(trim_text(j["temperature_unit"].get<std::string>()));
            if (u == "fahrenheit" || u == "celsius") s.temperature_unit = u;
        }
        if (j.contains("default_news_feed") && j["default_news_feed"].is_string())
            s.default_news_feed = to_lower(trim_text(j["default_news_feed"].get<std::string>()));
        if (j.contains("cache_seconds") && j["cache_seconds"].is_number_integer())
            s.cache_seconds = std::clamp(j["cache_seconds"].get<int>(), 0, 86400);
        if (j.contains("news_feeds") && j["news_feeds"].is_object()) {
            std::vector<std::pair<std::string, std::string>> feeds;
            for (auto it = j["news_feeds"].begin(); it != j["news_feeds"].end(); ++it)
                if (it.value().is_string() && !it.key().empty()) feeds.emplace_back(to_lower(it.key()), it.value().get<std::string>());
            if (!feeds.empty()) s.news_feeds = feeds;
        }
    } catch (const std::exception&) {
        // unreadable file: keep the defaults rather than failing the tool
    }
    return s;
}

// ---- pure helpers ----

std::string weather_code_text(int code) {
    switch (code) {
        case 0:  return "Clear sky";
        case 1:  return "Mainly clear";
        case 2:  return "Partly cloudy";
        case 3:  return "Overcast";
        case 45: return "Fog";
        case 48: return "Freezing fog";
        case 51: return "Light drizzle";
        case 53: return "Drizzle";
        case 55: return "Heavy drizzle";
        case 56: case 57: return "Freezing drizzle";
        case 61: return "Light rain";
        case 63: return "Rain";
        case 65: return "Heavy rain";
        case 66: case 67: return "Freezing rain";
        case 71: return "Light snow";
        case 73: return "Snow";
        case 75: return "Heavy snow";
        case 77: return "Snow grains";
        case 80: return "Light rain showers";
        case 81: return "Rain showers";
        case 82: return "Violent rain showers";
        case 85: return "Light snow showers";
        case 86: return "Heavy snow showers";
        case 95: return "Thunderstorm";
        case 96: case 99: return "Thunderstorm with hail";
        default: return "Unknown conditions";
    }
}

std::string compass_point(double degrees) {
    static const char* const points[16] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    double d = std::fmod(degrees, 360.0);
    if (d < 0) d += 360.0;
    return points[static_cast<int>((d + 11.25) / 22.5) % 16];
}

std::string INFO_PLACE::label() const {
    std::string out = name;
    if (!admin1.empty()) out += ", " + admin1;
    if (!country.empty()) out += ", " + country;
    return out;
}

std::pair<bool, INFO_PLACE> pick_place(const nlohmann::json& response, const std::string& hint_text) {
    auto results = response.find("results");
    if (results == response.end() || !results->is_array() || results->empty()) return {false, INFO_PLACE{}};

    const std::string hint = to_lower(trim_text(hint_text));
    size_t chosen = 0;
    if (!hint.empty()) {
        for (size_t i = 0; i < results->size(); ++i) {
            const auto& r = (*results)[i];
            std::string admin1 = to_lower(json_string(r, "admin1"));
            std::string country = to_lower(json_string(r, "country"));
            std::string code = to_lower(json_string(r, "country_code"));
            bool match = admin1 == hint || country == hint || code == hint ||
                         (hint.size() >= 3 && (admin1.find(hint) != std::string::npos || country.find(hint) != std::string::npos));
            if (match) { chosen = i; break; }
        }
    }

    const auto& r = (*results)[chosen];
    INFO_PLACE p;
    p.name = json_string(r, "name");
    p.admin1 = json_string(r, "admin1");
    p.country = json_string(r, "country");
    p.country_code = json_string(r, "country_code");
    p.timezone = json_string(r, "timezone");
    p.latitude = json_number(r, "latitude");
    p.longitude = json_number(r, "longitude");
    if (p.name.empty()) return {false, INFO_PLACE{}};
    return {true, p};
}

std::pair<bool, std::string> format_forecast(const nlohmann::json& f, const std::string& place_label, const std::string& temperature_unit) {
    try {
        const bool celsius = (temperature_unit == "celsius");
        const std::string deg = celsius ? "\xC2\xB0""C" : "\xC2\xB0""F";
        const std::string wind_unit = celsius ? "km/h" : "mph";
        const std::string rain_unit = celsius ? "mm" : "in";

        const auto& cur = f.at("current");
        const auto& hourly = f.at("hourly");
        const auto& daily = f.at("daily");

        std::string now = cur.at("time").get<std::string>();           // "2026-11-19T02:15"
        std::string today = now.substr(0, 10);
        std::string now_label = now;
        if (now_label.size() > 10) now_label[10] = ' ';

        std::string out = "Weather for " + place_label + " (local time " + now_label + "):\n";

        out += "Now: " + num_text(field(cur, "temperature_2m")) + deg +
               " (feels like " + num_text(field(cur, "apparent_temperature")) + deg + "), " +
               weather_code_text(field(cur, "weather_code").is_number() ? field(cur, "weather_code").get<int>() : -1) +
               ", humidity " + num_text(field(cur, "relative_humidity_2m")) + "%, wind " +
               num_text(field(cur, "wind_speed_10m")) + " " + wind_unit;
        if (field(cur, "wind_direction_10m").is_number()) out += " from " + compass_point(field(cur, "wind_direction_10m").get<double>());
        out += ", precipitation " + num_text(field(cur, "precipitation"), celsius ? 1 : 2) + " " + rain_unit + ".\n";

        // ---- next 24 hours ----
        const auto& h_time = hourly.at("time");
        const auto& h_temp = field(hourly, "temperature_2m");
        const auto& h_rain = field(hourly, "precipitation_probability");
        const auto& h_code = field(hourly, "weather_code");

        const std::string this_hour = now.substr(0, 13) + ":00";
        size_t i0 = 0;
        while (i0 < h_time.size() && h_time[i0].get<std::string>() < this_hour) ++i0;

        double lo = 1e9, hi = -1e9;
        size_t lo_i = 0, hi_i = 0;
        std::string rows;
        size_t last = std::min(h_time.size(), i0 + 24);
        for (size_t i = i0; i < last; ++i) {
            const auto& t = element_at(h_temp, i);
            if (t.is_number()) {
                double v = t.get<double>();
                if (v < lo) { lo = v; lo_i = i; }
                if (v > hi) { hi = v; hi_i = i; }
            }
            if ((i - i0) % 3 == 0) {
                std::string when = h_time[i].get<std::string>();
                const auto& c = element_at(h_code, i);
                rows += "  " + when.substr(11, 5) + "  " + num_text(t) + deg + ", " +
                        weather_code_text(c.is_number() ? c.get<int>() : -1) +
                        ", rain chance " + num_text(element_at(h_rain, i)) + "%\n";
            }
        }
        if (!rows.empty()) {
            out += "Next 24 hours (every 3 hours):\n" + rows;
            if (lo < 1e8) {
                auto when = [&](size_t i) {
                    std::string t = h_time[i].get<std::string>();
                    return (t.substr(0, 10) == today ? std::string("today") : weekday_name(t.substr(0, 10))) + " " + t.substr(11, 5);
                };
                out += "Lowest in the next 24 hours: " + num_text(nlohmann::json(lo)) + deg + " at " + when(lo_i) +
                       "; highest: " + num_text(nlohmann::json(hi)) + deg + " at " + when(hi_i) + ".\n";
            }
        }

        // ---- daily ----
        const auto& d_time = daily.at("time");
        out += "Daily forecast (a day's high and low cover the whole calendar day, midnight to midnight):\n";
        for (size_t i = 0; i < d_time.size() && i < 5; ++i) {
            std::string date = d_time[i].get<std::string>();
            std::string label = weekday_name(date) + " " + date;
            if (date == today) label += " (today)";
            else if (i == 1)   label += " (tomorrow)";
            const auto& c = element_at(field(daily, "weather_code"), i);
            const auto& sr = element_at(field(daily, "sunrise"), i);
            const auto& ss = element_at(field(daily, "sunset"), i);
            std::string sunrise = sr.is_string() ? sr.get<std::string>() : "";
            std::string sunset  = ss.is_string() ? ss.get<std::string>() : "";
            out += "  " + label + ": " + weather_code_text(c.is_number() ? c.get<int>() : -1) +
                   ", high " + num_text(element_at(field(daily, "temperature_2m_max"), i)) + deg +
                   ", low " + num_text(element_at(field(daily, "temperature_2m_min"), i)) + deg +
                   ", rain chance " + num_text(element_at(field(daily, "precipitation_probability_max"), i)) + "%";
            if (sunrise.size() >= 16 && sunset.size() >= 16) out += ", sunrise " + sunrise.substr(11, 5) + ", sunset " + sunset.substr(11, 5);
            out += "\n";
        }
        out += "Source: Open-Meteo.";
        return {true, out};
    } catch (const nlohmann::json::exception& e) {
        return {false, std::string("Error: the weather service sent data in an unexpected format (") + e.what() + ")."};
    } catch (const std::exception& e) {
        return {false, std::string("Error: could not read the weather data (") + e.what() + ")."};
    }
}

std::string decode_xml_entities(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') { out.push_back(text[i]); continue; }
        size_t semi = text.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 10) { out.push_back('&'); continue; }
        std::string ent = text.substr(i + 1, semi - i - 1);
        std::string rep;
        bool known = true;
        if      (ent == "amp")  rep = "&";
        else if (ent == "lt")   rep = "<";
        else if (ent == "gt")   rep = ">";
        else if (ent == "quot") rep = "\"";
        else if (ent == "apos") rep = "'";
        else if (ent == "nbsp") rep = " ";
        else if (!ent.empty() && ent[0] == '#') {
            bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
            std::string digits = ent.substr(hex ? 2 : 1);
            char* end = nullptr;
            unsigned long cp = digits.empty() ? 0 : std::strtoul(digits.c_str(), &end, hex ? 16 : 10);
            if (digits.empty() || (end && *end != '\0')) known = false;
            else append_utf8(rep, cp);
        } else known = false;

        if (known) { out += rep; i = semi; }
        else out.push_back('&');
    }
    return out;
}

std::vector<NEWS_ITEM> parse_rss(const std::string& xml, size_t max_items) {
    std::vector<NEWS_ITEM> items;

    // The feed's own name (first <title> before the first story), e.g. "BBC News".
    size_t first_item = xml.find("<item");
    std::string source = clean_field(xml.substr(0, first_item == std::string::npos ? xml.size() : first_item), "title", 80);

    size_t pos = 0;
    while (items.size() < max_items && (pos = xml.find("<item", pos)) != std::string::npos) {
        char next = pos + 5 < xml.size() ? xml[pos + 5] : '\0';
        if (next != '>' && next != ' ' && next != '\t' && next != '\r' && next != '\n') { pos += 5; continue; }
        size_t end = xml.find("</item>", pos);
        if (end == std::string::npos) break;
        std::string item = xml.substr(pos, end - pos);
        pos = end + 7;

        NEWS_ITEM n;
        n.title = clean_field(item, "title", 200);
        if (n.title.empty()) continue;
        n.description = clean_field(item, "description", NEWS_SUMMARY_MAX_BYTES);
        n.link = clean_field(item, "link", 0);
        n.published = clean_field(item, "pubDate", 40);
        n.source = source;
        items.push_back(n);
    }
    return items;
}

// ---- TOOL_WEATHER ----

void TOOL_WEATHER::configure(ai_root& chat) {
    profile_dir = chat.PROPS.OLLI_DIRECTORY;
    load_info_settings(profile_dir);   // writes info_settings.json with the defaults the first time
}

void TOOL_WEATHER::register_tool(ai_root&, json& tools) {
    tool_functions.clear();

    json params = {
        {"type", "object"},
        {"properties", {
            {"location", {{"type", "string"}, {"description", "Optional. A real place name, like 'Houston, Texas' or 'Paris, France'. For the weather where the user is or lives (right now, here, home, current), leave this out completely - do not write 'current' or 'here'."}}}
        }},
        {"required", json::array()}
    };
    tool_functions.push_back("get_weather");
    add_tool(tools, "get_weather",
        "Gets the real current weather and forecast: right now, the next 24 hours, and the next few days, with temperatures, rain chances, highs and lows. "
        "Use this for any question about weather, temperature, rain, or tonight's low, instead of web_search.",
        params);
}

std::pair<bool, std::string> TOOL_WEATHER::get_weather(const std::string& location) {
    const INFO_SETTINGS settings = load_info_settings(profile_dir);
    const std::string where = (trim_text(location).empty() || means_home(location)) ? settings.home_location : trim_text(location);
    const std::string key = to_lower(where) + "|" + settings.temperature_unit;

    const time_t now = std::time(nullptr);
    auto cached = result_cache.find(key);
    if (cached != result_cache.end() && now - cached->second.first < settings.cache_seconds)
        return {true, cached->second.second};

    // 1. place name -> coordinates
    const std::string place_key = to_lower(where);
    INFO_PLACE place;
    auto known = place_cache.find(place_key);
    if (known != place_cache.end()) {
        place = known->second;
    } else {
        size_t comma = where.find(',');
        std::string name = trim_text(comma == std::string::npos ? where : where.substr(0, comma));
        std::string hint = comma == std::string::npos ? "" : trim_text(where.substr(comma + 1));

        auto [ok, body] = TOOL_WEB_SEARCH::curl_get(
            "https://geocoding-api.open-meteo.com/v1/search?name=" + url_escape(name) + "&count=10&language=en&format=json",
            REQUEST_TIMEOUT_SECONDS, USER_AGENT, true);
        if (!ok) return {false, body};

        nlohmann::json geo = nlohmann::json::parse(body, nullptr, false);
        if (geo.is_discarded()) return {false, "Error: the place-lookup service sent something that was not JSON."};
        auto [found, p] = pick_place(geo, hint);
        if (!found) return {false, "Error: could not find a place called '" + where + "'. Ask the user for the city and state or country."};
        place = p;
        place_cache[place_key] = place;
    }

    // 2. forecast
    const bool celsius = (settings.temperature_unit == "celsius");
    char coords[96];
    std::snprintf(coords, sizeof coords, "latitude=%.4f&longitude=%.4f", place.latitude, place.longitude);
    const std::string url = std::string("https://api.open-meteo.com/v1/forecast?") + coords +
        "&current=temperature_2m,apparent_temperature,relative_humidity_2m,precipitation,weather_code,wind_speed_10m,wind_direction_10m"
        "&hourly=temperature_2m,precipitation_probability,weather_code"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset"
        "&timezone=auto&forecast_days=5" +
        (celsius ? "&temperature_unit=celsius&wind_speed_unit=kmh&precipitation_unit=mm"
                 : "&temperature_unit=fahrenheit&wind_speed_unit=mph&precipitation_unit=inch");

    auto [ok, body] = TOOL_WEB_SEARCH::curl_get(url, REQUEST_TIMEOUT_SECONDS, USER_AGENT, true);
    if (!ok) return {false, body};

    nlohmann::json forecast = nlohmann::json::parse(body, nullptr, false);
    if (forecast.is_discarded()) return {false, "Error: the weather service sent something that was not JSON."};
    if (forecast.contains("error") && forecast.value("error", false))
        return {false, "Error: the weather service reported a problem: " + json_string(forecast, "reason")};

    auto [good, text] = format_forecast(forecast, place.label(), settings.temperature_unit);
    if (good) result_cache[key] = {now, text};
    return {good, text};
}

void TOOL_WEATHER::handle_tool(ai_root& chat, TOOL_WORKER_CLASS* tool_worker, COMMS& comms, const json& args, const std::string& tc_id) {
    std::string location = (args.contains("location") && args.at("location").is_string()) ? args.at("location").get<std::string>() : "";

    auto [ok, result] = get_weather(location);

    chat.send_tool_result(tc_id, result);
    if (ok) chat.integrate_tool_result(tool_worker, comms, "", "Weather result: " + result);
    else    chat.integrate_tool_result(tool_worker, comms, FAILURE_INSTRUCTION, "The weather lookup failed: " + result);
}

bool TOOL_WEATHER::check(IO_WORKER_CLASS&, ai_root& chat, CLASS_SYSTEM*, std::vector<std::unique_ptr<TOOL_BASE>>&, TOOL_WORKER_CLASS* tool_worker, COMMS& comms, const ToolCall& tc) {
    if (tc.name != "get_weather") return false;
    handle_tool(chat, tool_worker, comms, tc.arguments, tc.id);
    return true;
}

// No periodic work needed - part of the common tool interface (see the note in tools.h).
void TOOL_WEATHER::monitor_tool(ai_root&, CLASS_SYSTEM*, std::vector<std::unique_ptr<TOOL_BASE>>&, TOOL_WORKER_CLASS*, COMMS&) {}

// ---- TOOL_NEWS ----

void TOOL_NEWS::configure(ai_root& chat) {
    profile_dir = chat.PROPS.OLLI_DIRECTORY;
    load_info_settings(profile_dir);
}

void TOOL_NEWS::register_tool(ai_root&, json& tools) {
    tool_functions.clear();

    // The feed names come from info_settings.json, so a feed the user adds there is offered to the model on the next request.
    const INFO_SETTINGS settings = load_info_settings(profile_dir);
    std::string names;
    for (const auto& [name, url] : settings.news_feeds) names += (names.empty() ? "" : ", ") + name;

    json params = {
        {"type", "object"},
        {"properties", {
            {"feed", {{"type", "string"}, {"description", "Optional. Which news feed to read: " + names + ". Leave it out for '" + settings.default_news_feed + "'."}}},
            {"count", {{"type", "integer"}, {"description", "Optional. How many stories, 1 to 10. Default 5."}}}
        }},
        {"required", json::array()}
    };
    tool_functions.push_back("get_news");
    add_tool(tools, "get_news",
        "Gets the latest news headlines with a short summary of each story, from news feeds (NPR and BBC). "
        "Use this when the user asks what is in the news, instead of web_search. Links to the stories are shown to the user separately.",
        params);
}

std::pair<bool, std::string> TOOL_NEWS::get_news(const std::string& feed_arg, int count, COMMS& comms) {
    const INFO_SETTINGS settings = load_info_settings(profile_dir);
    const std::string feed = trim_text(feed_arg).empty() ? settings.default_news_feed : to_lower(trim_text(feed_arg));

    std::string url;
    for (const auto& [name, u] : settings.news_feeds) if (name == feed) { url = u; break; }
    if (url.empty()) {
        std::string names;
        for (const auto& [name, u] : settings.news_feeds) names += (names.empty() ? "" : ", ") + name;
        return {false, "Error: there is no news feed called '" + feed + "'. Available feeds: " + names + "."};
    }

    const time_t now = std::time(nullptr);
    std::vector<NEWS_ITEM> stories;
    auto cached = cache.find(feed);
    if (cached != cache.end() && now - cached->second.first < settings.cache_seconds) {
        stories = cached->second.second;
    } else {
        auto [ok, body] = TOOL_WEB_SEARCH::curl_get(url, REQUEST_TIMEOUT_SECONDS, USER_AGENT, true);
        if (!ok) return {false, body};
        stories = parse_rss(body, NEWS_MAX_STORIES);
        if (stories.empty()) return {false, "Error: the '" + feed + "' feed had no stories in it (it may not be a normal RSS feed)."};
        cache[feed] = {now, stories};
    }

    count = std::clamp(count, 1, static_cast<int>(NEWS_MAX_STORIES));
    if (static_cast<size_t>(count) < stories.size()) stories.resize(static_cast<size_t>(count));

    std::string out = "Latest news - " + (stories[0].source.empty() ? feed : stories[0].source) + " (" + feed + " feed), " +
                      std::to_string(stories.size()) + " stories, newest first:\n";
    for (size_t i = 0; i < stories.size(); ++i) {
        out += std::to_string(i + 1) + ". " + stories[i].title + "\n";
        if (!stories[i].description.empty()) out += "   " + stories[i].description + "\n";
        if (!stories[i].published.empty())   out += "   (published " + stories[i].published + ")\n";
    }

    // Each story's link is shown to the user as a link, same as web_search's results.
    {
        std::lock_guard<std::mutex> lock(output_buffer_mutex);
        for (const auto& s : stories)
            if (!s.link.empty()) comms.TOOL_ATTACHMENTS.emplace_back("link", s.title, s.link);
    }
    return {true, out};
}

void TOOL_NEWS::handle_tool(ai_root& chat, TOOL_WORKER_CLASS* tool_worker, COMMS& comms, const json& args, const std::string& tc_id) {
    std::string feed = (args.contains("feed") && args.at("feed").is_string()) ? args.at("feed").get<std::string>() : "";

    // The model may send the count as a number or as text.
    int count = NEWS_DEFAULT_STORIES;
    if (args.contains("count")) {
        const auto& c = args.at("count");
        if (c.is_number()) count = static_cast<int>(c.get<double>());
        else if (c.is_string()) count = std::atoi(c.get<std::string>().c_str());
    }
    if (count < 1) count = NEWS_DEFAULT_STORIES;

    auto [ok, result] = get_news(feed, count, comms);

    chat.send_tool_result(tc_id, result);
    if (ok) chat.integrate_tool_result(tool_worker, comms, "", "News result: " + result);
    else    chat.integrate_tool_result(tool_worker, comms, FAILURE_INSTRUCTION, "The news lookup failed: " + result);
}

bool TOOL_NEWS::check(IO_WORKER_CLASS&, ai_root& chat, CLASS_SYSTEM*, std::vector<std::unique_ptr<TOOL_BASE>>&, TOOL_WORKER_CLASS* tool_worker, COMMS& comms, const ToolCall& tc) {
    if (tc.name != "get_news") return false;
    handle_tool(chat, tool_worker, comms, tc.arguments, tc.id);
    return true;
}

// No periodic work needed - part of the common tool interface (see the note in tools.h).
void TOOL_NEWS::monitor_tool(ai_root&, CLASS_SYSTEM*, std::vector<std::unique_ptr<TOOL_BASE>>&, TOOL_WORKER_CLASS*, COMMS&) {}
