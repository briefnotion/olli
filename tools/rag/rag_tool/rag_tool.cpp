// rag_tool - the olli-facing half of olli's RAG system. Registers as a
// remote tool (see ../../PROTOCOL.md) so olli can search the RAG knowledge
// base built by ../rag_admin/ - this program never writes to the database,
// only reads. Built from ../../template/template_tool.cpp; see that file
// and ../../PROTOCOL.md for what the connection/registration/heartbeat
// plumbing (olli_link.hpp/.cpp, shared, untouched here) already handles.
//
// Build: `make` in this directory (needs libsqlite3-dev and libcurl, same
// as ../rag_db/ and ../rag_admin/). Run: `./rag_tool [host] [db_path]` -
// host defaults to 127.0.0.1 (olli running on this same machine, matching
// every other tool's convention); db_path defaults to a fixed path under
// this user's own test profile (~/olli_files_claude/rag.db - see the repo
// memory note on why that profile, not the shared ~/olli_files/, is used
// for testing). Pass an explicit db_path to point at a different database.
// Needs Ollama running locally with the embedding model pulled (see
// ../rag_db/rag_embed.hpp for the default) - query text gets embedded here,
// same as at import time in rag_admin, so search compares like with like.

#include <nlohmann/json.hpp>

#include "olli_link.hpp"
#include "rag_db.hpp"
#include "rag_embed.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using json = nlohmann::json;

namespace {
    // Registers two calls: one so the model can discover what's actually
    // in the database before guessing a collection name, one to search it.
    json make_register_message()
    {
        return {
            {"type", "register"},
            {"tools", json::array({
                {
                    {"name", "rag_list_collections"},
                    {"description", "List the collections (categories of stored knowledge - e.g. "
                                     "notes, conversations, manuals) currently available in the RAG "
                                     "knowledge base, with a short description of each."},
                    {"parameters", {{"type", "object"}, {"properties", json::object()}}}
                },
                {
                    {"name", "rag_search"},
                    {"description", "Search the RAG knowledge base for information relevant to a "
                                     "query - notes, past conversations, documentation, or anything "
                                     "else imported into it. Use rag_list_collections first if you "
                                     "want to target a specific collection by name."},
                    {"parameters", {
                        {"type", "object"},
                        {"properties", {
                            {"query", {{"type", "string"}, {"description", "What to search for."}}},
                            {"collection", {{"type", "string"}, {"description",
                                "Optional - restrict the search to this one collection's name. "
                                "Omit to search everything."}}}
                        }},
                        {"required", json::array({"query"})}
                    }}
                }
            })}
        };
    }

    std::string handle_list_collections(RAG_DB& db)
    {
        std::vector<RAG_COLLECTION> collections = db.list_collections();
        if (collections.empty()) return "No collections exist in the RAG database yet.";

        std::ostringstream out;
        for (const auto& c : collections) {
            out << "- " << c.name;
            if (!c.description.empty()) out << ": " << c.description;
            out << "\n";
        }
        return out.str();
    }

    std::string handle_search(RAG_DB& db, RAG_EMBEDDER& embedder, const json& arguments)
    {
        std::string query = arguments.value("query", "");
        if (query.empty()) return "ERROR: query argument is required.";

        std::optional<int> collection_id;
        if (arguments.contains("collection") && !arguments["collection"].get<std::string>().empty()) {
            std::string collection_name = arguments["collection"].get<std::string>();
            std::optional<RAG_COLLECTION> found = db.find_collection(collection_name);
            if (!found) {
                return "ERROR: no collection named \"" + collection_name +
                       "\" - call rag_list_collections to see what's available.";
            }
            collection_id = found->id;
        }

        std::vector<float> query_embedding = embedder.embed_query(query);
        if (query_embedding.empty()) return "ERROR: embedding the query failed: " + embedder.last_error();

        // Fixed top-5, matching rag_admin's own default - see the design
        // discussion this came out of for why a model-settable top_k isn't
        // exposed (one less parameter to get wrong, easy to add later).
        std::vector<RAG_SEARCH_RESULT> results = db.search(query_embedding, 5, collection_id);
        if (results.empty()) return "No results found.";

        std::ostringstream out;
        int n = 1;
        for (const auto& r : results) {
            std::string snippet = r.chunk_text.substr(0, 300);
            out << n++ << ". [" << r.collection_name << " / " << r.document_title << "] "
                << snippet << (r.chunk_text.size() > 300 ? "..." : "") << "\n\n";
        }
        return out.str();
    }

    std::string handle_call(OLLI_LINK& link, RAG_DB& db, RAG_EMBEDDER& embedder, const json& msg)
    {
        std::string call_id = msg.value("call_id", "");
        std::string name = msg.value("name", "");
        json arguments = msg.contains("arguments") ? msg["arguments"] : json::object();

        if (name == "rag_list_collections") {
            link.send_result(call_id, handle_list_collections(db));
            return "Call answered: " + name;
        }
        if (name == "rag_search") {
            link.send_result(call_id, handle_search(db, embedder, arguments));
            return "Call answered: " + name;
        }

        link.send_error(call_id, "Unknown tool name: " + name);
        return "Unknown call received: " + name;
    }

