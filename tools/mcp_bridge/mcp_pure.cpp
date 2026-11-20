#include "mcp_pure.hpp"

#include <algorithm>
#include <cctype>

using json = nlohmann::json;

namespace {
    constexpr size_t MAX_TOOL_NAME_BYTES = 64;
    constexpr size_t MAX_DESCRIPTION_BYTES = 500;
    constexpr size_t MAX_SCHEMA_BYTES = 8000;
    constexpr size_t MAX_SERVER_NAME_LENGTH = 24;

    // Plain getters that never throw and never use operator[] on a missing key.
    const json* member(const json& object, const char* key)
    {
        if (!object.is_object()) return nullptr;
        auto it = object.find(key);
        return it == object.end() ? nullptr : &*it;
    }

    std::string string_member(const json& object, const char* key)
    {
        const json* value = member(object, key);
        return (value != nullptr && value->is_string()) ? value->get<std::string>() : std::string();
    }

    // The whole of nlohmann's dump(), but with invalid UTF-8 replaced instead of thrown.
    std::string safe_dump(const json& value)
    {
        return value.dump(-1, ' ', false, json::error_handler_t::replace);
    }

    bool is_env_name(const std::string& name)
    {
        if (name.empty()) return false;
        for (char ch : name) {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (!(std::isalnum(c) || c == '_')) return false;
        }
        return true;
    }
}

json default_config_json()
{
    MCP_BRIDGE_CONFIG defaults;
    return {
        {"max_result_chars", defaults.max_result_chars},
        {"call_timeout_seconds", defaults.call_timeout_seconds},
        {"servers", json::object()}
    };
}

std::string to_valid_utf8(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    const size_t n = text.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t length = 0;
        if (c < 0x80) length = 1;
        else if (c >= 0xC2 && c <= 0xDF) length = 2;
        else if (c >= 0xE0 && c <= 0xEF) length = 3;
        else if (c >= 0xF0 && c <= 0xF4) length = 4;

        bool ok = length != 0 && i + length <= n;
        if (ok && length > 1) {
            for (size_t k = 1; k < length; ++k) {
                if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) { ok = false; break; }
            }
        }
        if (ok && length == 3) {
            const unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
            if (c == 0xE0 && c1 < 0xA0) ok = false;      // overlong
            if (c == 0xED && c1 >= 0xA0) ok = false;     // UTF-16 surrogate
        }
        if (ok && length == 4) {
            const unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
            if (c == 0xF0 && c1 < 0x90) ok = false;      // overlong
            if (c == 0xF4 && c1 >= 0x90) ok = false;     // beyond U+10FFFF
        }

        if (ok) {
            out.append(text, i, length);
            i += length;
        } else {
            out += "\xEF\xBF\xBD";   // U+FFFD
            ++i;
        }
    }
    return out;
}

std::string truncate_utf8(const std::string& text, size_t max_bytes, bool& truncated)
{
    truncated = false;
    if (text.size() <= max_bytes) return text;
    truncated = true;
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return text.substr(0, cut);
}

