// Mail - an olli remote tool that watches one Gmail inbox, read-only, and runs a
// standing rule the user set ("when new mail arrives, do X") - see README.md.
//
// First version, deliberately small:
//   - one callable tool, set_mail_action: set / show / clear the standing rule.
//   - once an hour (--poll-seconds) it asks Gmail for the inbox's UIDNEXT and
//     UIDVALIDITY with a single IMAP STATUS command. That is all it ever asks:
//     it never selects the mailbox, never fetches a message, never reads a
//     subject, sender or body, and has no way to send, move or delete anything.
//     "New mail" means UIDNEXT went up since the last check.
//   - when it did, and a rule is set, it sends olli one `event` (the same shape
//     the clock uses for a finished timer): an optional message for olli to say
//     in character and an optional `action`, a prepared tool call olli runs
//     itself. The action is written by the user when the rule is set, so an
//     email can trigger it but can never choose or change it.
//
// Talks to olli (port 47601) and, like every tool, to the subcon tool's server
// (47603), see ../PROTOCOL.md. Events always go to olli; subcon can reach this
// tool but set_mail_action is not on its read-only allowlist.
//
// Credentials: <profile folder>/mail/credentials.json, which must be readable by
// its owner only (chmod 600) or the tool refuses to use it. A Gmail app password
// is the simplest thing that works (Google account -> Security -> 2-Step
// Verification -> App passwords).
//
// Build: `make` in this directory. Run: `./mail [host]` - does not need olli to
// already be running.

#include <nlohmann/json.hpp>

