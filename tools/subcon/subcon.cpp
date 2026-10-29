// SUBCON - olli's background "subconscious" as a standalone remote tool.
// See README.md in this directory for the full picture; in short, this one
// program is BOTH:
//
//   (a) an ordinary remote tool of olli: one OLLI_LINK to olli's port 47601
//       registering a single tool, get_subcon_notes. Olli PULLS from it -
//       the answer is built instantly from the persisted note queue, no
//       model involved (reasoner.cpp, pull_notes()).
//   (b) a small server on port 47603 speaking the same protocol, so the
//       other tools (clock, hue, presence, rag) hold a second OLLI_LINK to
//       it and register their tools with it too. Subcon's think cycles call
//       those tools (tool_server.cpp), and presence events arriving there
//       feed its arrival tracking.
//
// Layout: tool_server.* (the server), reasoner.* (the think cycle + note
// queue), job_worker.* (the one thread behind slow model/web calls),
// ollama_client.* / internal_tools.* (what runs on that thread),
// subcon_store.* (persistence), subcon_tasks.h (task definitions and every
// time-based rule, pure and unit-testable), and this file: settings, the
// command line, the select() loop that ties them together, and the display.
//
// Threads: the main thread does everything except the blocking work (one
// JOB_WORKER thread, see job_worker.hpp for the handoff rules).
//
// Build: `make` in this directory. Run: `./subcon [host] [options]` - see
// `./subcon --help`. Does not need olli to already be running.

#include <nlohmann/json.hpp>

#include "olli_display.hpp"
#include "olli_link.hpp"

#include "job_worker.hpp"
#include "reasoner.hpp"
#include "tool_server.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>