std::string sanitize_identifier(const std::string& text)
{
    std::string out;
    for (char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        char mapped = std::isalnum(c) && c < 0x80 ? static_cast<char>(std::tolower(c)) : '_';
        if (mapped == '_' && (out.empty() || out.back() == '_')) continue;
        out += mapped;
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

namespace {
    // Code points that show nothing (or reorder what is shown) and so can hide text from a person.
    bool is_invisible_code_point(unsigned cp)
    {
        return (cp >= 0x80 && cp <= 0x9F)                 // C1 controls
            || (cp >= 0x200B && cp <= 0x200F)             // zero-width space/joiners, LRM/RLM
            || (cp >= 0x202A && cp <= 0x202E)             // bidi embeddings and overrides
            || (cp >= 0x2060 && cp <= 0x206F)             // word joiner, invisible operators, bidi isolates
            || cp == 0x2028 || cp == 0x2029               // line/paragraph separators
            || cp == 0xFEFF                               // byte order mark / zero-width no-break space
            || (cp >= 0xFFF9 && cp <= 0xFFFB)             // interlinear annotation
            || (cp >= 0xE0000 && cp <= 0xE007F);         // "tag" characters
    }

    // Input must already be valid UTF-8 (see to_valid_utf8()).
    std::string remove_invisible(const std::string& valid)
    {
        std::string out;
        out.reserve(valid.size());
        size_t i = 0;
        while (i < valid.size()) {
            const unsigned char c = static_cast<unsigned char>(valid[i]);
            size_t length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            unsigned cp = c < 0x80 ? c : c < 0xE0 ? (c & 0x1Fu) : c < 0xF0 ? (c & 0x0Fu) : (c & 0x07u);
            for (size_t k = 1; k < length && i + k < valid.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(valid[i + k]) & 0x3Fu);
            if (!is_invisible_code_point(cp)) out.append(valid, i, length);
            i += length;
        }
        return out;
    }
}

std::string clean_description(const std::string& text, size_t max_bytes)
{
    std::string spaced;
    for (char ch : remove_invisible(to_valid_utf8(text))) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F) c = ' ';
        if (c == ' ' && (spaced.empty() || spaced.back() == ' ')) continue;
        spaced += static_cast<char>(c);
    }
    while (!spaced.empty() && spaced.back() == ' ') spaced.pop_back();

    bool truncated = false;
    std::string cut = truncate_utf8(spaced, max_bytes, truncated);
    if (truncated) {
        while (!cut.empty() && cut.back() == ' ') cut.pop_back();
        cut += "...";
    }
    return cut;
}

bool json_too_deep(const std::string& json_text, int max_depth)
{
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (char c : json_text) {
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
        } else if (c == '"') {
            in_string = true;
        } else if (c == '[' || c == '{') {
            if (++depth > max_depth) return true;
        } else if ((c == ']' || c == '}') && depth > 0) {
            --depth;
        }
    }
    return false;
}

bool json_value_too_deep(const json& value, int max_depth)
{
    std::vector<std::pair<const json*, int>> pending;
    pending.push_back({&value, 1});
    while (!pending.empty()) {
        const auto [node, depth] = pending.back();
        pending.pop_back();
        if (!node->is_structured()) continue;
        if (depth > max_depth) return true;
        for (const json& child : *node) pending.push_back({&child, depth + 1});
    }
    return false;
}