#include "olli_link.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
    // ---------------------------------------------------------------------
    // Fixed on purpose. This tool only ever talks to Gmail over TLS. The
    // loopback override below exists so the tool can be tested against a fake
    // IMAP server, and cannot point anywhere else.
    // ---------------------------------------------------------------------
    const char* const GMAIL_IMAPS_URL = "imaps://imap.gmail.com:993/";

    struct Options {
        std::string host = "127.0.0.1";   // olli
        int poll_seconds = 3600;          // once an hour
        bool test_plain_imap = false;     // --test-imap-port: plain IMAP, loopback only
        int test_imap_port = 0;
        int olli_port = OLLI_TOOL_PORT;   // --olli-port / --subcon-port: only for tests against fake servers
        int subcon_port = SUBCON_TOOL_PORT;
    };

    std::atomic<bool> quit_requested{false};
    void handle_signal(int) { quit_requested.store(true); }

    std::string timestamp()
    {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_r(&t, &tm);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
        return buf;
    }

    void say(const std::string& line)
    {
        std::cout << timestamp() << "  " << line << std::endl;
    }

    // ---------------------------------------------------------------------
    // Files: <profile folder>/mail/{credentials.json,state.json,rule.json}
    // ---------------------------------------------------------------------
    fs::path home_dir()
    {
        const char* home = std::getenv("HOME");
        return fs::path(home ? home : ".");
    }

    // Same ~/olli_files_<name>/ (or shared ~/olli_files/ for an empty name)
    // convention as the other tools, so this lives next to olli's own files.
    fs::path mail_dir_for(const std::string& profile_name)
    {
        std::string dir_name = profile_name.empty() ? "olli_files" : "olli_files_" + profile_name;
        return home_dir() / dir_name / "mail";
    }

    // Written to a temp file and renamed over the target, so a crash mid-write
    // can never leave half a file behind. mode: the permissions of the new file.
    bool write_file_atomic(const fs::path& path, const std::string& content, mode_t mode)
    {
        fs::path tmp = path;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out) return false;
            out << content;
            if (!out) return false;
        }
        if (chmod(tmp.c_str(), mode) != 0) { std::error_code ec; fs::remove(tmp, ec); return false; }
        std::error_code ec;
        fs::rename(tmp, path, ec);
        return !ec;
    }

    bool read_json_file(const fs::path& path, json& out)
    {
        std::ifstream in(path);
        if (!in) return false;
        try {
            in >> out;
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    // Reads a string out of untrusted JSON without throwing on a wrong type.
    std::string json_string(const json& j, const char* key)
    {
        if (!j.is_object() || !j.contains(key) || !j[key].is_string()) return "";
        return j[key].get<std::string>();
    }

    struct Credentials {
        std::string user;
        std::string app_password;
        bool usable() const { return !user.empty() && !app_password.empty(); }
    };

    enum class CredentialsState { MISSING_CREATED, NOT_FILLED_IN, TOO_OPEN, UNREADABLE, OK };

    // The file holds a password, so it must not be readable by anyone else.
    // Creates a placeholder (owner-only) the first time, like the presence tool
    // does for its own settings.
    CredentialsState load_credentials(const fs::path& dir, Credentials& out)
    {
        std::error_code ec;
        fs::create_directories(dir, ec);
        chmod(dir.c_str(), 0700);

        fs::path path = dir / "credentials.json";
        if (!fs::exists(path)) {
            json placeholder = {{"user", ""}, {"app_password", ""}};
            write_file_atomic(path, placeholder.dump(2) + "\n", 0600);
            return CredentialsState::MISSING_CREATED;
        }

        struct stat st{};
        if (stat(path.c_str(), &st) != 0) return CredentialsState::UNREADABLE;
        if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0) return CredentialsState::TOO_OPEN;

        json j;
        if (!read_json_file(path, j)) return CredentialsState::UNREADABLE;

        out.user = json_string(j, "user");
        // Google shows an app password in four groups with spaces; it works either way.
        std::string pw = json_string(j, "app_password");
        pw.erase(std::remove_if(pw.begin(), pw.end(), [](unsigned char c) { return std::isspace(c) != 0; }), pw.end());
        out.app_password = pw;
        return out.usable() ? CredentialsState::OK : CredentialsState::NOT_FILLED_IN;
    }

    std::string describe_credentials_problem(CredentialsState s, const fs::path& dir)
    {
        fs::path path = dir / "credentials.json";
        switch (s) {
            case CredentialsState::MISSING_CREATED:
                return "No Gmail credentials yet - created " + path.string() + ". Put the Gmail address and an app password in it.";
            case CredentialsState::NOT_FILLED_IN:
                return "Gmail credentials file is not filled in yet: " + path.string();
            case CredentialsState::TOO_OPEN:
                return "Refusing to use " + path.string() + " - other users can read it. Run: chmod 600 " + path.string();
            case CredentialsState::UNREADABLE:
                return "Could not read " + path.string() + " (missing or not valid JSON).";
            case CredentialsState::OK:
                break;
        }
        return "";
    }

    // What the tool remembers between runs. uidnext is the next UID the inbox
    // will hand out, so "uidnext went up" means mail arrived. UIDs are only
    // valid within one uidvalidity; if that changes, the baseline starts over.
    struct MailState {
        bool have_baseline = false;
        long long uidvalidity = 0;
        long long uidnext = 0;
        bool pending = false; // new mail seen but its event has not gone out yet
    };

    MailState load_state(const fs::path& dir)
    {
        MailState s;
        json j;
        if (!read_json_file(dir / "state.json", j) || !j.is_object()) return s;
        try {
            s.have_baseline = j.value("have_baseline", false);
            s.uidvalidity = j.value("uidvalidity", 0LL);
            s.uidnext = j.value("uidnext", 0LL);
            s.pending = j.value("pending", false);
        } catch (const std::exception&) {
            return MailState();
        }
        return s;
    }

    void save_state(const fs::path& dir, const MailState& s)
    {
        json j = {{"have_baseline", s.have_baseline}, {"uidvalidity", s.uidvalidity}, {"uidnext", s.uidnext}, {"pending", s.pending}};
        write_file_atomic(dir / "state.json", j.dump(2) + "\n", 0600);
    }

    // The standing rule. Empty means "do nothing".
    struct MailRule {
        std::string announce;      // what olli should tell the user, in character
        std::string action_tool;   // a tool olli runs itself
        json action_arguments = json::object();
        bool empty() const { return announce.empty() && action_tool.empty(); }
    };

    MailRule load_rule(const fs::path& dir)
    {
        MailRule r;
        json j;
        if (!read_json_file(dir / "rule.json", j) || !j.is_object()) return r;
        r.announce = json_string(j, "announce");
        r.action_tool = json_string(j, "action_tool");
        if (j.contains("action_arguments") && j["action_arguments"].is_object()) r.action_arguments = j["action_arguments"];
        return r;
    }

    void save_rule(const fs::path& dir, const MailRule& r)
    {
        std::error_code ec;
        if (r.empty()) { fs::remove(dir / "rule.json", ec); return; }
        json j = {{"announce", r.announce}, {"action_tool", r.action_tool}, {"action_arguments", r.action_arguments}};
        write_file_atomic(dir / "rule.json", j.dump(2) + "\n", 0600);
    }

    std::string describe_rule(const MailRule& r)
    {
        if (r.empty()) return "No rule is set - nothing happens when mail arrives.";
        std::string s = "When new mail arrives: ";
        if (!r.announce.empty()) s += "tell the user \"" + r.announce + "\"";
        if (!r.announce.empty() && !r.action_tool.empty()) s += " and ";
        if (!r.action_tool.empty()) s += "run " + r.action_tool + "(" + r.action_arguments.dump() + ")";
        return s + ".";
    }

    // ---------------------------------------------------------------------
    // The one thing this tool asks Gmail: STATUS INBOX (UIDNEXT UIDVALIDITY).
    // Runs on its own thread (std::async) - a slow or hung connection must
    // never stall the main loop, since a tool that misses olli's heartbeat gets
    // dropped (see ../PROTOCOL.md).
    // ---------------------------------------------------------------------
    struct ImapResult {
        bool ok = false;
        bool login_denied = false; // wrong/revoked password - do not retry soon
        std::string error;
        long long uidnext = 0;
        long long uidvalidity = 0;
    };

    size_t collect_reply(char* data, size_t size, size_t nmemb, void* userdata)
    {
        auto* buf = static_cast<std::string*>(userdata);
        size_t n = size * nmemb;
        if (buf->size() + n > 65536) return 0; // a STATUS reply is a few dozen bytes; abort anything larger
        buf->append(data, n);
        return n;
    }

    // Finds "<keyword> <number>" in the reply, e.g. "UIDNEXT 1234".
    bool find_number_after(const std::string& text, const std::string& keyword, long long& out)
    {
        size_t pos = text.find(keyword);
        while (pos != std::string::npos) {
            size_t i = pos + keyword.size();
            if (i < text.size() && text[i] == ' ') {
                ++i;
                size_t start = i;
                while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
                if (i > start && i - start < 18) {
                    out = std::stoll(text.substr(start, i - start));
                    return true;
                }
            }
            pos = text.find(keyword, pos + 1);
        }
        return false;
    }

    ImapResult check_inbox(Credentials creds, bool plain_test, int test_port)
    {
        ImapResult result;
        CURL* curl = curl_easy_init();
        if (!curl) { result.error = "could not start libcurl"; return result; }

        std::string url = plain_test ? "imap://127.0.0.1:" + std::to_string(test_port) + "/" : GMAIL_IMAPS_URL;
        std::string reply;
        char curl_error[CURL_ERROR_SIZE] = {0};

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERNAME, creds.user.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, creds.app_password.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "STATUS INBOX (UIDNEXT UIDVALIDITY)");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect_reply);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        // Only the IMAP protocol, and only over TLS (certificate and host name
        // checked - libcurl's defaults, set explicitly so a change is visible).
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, plain_test ? "imap" : "imaps");
#else
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS, plain_test ? CURLPROTO_IMAP : CURLPROTO_IMAPS);
#endif
        if (!plain_test) {
            curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        }

        CURLcode code = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (code != CURLE_OK) {
            result.login_denied = (code == CURLE_LOGIN_DENIED);
            result.error = std::string(curl_easy_strerror(code)) + (curl_error[0] ? std::string(" (") + curl_error + ")" : "");
            return result;
        }
        if (!find_number_after(reply, "UIDNEXT", result.uidnext) || !find_number_after(reply, "UIDVALIDITY", result.uidvalidity)) {
            result.error = "unexpected reply to STATUS (no UIDNEXT/UIDVALIDITY)";
            return result;
        }
        result.ok = true;
        return result;
    }

    // ---------------------------------------------------------------------
    // The tool: one profile's worth of state, the standing rule, the polling.
    // ---------------------------------------------------------------------
    class MAIL_TOOL {
        public:
            explicit MAIL_TOOL(const Options& opt) : opt_(opt) {}

            bool profile_loaded() const { return profile_loaded_; }

            // An "identity" message from either server names the profile. The
            // two servers send the same name; only a real change reloads.
            void handle_identity(const json& msg)
            {
                std::string name = json_string(msg, "name");
                if (profile_loaded_ && name == profile_name_) return;
                profile_name_ = name;
                dir_ = mail_dir_for(name);
                std::error_code ec;
                fs::create_directories(dir_, ec);
                chmod(dir_.c_str(), 0700);

                state_ = load_state(dir_);
                rule_ = load_rule(dir_);
                creds_ = Credentials();
                last_creds_state_ = CredentialsState::OK;
                profile_loaded_ = true;
                next_check_ = std::chrono::steady_clock::now(); // look right away
                say("profile: " + (name.empty() ? std::string("(shared default)") : name) + " - files in " + dir_.string() + ". " + describe_rule(rule_));
            }

            std::string setup_note()
            {
                if (!profile_loaded_) return "";
                Credentials c;
                CredentialsState s = load_credentials(dir_, c);
                return s == CredentialsState::OK ? "" : " Note: " + describe_credentials_problem(s, dir_);
            }

            // set_mail_action: no arguments = show the rule; clear = remove it;
            // otherwise the given announce/action replace the rule.
            std::string handle_set_mail_action(const json& args, bool& is_error)
            {
                is_error = false;
                if (!profile_loaded_) {
                    is_error = true;
                    return "The mail tool has not been told which profile it is working for yet. Try again in a moment.";
                }

                bool clear = args.is_object() && args.contains("clear") && args["clear"].is_boolean() && args["clear"].get<bool>();
                std::string announce = json_string(args, "announce");
                std::string tool = json_string(args, "action_tool");

                if (clear) {
                    rule_ = MailRule();
                    save_rule(dir_, rule_);
                    say("rule cleared");
                    return "Rule cleared. Nothing happens when new mail arrives." + setup_note();
                }
                if (announce.empty() && tool.empty()) {
                    return describe_rule(rule_) + setup_note();
                }

                if (announce.size() > 500) {
                    is_error = true;
                    return "The announcement is too long (500 characters at most).";
                }
                MailRule r;
                r.announce = announce;
                r.action_tool = tool;
                if (!tool.empty()) {
                    if (tool == "set_mail_action") {
                        is_error = true;
                        return "A rule cannot run set_mail_action itself.";
                    }
                    if (args.contains("action_arguments")) {
                        if (!args["action_arguments"].is_object()) {
                            is_error = true;
                            return "action_arguments must be an object, in the same shape as when calling that tool directly.";
                        }
                        r.action_arguments = args["action_arguments"];
                    }
                }
                rule_ = r;
                save_rule(dir_, rule_);
                say("rule set: " + describe_rule(rule_));
                return "Standing rule saved. " + describe_rule(rule_) + " It stays in force until changed or cleared." + setup_note();
            }

            // Called every loop tick. Starts a check when one is due, collects a
            // finished one, and sends the event when there is one to send.
            void tick(OLLI_LINK& olli_link)
            {
                if (!profile_loaded_) return;
                auto now = std::chrono::steady_clock::now();

                // 1. collect a finished check
                if (check_.valid() && check_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    on_check_result(check_.get());
                }

                // 2. start one when due
                if (!check_.valid() && now >= next_check_) {
                    CredentialsState s = load_credentials(dir_, creds_);
                    if (s != CredentialsState::OK) {
                        // Logged when the problem first appears or changes, not on every look.
                        if (s != last_creds_state_) say(describe_credentials_problem(s, dir_));
                        last_creds_state_ = s;
                        next_check_ = now + std::chrono::seconds(std::min(30, opt_.poll_seconds)); // cheap file check, not a login
                    } else {
                        last_creds_state_ = CredentialsState::OK;
                        Credentials creds = creds_;
                        bool plain = opt_.test_plain_imap;
                        int port = opt_.test_imap_port;
                        check_ = std::async(std::launch::async, [creds, plain, port]() { return check_inbox(creds, plain, port); });
                    }
                }

                // 3. send the event for new mail, once
                if (state_.pending) {
                    if (rule_.empty()) {
                        state_.pending = false; // no rule: do nothing
                        save_state(dir_, state_);
                    } else if (olli_link.is_connected()) {
                        send_rule_event(olli_link);
                        state_.pending = false;
                        save_state(dir_, state_);
                    }
                }
            }

            // Waits for a check still in flight so shutdown never leaves a
            // thread running against a destroyed object.
            void finish()
            {
                if (check_.valid()) check_.wait();
            }

        private:
            void on_check_result(const ImapResult& r)
            {
                auto now = std::chrono::steady_clock::now();
                if (!r.ok) {
                    say("check failed: " + r.error);
                    // A wrong or revoked password must not be retried quickly -
                    // repeated failed logins are how an account gets locked.
                    next_check_ = now + std::chrono::seconds(r.login_denied ? std::max(opt_.poll_seconds, 3600) : std::max(opt_.poll_seconds / 4, 300));
                    return;
                }
                next_check_ = now + std::chrono::seconds(opt_.poll_seconds);

                if (!state_.have_baseline || state_.uidvalidity != r.uidvalidity) {
                    say(std::string(state_.have_baseline ? "inbox was renumbered by the server" : "first check") + " - baseline set (next UID " + std::to_string(r.uidnext) + "), no event");
                    state_.have_baseline = true;
                    state_.uidvalidity = r.uidvalidity;
                    state_.uidnext = r.uidnext;
                    state_.pending = false;
                    save_state(dir_, state_);
                    return;
                }
                if (r.uidnext > state_.uidnext) {
                    long long fresh = r.uidnext - state_.uidnext;
                    say("new mail: " + std::to_string(fresh) + " since the last check");
                    state_.uidnext = r.uidnext;
                    state_.pending = true;
                    save_state(dir_, state_);
                } else {
                    say("checked - nothing new");
                }
            }

            // Same event shape the clock sends for a finished timer. An empty
            // message means "no narration" (olli skips it) and the action, if
            // any, is queued for olli to run itself.
            void send_rule_event(OLLI_LINK& olli_link)
            {
                std::string message;
                if (!rule_.announce.empty()) {
                    message = "New email has arrived in the inbox. The user asked to be told: \"" + rule_.announce +
                              "\". Say that to them in character.";
                }
                json action = nullptr;
                if (!rule_.action_tool.empty()) {
                    action = {{"tool", rule_.action_tool}, {"arguments", rule_.action_arguments}};
                }
                olli_link.send_event(message, action);
                say("rule fired: " + describe_rule(rule_));
            }

            Options opt_;
            bool profile_loaded_ = false;
            std::string profile_name_;
            fs::path dir_;
            Credentials creds_;
            CredentialsState last_creds_state_ = CredentialsState::OK;
            MailState state_;
            MailRule rule_;
            std::future<ImapResult> check_;
            std::chrono::steady_clock::time_point next_check_ = std::chrono::steady_clock::now();
    };

    json make_register_message()
    {
        return {
            {"type", "register"},
            {"tools", json::array({
                {
                    {"name", "set_mail_action"},
                    {"description",
                     "Sets, shows or clears the standing rule for what happens when a new email arrives in the user's Gmail inbox. "
                     "The mail tool checks the inbox about once an hour; it is read-only and never tells anyone who the mail is from or what it says. "
                     "Use it when the user says things like \"when I get a new email, tell me\", \"when mail comes in, blink the lights\" or \"stop telling me about email\". "
                     "Give 'announce' to have the user told something in character (for example \"You have mail!\"). "
                     "Give 'action_tool' and 'action_arguments' to have another tool run automatically at that moment, in exactly the same shape you would use calling that tool directly "
                     "(for example action_tool set_hue_light). You can give both. "
                     "Set 'clear' to true for \"do nothing when mail arrives\". Call it with no arguments to see the current rule. "
                     "Setting a new rule replaces the old one, and the rule stays in force until it is changed."},
                    {"parameters", {
                        {"type", "object"},
                        {"properties", {
                            {"announce", {{"type", "string"}, {"description", "Optional: what to tell the user when new mail arrives, in character. Narration only - it does not do anything else."}}},
                            {"action_tool", {{"type", "string"}, {"description", "Optional: the name of another registered tool to run automatically when new mail arrives."}}},
                            {"action_arguments", {{"type", "object"}, {"description", "The arguments object for action_tool, in the same shape as calling that tool directly."}}},
                            {"clear", {{"type", "boolean"}, {"description", "True to remove the rule so nothing happens when mail arrives."}}}
                        }},
                        {"required", json::array()}
                    }}
                }
            })}
        };
    }

    void print_usage(const char* argv0)
    {
        std::string prog = argv0;
        auto slash = prog.find_last_of('/');
        if (slash != std::string::npos) prog = prog.substr(slash + 1);

        std::cout << "Usage: " << prog << " [host] [--poll-seconds N] [-h|--help]\n\n"
                     "  host              IP address of the machine running olli. Defaults to\n"
                     "                    127.0.0.1 (olli running on this same machine).\n\n"
                     "  --poll-seconds N  How often to check the inbox. Default 3600 (once an hour).\n\n"
                     "  -h, --help        Show this help and exit.\n\n"
                     "Credentials go in <profile folder>/mail/credentials.json (owner-only).\n";
    }
}