    void olli_processing(OLLI_LINK& link, RAG_DB& db, RAG_EMBEDDER& embedder, bool socket_readable, std::string& status)
    {
        auto dispatch = [&](const json& msg) {
            std::string type = msg.value("type", "");
            if (type == "call") status = handle_call(link, db, embedder, msg);
        };

        link.service(socket_readable);

        json msg;
        while (link.next_message(msg)) dispatch(msg);

        if (!link.status().empty()) status = link.status();
    }

    // RAII: puts stdin into raw, non-canonical, non-echoing mode so 'q' can
    // be read immediately - same pattern as ../../template/template_tool.cpp.
    class RawTerminal {
        public:
            RawTerminal()
            {
                if (tcgetattr(STDIN_FILENO, &old_termios) == 0) {
                    termios raw = old_termios;
                    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG));
                    raw.c_cc[VMIN] = 0;
                    raw.c_cc[VTIME] = 0;
                    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
                    active = true;
                }
                std::cout << "\033[?25l" << std::flush;
            }

            ~RawTerminal()
            {
                std::cout << "\033[?25h" << std::flush;
                if (active) tcsetattr(STDIN_FILENO, TCSANOW, &old_termios);
            }

            RawTerminal(const RawTerminal&) = delete;
            RawTerminal& operator=(const RawTerminal&) = delete;

        private:
            termios old_termios{};
            bool active = false;
    };

    void redraw_screen(const std::string& status, const std::string& db_path)
    {
        std::cout << "\033[H\033[2K" << "rag_tool - db: " << db_path << " - " << status << std::flush;
    }

    std::string default_db_path()
    {
        const char* home = std::getenv("HOME");
        std::string home_dir = home ? home : ".";
        return home_dir + "/olli_files_claude/rag.db";
    }

    void print_usage(const char* argv0)
    {
        std::string prog = argv0;
        size_t slash = prog.find_last_of('/');
        if (slash != std::string::npos) prog = prog.substr(slash + 1);

        std::cout << "Usage: " << prog << " [host] [db_path] [-h|--help]\n\n"
                      "  host          IP address of the machine running olli. Defaults to\n"
                      "                127.0.0.1 (olli running on this same machine).\n\n"
                      "  db_path       Path to the RAG SQLite database (read-only from here -\n"
                      "                see ../rag_admin/ for writing to it). Defaults to\n"
                      "                " << default_db_path() << "\n\n"
                      "  -h, --help    Show this help and exit.\n";
    }
}

int main(int argc, char* argv[])
{
    std::string host = "127.0.0.1";
    std::string db_path = default_db_path();

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") { print_usage(argv[0]); return 0; }
        if (i == 1) host = arg;
        else if (i == 2) db_path = arg;
    }

    in_addr host_addr{};
    if (inet_pton(AF_INET, host.c_str(), &host_addr) != 1) {
        std::cerr << "Not a valid IPv4 address: " << host << "\n\n";
        print_usage(argv[0]);
        return 1;
    }

    RAG_DB db(db_path);
    if (!db.is_open()) {
        std::cerr << "Failed to open RAG database at \"" << db_path << "\": " << db.last_error() << "\n";
        return 1;
    }

    RAG_EMBEDDER embedder;
    OLLI_LINK link(host, host_addr, make_register_message());

    RawTerminal raw_terminal;
    std::cout << "\033[2J";

    bool has_real_terminal = isatty(STDIN_FILENO) != 0;
    std::string status = "Not connected to olli at " + host + " - retrying...";

    bool quit = false;
    while (!quit) {
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        fd_set read_fds;
        FD_ZERO(&read_fds);
        int max_fd = -1;
        if (has_real_terminal) {
            FD_SET(STDIN_FILENO, &read_fds);
            max_fd = STDIN_FILENO;
        }
        if (link.fd() >= 0) {
            FD_SET(link.fd(), &read_fds);
            max_fd = std::max(link.fd(), max_fd);
        }

        int ready = select(max_fd + 1, &read_fds, nullptr, nullptr, &tv);

        if (ready > 0 && FD_ISSET(STDIN_FILENO, &read_fds)) {
            char c = 0;
            if (read(STDIN_FILENO, &c, 1) > 0) {
                if (c == 'q' || c == 'Q' || c == 3) quit = true;
            }
        }

        bool socket_readable = link.fd() >= 0 && ready > 0 && FD_ISSET(link.fd(), &read_fds);

        if (!quit) olli_processing(link, db, embedder, socket_readable, status);
        if (!quit) redraw_screen(status, db_path);
    }

    return 0;
}