MCP_BRIDGE_CONFIG parse_config(const json& config)
{
    MCP_BRIDGE_CONFIG out;
    if (!config.is_object()) {
        out.warnings.push_back("the config file is not a JSON object - using the defaults (no servers)");
        return out;
    }

    auto read_int = [&](const json& object, const char* key, int low, int high, int& target, const std::string& where) {
        const json* value = member(object, key);
        if (value == nullptr) return;
        if (!value->is_number_integer()) {
            out.warnings.push_back(where + "'" + key + "' must be a whole number - using " + std::to_string(target));
            return;
        }
        long long v = value->get<long long>();
        if (v < low || v > high) {
            out.warnings.push_back(where + "'" + key + "' must be between " + std::to_string(low) + " and " + std::to_string(high) + " - using " + std::to_string(target));
            return;
        }
        target = static_cast<int>(v);
    };

    read_int(config, "max_result_chars", 500, 100000, out.max_result_chars, "");
    read_int(config, "call_timeout_seconds", 5, 280, out.call_timeout_seconds, "");

    const json* servers = member(config, "servers");
    if (servers == nullptr) return out;
    if (!servers->is_object()) {
        out.warnings.push_back("'servers' must be an object of name -> settings - no servers loaded");
        return out;
    }

    for (auto it = servers->begin(); it != servers->end(); ++it) {
        const std::string name = it.key();
        const std::string where = "server '" + name + "': ";
        const json& s = it.value();

        if (name.empty() || name.size() > MAX_SERVER_NAME_LENGTH || sanitize_identifier(name) != name) {
            out.warnings.push_back("server '" + name + "' skipped: a server name must be 1-" + std::to_string(MAX_SERVER_NAME_LENGTH) +
                                   " characters, lower-case letters, digits and single underscores only");
            continue;
        }
        if (!s.is_object()) { out.warnings.push_back(where + "skipped: its settings must be an object"); continue; }

        MCP_SERVER_CONFIG server;
        server.name = name;

        const json* command = member(s, "command");
        if (command == nullptr || !command->is_array() || command->empty()) {
            out.warnings.push_back(where + "skipped: 'command' must be a non-empty list, e.g. [\"uvx\", \"mcp-server-time\"]");
            continue;
        }
        bool command_ok = true;
        for (const auto& part : *command) {
            if (!part.is_string() || part.get<std::string>().empty()) { command_ok = false; break; }
            server.command.push_back(part.get<std::string>());
        }
        if (!command_ok) {
            out.warnings.push_back(where + "skipped: every part of 'command' must be a non-empty string");
            continue;
        }

        if (const json* enabled = member(s, "enabled")) {
            if (enabled->is_boolean()) server.enabled = enabled->get<bool>();
            else out.warnings.push_back(where + "'enabled' must be true or false - using true");
        }

        if (const json* env = member(s, "env")) {
            if (!env->is_object()) {
                out.warnings.push_back(where + "'env' must be an object of NAME -> value - ignored");
            } else {
                for (auto e = env->begin(); e != env->end(); ++e) {
                    if (!is_env_name(e.key()) || !e.value().is_string()) {
                        out.warnings.push_back(where + "env entry '" + e.key() + "' ignored (needs a plain NAME and a string value)");
                        continue;
                    }
                    server.env[e.key()] = e.value().get<std::string>();
                }
            }
        }

        if (const json* cwd = member(s, "cwd")) {
            if (cwd->is_string()) server.cwd = cwd->get<std::string>();
            else out.warnings.push_back(where + "'cwd' must be a string - ignored");
        }

        if (const json* allow = member(s, "allow_tools")) {
            if (!allow->is_array()) {
                out.warnings.push_back(where + "'allow_tools' must be a list of tool names (or [\"*\"]) - no tools allowed");
            } else {
                for (const auto& entry : *allow) {
                    if (entry.is_string() && !entry.get<std::string>().empty()) server.allow_tools.push_back(entry.get<std::string>());
                    else out.warnings.push_back(where + "an 'allow_tools' entry that is not a non-empty string was ignored");
                }
            }
        }

        server.prefix = "mcp_" + name + "_";
        if (const json* prefix = member(s, "prefix")) {
            if (prefix->is_string()) {
                std::string cleaned = sanitize_identifier(prefix->get<std::string>());
                server.prefix = cleaned.empty() ? std::string() : cleaned + "_";
            } else {
                out.warnings.push_back(where + "'prefix' must be a string - using '" + server.prefix + "'");
            }
        }

        read_int(s, "startup_timeout_seconds", 5, 600, server.startup_timeout_seconds, where);

        out.servers.push_back(std::move(server));
    }
    return out;
}

bool tool_is_allowed(const MCP_SERVER_CONFIG& server, const std::string& real_name)
{
    for (const auto& entry : server.allow_tools) {
        if (entry == "*" || entry == real_name) return true;
    }
    return false;
}

namespace {
    constexpr int MAX_SCHEMA_DEPTH = 8;
    constexpr size_t MAX_SCHEMA_PROPERTIES = 50;
    constexpr size_t MAX_SCHEMA_TEXT_BYTES = 300;
    constexpr size_t MAX_ENUM_ENTRIES = 50;

    // A property name the model will see and send back: plain characters only.
    bool is_safe_property_name(const std::string& name)
    {
        if (name.empty() || name.size() > 64) return false;
        for (char ch : name) {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
        }
        return true;
    }