int main(int argc, char* argv[])
{
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") { print_usage(argv[0]); return 0; }
        else if (arg == "--poll-seconds" && i + 1 < argc) { opt.poll_seconds = std::max(1, std::atoi(argv[++i])); }
        else if (arg == "--test-imap-port" && i + 1 < argc) { opt.test_plain_imap = true; opt.test_imap_port = std::atoi(argv[++i]); }
        else if (arg == "--olli-port" && i + 1 < argc) { opt.olli_port = std::atoi(argv[++i]); }
        else if (arg == "--subcon-port" && i + 1 < argc) { opt.subcon_port = std::atoi(argv[++i]); }
        else if (!arg.empty() && arg[0] != '-') { opt.host = arg; }
        else { std::cerr << "Unknown argument: " << arg << "\n\n"; print_usage(argv[0]); return 1; }
    }

    in_addr host_addr{};
    if (inet_pton(AF_INET, opt.host.c_str(), &host_addr) != 1) {
        std::cerr << "Not a valid IPv4 address: " << opt.host << "\n\n";
        print_usage(argv[0]);
        return 1;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    OLLI_LINK link(opt.host, host_addr, make_register_message(), opt.olli_port);
    OLLI_LINK subcon_link(opt.host, host_addr, make_register_message(), opt.subcon_port);

    MAIL_TOOL mail(opt);
    say("mail started" + std::string(opt.test_plain_imap ? " [TEST MODE: plain IMAP on localhost]" : "") +
        ", checking every " + std::to_string(opt.poll_seconds) + " s");

    std::string last_link_status;

    auto handle_message = [&](OLLI_LINK& from, const json& msg) {
        std::string type = json_string(msg, "type");
        if (type == "identity") {
            mail.handle_identity(msg);
        } else if (type == "call") {
            std::string call_id = json_string(msg, "call_id");
            std::string name = json_string(msg, "name");
            if (name == "set_mail_action") {
                bool is_error = false;
                json args = msg.contains("arguments") ? msg["arguments"] : json::object();
                std::string text = mail.handle_set_mail_action(args, is_error);
                if (is_error) from.send_error(call_id, text);
                else from.send_result(call_id, text);
            } else {
                from.send_error(call_id, "Unknown tool name: " + name);
            }
        }
    };

    auto service_link = [&](OLLI_LINK& l, bool readable, bool is_olli) {
        l.service(readable);
        json msg;
        while (l.next_message(msg)) handle_message(l, msg);
        if (is_olli && !l.status().empty() && l.status() != last_link_status) {
            last_link_status = l.status();
            say(last_link_status);
        }
        l.consume_disconnected(); // nothing to reset: olli going away changes nothing we hold
    };

    while (!quit_requested.load()) {
        // One bad message must not end the process (nlohmann's value() throws on a
        // wrongly-typed field and an exception leaving main() is std::terminate).
        try {
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 200000;

            fd_set read_fds;
            FD_ZERO(&read_fds);
            int max_fd = -1;
            if (link.fd() >= 0) { FD_SET(link.fd(), &read_fds); max_fd = std::max(max_fd, link.fd()); }
            if (subcon_link.fd() >= 0) { FD_SET(subcon_link.fd(), &read_fds); max_fd = std::max(max_fd, subcon_link.fd()); }

            int ready = select(max_fd + 1, &read_fds, nullptr, nullptr, &tv);
            if (ready < 0) { FD_ZERO(&read_fds); ready = 0; } // a signal: run a normal tick

            service_link(link, link.fd() >= 0 && ready > 0 && FD_ISSET(link.fd(), &read_fds), true);
            service_link(subcon_link, subcon_link.fd() >= 0 && ready > 0 && FD_ISSET(subcon_link.fd(), &read_fds), false);

            mail.tick(link);
        } catch (const std::exception& e) {
            say(std::string("error in the main loop (continuing): ") + e.what());
            usleep(200000);
        }
    }

    say("shutting down");
    mail.finish();
    curl_global_cleanup();
    return 0;
}