#include <arpa/inet.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/file.h>
#include <sys/select.h>
#include <unistd.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
    std::atomic<bool> quit_requested{false};
    void on_signal(int) { quit_requested.store(true); }

    // --- Settings ---------------------------------------------------------
    //
    // <profile folder>/subcon_settings.json, same per-tool convention as
    // hue_settings.json / presence_settings.json. Optional: a missing file
    // means every default (the same ones olli itself uses for the model and
    // Ollama address); it is never created automatically. Command-line
    // options override it.
    struct SUBCON_SETTINGS {
        REASONER_CONFIG reasoner;
        int call_timeout_seconds = 30; // how long a connected tool may take to answer a call
    };

    SUBCON_SETTINGS load_settings(const fs::path& profile_dir, const std::function<void(const std::string&)>& say)
    {
        SUBCON_SETTINGS s;

        // The serpapi key lives in olli's own per-profile settings.json
        // (tool_web_search_apiKey, source/helper_olli.cpp), where the old
        // in-olli subcon got it from. Read as a fallback so an existing
        // profile works with no extra setup; subcon_settings.json can
        // override it.
        try {
            std::ifstream olli_settings(profile_dir / "settings.json");
            if (olli_settings) {
                json j; olli_settings >> j;
                s.reasoner.web_search_api_key = j.value("tool_web_search_apiKey", std::string());
            }
        } catch (const std::exception&) { /* unreadable: web_search then reports itself unconfigured */ }

        fs::path path = profile_dir / "subcon_settings.json";
        std::ifstream file(path);
        if (!file) return s;
        try {
            json j; file >> j;
            OLLAMA_SETTINGS& o = s.reasoner.ollama;
            o.host = j.value("ollama_host", o.host);
            o.port = j.value("ollama_port", o.port);
            o.model = j.value("model", o.model);
            o.num_ctx = j.value("num_ctx", o.num_ctx);
            o.repeat_penalty = j.value("repeat_penalty", o.repeat_penalty);
            o.keep_alive_seconds = j.value("keep_alive_seconds", o.keep_alive_seconds);
            o.think = j.value("think", o.think);
            s.reasoner.web_search_api_key = j.value("web_search_api_key", s.reasoner.web_search_api_key);
            s.reasoner.max_tool_rounds = j.value("max_tool_rounds", s.reasoner.max_tool_rounds);
            s.reasoner.rethink_seconds = j.value("rethink_seconds", s.reasoner.rethink_seconds);
            s.call_timeout_seconds = j.value("call_timeout_seconds", s.call_timeout_seconds);
        } catch (const std::exception& e) {
            say(std::string("couldn't read ") + path.filename().string() + " (" + e.what() + ") - using defaults");
        }
        return s;
    }

    fs::path profile_dir_for(const std::string& profile_name)
    {
        const char* home = std::getenv("HOME");
        std::string base = home ? home : ".";
        // Same convention as olli itself and every other tool: ~/olli_files_<name>/,
        // or the shared ~/olli_files/ for an empty name.
        return fs::path(base) / (profile_name.empty() ? "olli_files" : "olli_files_" + profile_name);
    }

    // --- Command line -------------------------------------------------------
    struct OPTIONS {
        std::string host = "127.0.0.1";
        int olli_port = OLLI_TOOL_PORT;
        int listen_port = SUBCON_TOOL_PORT;
        fs::path dir;               // --dir: the profile folder, pinned (ignores olli's identity for state)
        std::string profile;        // --profile: same, by name
        std::string user_name;      // --user-name: the name sent to tools in their identity message (default: --profile)
        bool think_cycle = true;
        bool force_run = false;
        bool ignore_limits = false;
        int first_cycle_delay = -1; // -1: leave the default
        // Overrides applied on top of the settings file:
        std::string model, ollama_host;
        int ollama_port = 0;
    };

    void print_usage(const char* argv0)
    {
        std::string prog = argv0;
        auto slash = prog.find_last_of('/');
        if (slash != std::string::npos) prog = prog.substr(slash + 1);

        std::cout << "Usage: " << prog << " [host] [options]\n\n"
            "  host                  IP address of the machine running olli. Defaults to\n"
            "                        127.0.0.1 (olli running on this same machine).\n\n"
            "Ports:\n"
            "  --olli-port N         olli's remote-tool port to register get_subcon_notes on (47601).\n"
            "  --listen-port N       port this tool serves other tools on (47603).\n\n"
            "State (default: learned from olli's identity message, like the other tools):\n"
            "  --profile NAME        pin to ~/olli_files_NAME/ instead of waiting for olli's identity.\n"
            "  --dir PATH            pin to this profile folder (state goes in PATH/subcon/).\n"
            "  --user-name NAME      the user name tools are told in their identity message (what olli would\n"
            "                        send as `name`). Default: --profile's name, else whatever olli's own\n"
            "                        identity message says (nothing is sent to tools until one of those is known).\n\n"
            "Model (defaults match olli's: qwen3:8b on localhost:11434):\n"
            "  --model NAME  --ollama-host HOST  --ollama-port N\n\n"
            "Switches:\n"
            "  --no-think-cycle      do not start new think cycles (queued notes can still be pulled).\n"
            "  --force-run           TEST ONLY: ignore the model's \"nothing worth doing\"; run the stalest task.\n"
            "  --test-ignore-limits  TEST ONLY: ignore per-task caps and minimum gaps.\n"
            "  --first-cycle-delay S seconds before the first cycle (default 60).\n\n"
            "  -h, --help            Show this help and exit.\n";
    }

    bool parse_args(int argc, char* argv[], OPTIONS& opt, std::string& error)
    {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto value = [&](std::string& out) -> bool {
                if (i + 1 >= argc) { error = arg + " needs a value"; return false; }
                out = argv[++i];
                return true;
            };
            auto number = [&](int& out) -> bool {
                std::string text;
                if (!value(text)) return false;
                try { out = std::stoi(text); } catch (const std::exception&) { error = arg + " needs a number, got '" + text + "'"; return false; }
                return true;
            };

            std::string text;
            if (arg == "--olli-port") { if (!number(opt.olli_port)) return false; }
            else if (arg == "--listen-port") { if (!number(opt.listen_port)) return false; }
            else if (arg == "--dir") { if (!value(text)) return false; opt.dir = text; }
            else if (arg == "--profile") { if (!value(opt.profile)) return false; }
            else if (arg == "--user-name") { if (!value(opt.user_name)) return false; }
            else if (arg == "--model") { if (!value(opt.model)) return false; }
            else if (arg == "--ollama-host") { if (!value(opt.ollama_host)) return false; }
            else if (arg == "--ollama-port") { if (!number(opt.ollama_port)) return false; }
            else if (arg == "--first-cycle-delay") { if (!number(opt.first_cycle_delay)) return false; }
            else if (arg == "--no-think-cycle") opt.think_cycle = false;
            else if (arg == "--force-run") opt.force_run = true;
            else if (arg == "--test-ignore-limits") opt.ignore_limits = true;
            else if (!arg.empty() && arg[0] == '-') { error = "unknown option " + arg; return false; }
            else opt.host = arg;
        }
        return true;
    }

    // --- State-directory lock ---------------------------------------------
    //
    // flock() on <state dir>/subcon.lock, held for as long as the process
    // runs (the kernel drops it if the process dies, so it can't go stale).
    // The port 47603 bind and olli's single-instance check already keep two
    // subcons apart on one machine, but neither covers a second subcon on
    // other ports, or a re-pinned profile: two writers of the same
    // todo_items.json / queue.json would each overwrite the other's run
    // caps and notes. The lock is on the state folder itself, so it holds
    // however the two were started. (The old in-olli subcon, which writes
    // the same files, does not take it - it has to be retired first, see
    // README.md.)
    class STATE_LOCK {
        public:
            ~STATE_LOCK() { release(); }

            // Takes the lock for 'state_dir', then lets go of the previous
            // one (so a profile switch is never briefly unprotected). True
            // if held afterwards; on false 'error' says who has it. Asking
            // for the directory already held is a no-op - a second flock()
            // on the same file from this process would refuse itself.
            bool acquire(const fs::path& state_dir, std::string& error)
            {
                fs::path lock_path = state_dir / "subcon.lock";
                if (fd >= 0 && lock_path == held_path) return true;

                int new_fd = open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
                if (new_fd < 0) { error = "couldn't open " + lock_path.string(); return false; }
                if (flock(new_fd, LOCK_EX | LOCK_NB) != 0) {
                    char buffer[32] = {};
                    ssize_t n = read(new_fd, buffer, sizeof(buffer) - 1);
                    std::string holder = n > 0 ? std::string(buffer, static_cast<size_t>(n)) : std::string();
                    while (!holder.empty() && std::isspace(static_cast<unsigned char>(holder.back()))) holder.pop_back();
                    error = lock_path.string() + " is locked by another subcon" + (holder.empty() ? "" : " (pid " + holder + ")");
                    close(new_fd);
                    return false;
                }
                // Our pid, only so the next one to be refused can say who has it.
                std::string pid = std::to_string(getpid()) + "\n";
                if (ftruncate(new_fd, 0) == 0) { ssize_t w = write(new_fd, pid.data(), pid.size()); (void)w; }

                release();
                fd = new_fd;
                held_path = lock_path;
                return true;
            }

        private:
            void release()
            {
                if (fd >= 0) close(fd); // closing drops the flock
                fd = -1;
            }

            int fd = -1;
            fs::path held_path;
    };

    // --- Logging + display ----------------------------------------------------
    //
    // Every line goes to <state dir>/subcon_log.txt (a plain-text record of
    // cycles, tool calls and findings - lines that arrive before the profile
    // is known are held and written once it is). On a terminal the newest
    // line also shows on OLLI_DISPLAY's status line; with output redirected
    // (nohup, tmux pipe, a test) it is printed to stdout instead, so the tool
    // runs headless with no display library involved.
    class SUBCON_LOG {
        public:
            void open_file(const fs::path& path)
            {
                file.close();
                std::error_code ec;
                // One generation of rotation is plenty for a plain-text debug aid.
                if (fs::exists(path, ec) && fs::file_size(path, ec) > MAX_LOG_BYTES)
                    fs::rename(path, fs::path(path.string() + ".old"), ec);
                file.open(path, std::ios::app);
                for (const std::string& line : held) file << line << "\n";
                file.flush();
                held.clear();
            }

            void write(const std::string& line, bool file_only)
            {
                char stamp[32];
                std::time_t now = std::time(nullptr);
                std::tm local{};
                localtime_r(&now, &local);
                std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S ", &local);
                std::string full = stamp + line;

                if (file.is_open()) { file << full << "\n"; file.flush(); }
                else held.push_back(full);

                if (!file_only) last_line = line;
                if (!file_only && !to_display) std::cout << full << std::endl;
            }

            bool to_display = false;
            std::string last_line; // newest non-file-only line, for the status line

        private:
            static constexpr std::uintmax_t MAX_LOG_BYTES = 5 * 1024 * 1024;
            std::ofstream file;
            std::vector<std::string> held;
    };

    constexpr int TOOL_AREA_HEIGHT = 9;

    // The tool area: what subcon is doing, what it knows, which tools are
    // connected. Redrawn only when the text changes.
    void draw_tool_area(OLLI_DISPLAY& display, const std::vector<std::string>& lines, std::string& last_drawn)
    {
        std::string joined;
        for (const std::string& l : lines) joined += l + "\n";
        if (joined == last_drawn) return;
        last_drawn = joined;

        WINDOW* win = display.tool_area();
        int height = 0, width = 0;
        getmaxyx(win, height, width);
        werase(win);
        for (int row = 0; row < height && row < static_cast<int>(lines.size()); ++row)
            mvwaddnstr(win, row, 0, lines[static_cast<size_t>(row)].c_str(), std::max(0, width - 1));
        display.refresh_tool_area();
    }

    json make_register_message()
    {
        return {
            {"type", "register"},
            {"tools", json::array({
                {
                    {"name", "get_subcon_notes"},
                    {"description", "The ONLY way to know what is on your mind: you cannot know without calling this tool, and you "
                                     "must not guess. Call it whenever the user asks if you have anything on your mind, anything "
                                     "else, whether anything came up, what you have been thinking about, or if you have any "
                                     "thoughts - every single time, even right after you already answered, because your background "
                                     "subconscious keeps working and a new note can appear at any moment. Also call it when the "
                                     "user returns or greets you after being away. It answers instantly with whatever it has "
                                     "noticed (weather, something left unresolved, an upcoming appointment, lights left on, a "
                                     "forgotten timer)."},
                    {"parameters", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}
                }
            })}
        };
    }
}