    // A value the model might copy back to the server (enum/const/default): kept only if cleaning
    // would not change it - altering it would make the model send something the server rejects.
    bool keep_literal(const json& value, json& out)
    {
        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            if (text.size() > 100 || clean_description(text, 100) != text) return false;
            out = value;
            return true;
        }
        if (value.is_number() || value.is_boolean() || value.is_null()) { out = value; return true; }
        return false;
    }

    // Copies only the JSON-Schema keywords a model needs to build a call, with every piece of
    // free text cleaned (this text goes straight into the model's tool list). false = the schema
    // cannot be offered safely (unsafe names, too deep, too many properties).
    bool clean_schema(const json& in, json& out, int depth, std::string& why)
    {
        if (depth > MAX_SCHEMA_DEPTH) { why = "its argument description is nested too deeply"; return false; }
        out = json::object();
        if (!in.is_object()) return true;

        for (auto it = in.begin(); it != in.end(); ++it) {
            const std::string& key = it.key();
            const json& value = it.value();

            if (key == "type") {
                if (value.is_string()) {
                    out["type"] = clean_description(value.get<std::string>(), 16);
                } else if (value.is_array()) {
                    json types = json::array();
                    for (const auto& t : value) {
                        if (t.is_string() && types.size() < 8) types.push_back(clean_description(t.get<std::string>(), 16));
                    }
                    out["type"] = types;
                }
            } else if (key == "description" || key == "title") {
                if (value.is_string()) {
                    std::string text = clean_description(value.get<std::string>(), MAX_SCHEMA_TEXT_BYTES);
                    if (!text.empty()) out[key] = text;
                }
            } else if (key == "enum") {
                if (!value.is_array() || value.size() > MAX_ENUM_ENTRIES) continue;
                json kept = json::array();
                for (const auto& entry : value) {
                    json literal;
                    if (!keep_literal(entry, literal)) { why = "an allowed value in its argument description holds odd characters"; return false; }
                    kept.push_back(literal);
                }
                out["enum"] = kept;
            } else if (key == "const" || key == "default") {
                json literal;
                if (keep_literal(value, literal)) out[key] = literal;
            } else if (key == "format") {
                if (value.is_string() && value.get<std::string>().size() <= 32 && is_safe_property_name(value.get<std::string>())) out["format"] = value;
            } else if (key == "minimum" || key == "maximum" || key == "minLength" || key == "maxLength" || key == "minItems" || key == "maxItems") {
                if (value.is_number()) out[key] = value;
            } else if (key == "additionalProperties") {
                if (value.is_boolean()) out[key] = value;
            } else if (key == "required") {
                if (!value.is_array()) continue;
                json names = json::array();
                for (const auto& n : value) {
                    if (n.is_string() && is_safe_property_name(n.get<std::string>())) names.push_back(n);
                }
                out["required"] = names;
            } else if (key == "properties") {
                if (!value.is_object()) continue;
                if (value.size() > MAX_SCHEMA_PROPERTIES) { why = "it has more than " + std::to_string(MAX_SCHEMA_PROPERTIES) + " arguments"; return false; }
                json properties = json::object();
                for (auto p = value.begin(); p != value.end(); ++p) {
                    if (!is_safe_property_name(p.key())) { why = "one of its arguments has an odd name"; return false; }
                    json cleaned;
                    if (!clean_schema(p.value(), cleaned, depth + 1, why)) return false;
                    properties[p.key()] = std::move(cleaned);
                }
                out["properties"] = std::move(properties);
            } else if (key == "items") {
                if (!value.is_object()) continue;
                json cleaned;
                if (!clean_schema(value, cleaned, depth + 1, why)) return false;
                out["items"] = std::move(cleaned);
            } else if (key == "anyOf" || key == "oneOf" || key == "allOf") {
                if (!value.is_array() || value.size() > 8) continue;
                json options = json::array();
                for (const auto& option : value) {
                    json cleaned;
                    if (!clean_schema(option, cleaned, depth + 1, why)) return false;
                    options.push_back(std::move(cleaned));
                }
                out[key] = std::move(options);
            }
            // anything else ($schema, $ref, pattern, examples, vendor extensions...) is not passed on
        }
        return true;
    }
}