int main(int argc, char* argv[])
{
    OPTIONS opt;
    std::string error;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { print_usage(argv[0]); return 0; }
    }
    if (!parse_args(argc, argv, opt, error)) {
        std::cerr << error << "\n\n";
        print_usage(argv[0]);
        return 1;
    }

    in_addr host_addr{};
    if (inet_pton(AF_INET, opt.host.c_str(), &host_addr) != 1) {
        std::cerr << "Not a valid IPv4 address: " << opt.host << "\n\n";
        print_usage(argv[0]);
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);
    curl_global_init(CURL_GLOBAL_DEFAULT); // once, before any thread uses libcurl

    // The server first: if its port is taken (another subcon is running)
    // there is nothing useful this process could do.
    SUBCON_TOOL_SERVER server(opt.listen_port, 30);
    if (server.bind_failed()) {
        std::cerr << "Couldn't listen on port " << opt.listen_port << " - is another subcon already running?\n";
        return 1;
    }

    // Wake pipe: the worker writes a byte when a result is ready so select()
    // returns at once instead of at its 200ms tick.
    int wake_pipe[2];
    if (pipe(wake_pipe) != 0) { std::cerr << "pipe() failed\n"; return 1; }
    for (int fd : wake_pipe) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    SUBCON_LOG logger;
    bool use_display = isatty(STDOUT_FILENO) != 0;
    logger.to_display = use_display;

    JOB_WORKER worker(wake_pipe[1]);
    SUBCON_REASONER reasoner(server, worker, [&](const std::string& line, bool file_only) { logger.write(line, file_only); }, REASONER_CONFIG{});
    OLLI_LINK link(opt.host, host_addr, make_register_message(), opt.olli_port);

    // ---- profile handling ----
    // Pinned (--dir / --profile) or learned from olli's identity message.
    bool pinned = !opt.dir.empty() || !opt.profile.empty();
    std::string current_profile_name;   // as last seen from olli (or --profile)
    bool profile_active = false;
    fs::path active_profile_dir;

    auto say = [&](const std::string& line) { logger.write(line, false); };

    STATE_LOCK state_lock;
    std::string fatal_error; // set if the state folder is already owned by another subcon

    auto activate_profile = [&](const fs::path& profile_dir, const std::string& label) {
        SUBCON_SETTINGS settings = load_settings(profile_dir, say);
        REASONER_CONFIG config = settings.reasoner;
        // Command-line options win over the settings file.
        if (!opt.model.empty()) config.ollama.model = opt.model;
        if (!opt.ollama_host.empty()) config.ollama.host = opt.ollama_host;
        if (opt.ollama_port != 0) config.ollama.port = opt.ollama_port;
        config.think_cycle_enabled = opt.think_cycle;
        config.force_run = opt.force_run;
        config.ignore_limits = opt.ignore_limits;
        if (opt.first_cycle_delay >= 0) config.first_cycle_delay_seconds = opt.first_cycle_delay;
        server.set_call_timeout_seconds(settings.call_timeout_seconds);

        fs::path state_dir = profile_dir / "subcon";
        std::error_code ec;
        fs::create_directories(state_dir, ec);

        // Before anything is read or written there - including the log. If
        // another subcon owns this state folder this one exits (simplest
        // thing that can't corrupt anything): nothing is loaded, so no cycle
        // can run, and the main loop is skipped.
        std::string lock_error;
        if (!state_lock.acquire(state_dir, lock_error)) {
            fatal_error = "refusing to run: " + lock_error;
            say(fatal_error);
            quit_requested.store(true);
            return;
        }
        logger.open_file(state_dir / "subcon_log.txt");
        say("profile: " + label + " - state in " + state_dir.string() + ", model " + config.ollama.model + " at " +
            config.ollama.host + ":" + std::to_string(config.ollama.port) +
            (config.think_cycle_enabled ? "" : " [think cycle OFF]") + (config.force_run ? " [FORCE-RUN]" : "") +
            (config.ignore_limits ? " [IGNORING LIMITS]" : ""));

        reasoner.set_config(config);
        reasoner.load_state(state_dir);
        active_profile_dir = profile_dir;
        profile_active = true;
        reasoner.probe_presence_if_unknown();
    };

    if (!opt.dir.empty()) {
        activate_profile(opt.dir, opt.dir.string());
    } else if (!opt.profile.empty()) {
        current_profile_name = opt.profile;
        activate_profile(profile_dir_for(opt.profile), opt.profile);
    }

    // What tools are told about the user. Tools handle identity per link but
    // hold ONE profile name, so this must be the same name olli sends them -
    // never empty-by-accident or different, or a tool with a link to each
    // server would flip profiles whenever the two disagree. A pinned name
    // (--user-name, or --profile) is sent from the start; otherwise olli's own
    // identity message is forwarded verbatim when it arrives (below).
    const bool identity_pinned = !opt.user_name.empty() || !opt.profile.empty();
    if (identity_pinned)
        server.set_identity({{"type", "identity"}, {"name", opt.user_name.empty() ? opt.profile : opt.user_name}, {"full_name", ""}, {"about", ""}});

    OLLI_DISPLAY* display = use_display ? new OLLI_DISPLAY(TOOL_AREA_HEIGHT) : nullptr;
    std::string last_drawn_tool_area;
    std::string connection_status = "Not connected to olli at " + opt.host + " - retrying...";

    auto handle_olli_message = [&](const json& msg) {
        std::string type = msg.value("type", "");

        if (type == "identity") {
            // Forward as-is to every tool that registers with us (and to the
            // ones already connected) - what olli itself does for its own
            // tools, so a tool's second link settles on the same profile.
            // (--user-name / --profile pin the name instead, set at startup above.)
            if (!identity_pinned) server.set_identity(msg);
            std::string name = msg.value("name", "");
            if (!pinned && (!profile_active || name != current_profile_name)) {
                current_profile_name = name;
                activate_profile(profile_dir_for(name), name.empty() ? "(shared default)" : name);
            }
        } else if (type == "call") {
            std::string call_id = msg.value("call_id", "");
            std::string name = msg.value("name", "");
            if (name == "get_subcon_notes") {
                if (!reasoner.state_loaded()) {
                    // Only possible if olli calls before its identity arrived.
                    link.send_result(call_id, "Nothing on my mind right now.");
                } else {
                    SUBCON_PULL_RESULT pulled = reasoner.pull_notes();
                    link.send_result(call_id, pulled.text, pulled.special_instruction);
                }
            } else {
                link.send_error(call_id, "Unknown tool name: " + name);
            }
        }
    };

    say("subcon started: serving tools on 127.0.0.1:" + std::to_string(opt.listen_port) + ", registering with olli at " +
        opt.host + ":" + std::to_string(opt.olli_port));

    while (!quit_requested.load()) {
        // One bad message must not end the process: nlohmann's value() throws
        // on a wrongly-typed field (review 2026-10-29; handle_olli_message()
        // and the registered-tool handling both read untrusted JSON), and an
        // exception leaving main() is std::terminate. Log it and carry on -
        // a message the loop was in the middle of is lost, the rest stay
        // queued for the next tick.
        try {
            if (display) display->tick();

            fd_set read_fds, write_fds;
            FD_ZERO(&read_fds);
            FD_ZERO(&write_fds);
            int max_fd = wake_pipe[0];
            FD_SET(wake_pipe[0], &read_fds);
            int link_fd = link.fd();
            if (link_fd >= 0) { FD_SET(link_fd, &read_fds); max_fd = std::max(max_fd, link_fd); }
            server.add_fds(read_fds, write_fds, max_fd);

            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 200000;
            int ready = select(max_fd + 1, &read_fds, &write_fds, nullptr, &tv);
            if (ready < 0) { FD_ZERO(&read_fds); FD_ZERO(&write_fds); ready = 0; } // EINTR (a signal): run a normal tick

            if (ready > 0 && FD_ISSET(wake_pipe[0], &read_fds)) {
                char drain[64];
                while (read(wake_pipe[0], drain, sizeof(drain)) > 0) {}
            }

            if (display) {
                int key = display->get_key();
                if (key == 'q' || key == 'Q') quit_requested.store(true);
            }

            // --- olli ---
            link.service(link_fd >= 0 && ready > 0 && FD_ISSET(link_fd, &read_fds));
            json msg;
            while (link.next_message(msg)) handle_olli_message(msg);
            // OLLI_LINK repeats "not connected - retrying" on every attempt (every 3s);
            // only a change is worth a log line.
            if (!link.status().empty() && link.status() != connection_status) { connection_status = link.status(); logger.write(connection_status, false); }
            else if (!link.status().empty()) connection_status = link.status();
            link.consume_disconnected(); // nothing to reset: olli being away changes nothing about what we hold

            // --- tools ---
            server.service(read_fds, write_fds);
            for (const SERVER_NOTICE& notice : server.take_notices()) {
                if (notice.kind == SERVER_NOTICE::REGISTERED) {
                    say(notice.peer + " connected");
                    if (std::find(notice.tool_names.begin(), notice.tool_names.end(), "check_presence") != notice.tool_names.end())
                        reasoner.probe_presence_if_unknown();
                } else if (notice.kind == SERVER_NOTICE::DISCONNECTED) {
                    say(notice.peer + " disconnected");
                } else if (notice.kind == SERVER_NOTICE::PROBLEM) {
                    say(notice.peer + " " + notice.message);
                } else if (notice.var_name == "user.presence") {
                    reasoner.on_presence(notice.var_value, notice.peer);
                } else {
                    // Other tools' events (a timer firing, ...) are olli's to
                    // narrate; subcon only notes that they happened.
                    logger.write("event from " + notice.peer + ": " + notice.message, true);
                }
            }

            // --- the work ---
            JOB_RESULT job_result;
            while (worker.take_result(job_result)) reasoner.on_job_result(job_result);
            reasoner.tick();

            // --- display ---
            if (display) {
                std::vector<std::string> lines;
                lines.push_back("SUBCON - " + reasoner.stage_description());
                lines.push_back("Presence: " + reasoner.presence() + "    Notes waiting: " + std::to_string(reasoner.pending_note_count()));
                lines.push_back("Last finding: " + (reasoner.last_finding().empty() ? std::string("(none yet)") : reasoner.last_finding()));
                std::vector<std::string> tools = server.connection_summaries();
                lines.push_back("Tools connected: " + std::to_string(tools.size()) + "  (port " + std::to_string(opt.listen_port) + ")");
                for (size_t i = 0; i < tools.size() && lines.size() < static_cast<size_t>(TOOL_AREA_HEIGHT); ++i)
                    lines.push_back("  " + tools[i]);
                draw_tool_area(*display, lines, last_drawn_tool_area);

                display->set_connection(connection_status);
                display->set_profile(profile_active ? "Profile: " + (current_profile_name.empty() && pinned ? active_profile_dir.string()
                                                                    : current_profile_name.empty() ? std::string("(shared default)") : current_profile_name)
                                                     : std::string("Profile: (waiting for olli's identity)"));
                display->set_tool_status(logger.last_line);
                display->present();
            }
        } catch (const std::exception& e) {
            logger.write(std::string("error in the main loop (continuing): ") + e.what(), false);
            usleep(200000); // if the same thing throws every tick, log at the loop's own pace, not flat out
        }
    }

    // ---- shutdown ----
    // Nothing is mid-write that matters: every state file is written
    // atomically at the moment it changes, so there is nothing to flush.
    // Stopping the worker aborts any model call in flight and joins its
    // thread (job_worker.hpp) - done before anything it could touch is
    // destroyed.
    logger.write("shutting down", false);
    worker.stop();
    delete display;
    curl_global_cleanup();
    close(wake_pipe[0]);
    close(wake_pipe[1]);
    if (!fatal_error.empty()) { std::cerr << fatal_error << "\n"; return 1; }
    return 0;
}