bool convert_tool(const MCP_SERVER_CONFIG& server, const json& mcp_tool, EXPOSED_TOOL& out, std::string& why_not)
{
    const std::string real_name = string_member(mcp_tool, "name");
    if (real_name.empty()) { why_not = "the tool has no name"; return false; }

    const std::string middle = sanitize_identifier(real_name);
    if (middle.empty()) { why_not = "its name has no usable letters or digits"; return false; }

    const std::string exposed = server.prefix + middle;
    if (exposed.size() > MAX_TOOL_NAME_BYTES) { why_not = "its name is longer than " + std::to_string(MAX_TOOL_NAME_BYTES) + " characters once prefixed"; return false; }

    std::string description = clean_description(string_member(mcp_tool, "description"), MAX_DESCRIPTION_BYTES);
    if (description.empty()) description = clean_description(string_member(mcp_tool, "title"), MAX_DESCRIPTION_BYTES);
    if (description.empty()) description = "no description given";
    description = "[MCP server '" + server.name + "'] " + description;

    json parameters = json::object();
    if (const json* schema = member(mcp_tool, "inputSchema")) {
        if (!clean_schema(*schema, parameters, 0, why_not)) return false;
    }
    parameters["type"] = "object";
    if (!parameters.contains("properties") || !parameters["properties"].is_object()) parameters["properties"] = json::object();
    if (safe_dump(parameters).size() > MAX_SCHEMA_BYTES) {
        why_not = "its argument description is larger than " + std::to_string(MAX_SCHEMA_BYTES) + " bytes";
        return false;
    }

    out.exposed_name = exposed;
    out.server = server.name;
    out.real_name = real_name;
    out.definition = {
        {"name", exposed},
        {"description", description},
        {"parameters", parameters}
    };
    return true;
}

FLATTENED_RESULT flatten_call_result(const json& result, size_t max_chars)
{
    FLATTENED_RESULT out;
    if (const json* is_error = member(result, "isError")) {
        if (is_error->is_boolean()) out.is_error = is_error->get<bool>();
    }

    std::vector<std::string> parts;
    bool have_text = false;

    if (const json* content = member(result, "content")) {
        if (content->is_array()) {
            for (const auto& block : *content) {
                const std::string type = string_member(block, "type");
                if (type == "text") {
                    parts.push_back(string_member(block, "text"));
                    have_text = true;
                } else if (type == "image" || type == "audio") {
                    std::string mime = string_member(block, "mimeType");
                    parts.push_back("[" + type + (mime.empty() ? "" : " (" + mime + ")") + " came back but cannot be shown here]");
                } else if (type == "resource_link") {
                    std::string title = string_member(block, "title");
                    if (title.empty()) title = string_member(block, "name");
                    std::string uri = string_member(block, "uri");
                    parts.push_back("Link: " + (title.empty() ? uri : title + " - " + uri));
                    have_text = true;
                } else if (type == "resource") {
                    const json* resource = member(block, "resource");
                    std::string text = resource != nullptr ? string_member(*resource, "text") : std::string();
                    if (!text.empty()) {
                        parts.push_back(text);
                        have_text = true;
                    } else {
                        std::string uri = resource != nullptr ? string_member(*resource, "uri") : std::string();
                        parts.push_back("[a resource" + (uri.empty() ? std::string() : " (" + uri + ")") + " came back without text]");
                    }
                } else {
                    parts.push_back("[content of type '" + (type.empty() ? std::string("unknown") : to_valid_utf8(type)) + "' not shown]");
                }
            }
        }
    }

    if (!have_text) {
        if (const json* structured = member(result, "structuredContent")) {
            parts.clear();
            parts.push_back(safe_dump(*structured));
        }
    }

    std::string joined;
    for (const auto& part : parts) {
        if (!joined.empty()) joined += "\n";
        joined += part;
    }
    if (joined.empty()) joined = "(the tool returned nothing)";

    joined = to_valid_utf8(joined);
    bool truncated = false;
    out.text = truncate_utf8(joined, max_chars, truncated);
    if (truncated) out.text += "\n[...cut off: the tool's answer was longer than " + std::to_string(max_chars) + " characters]";
    return out;
}
