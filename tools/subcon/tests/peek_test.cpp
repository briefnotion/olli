// Test of the quick-look builder in subcon_tasks.h (peek_subcon_notes). From tools/subcon/:
//   g++ -std=c++17 -Wall -Wextra -I../../../json/include -o tests/peek_test tests/peek_test.cpp && ./tests/peek_test
#include "../subcon_tasks.h"

#include <cstdio>

static int fails = 0;
static void check(bool ok, const std::string& what) { std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) ++fails; }

int main()
{
    const int64_t now = 1000000;
    std::vector<SUBCON_NOTE> notes;
    auto add = [&](const std::string& label, const std::string& content, const std::string& urgency, int64_t age_seconds, bool delivered) {
        SUBCON_NOTE n; n.label = label; n.content = content; n.urgency = urgency; n.created_unix = now - age_seconds; n.delivered = delivered; notes.push_back(n);
    };
    add("Weather", "Storms are coming tonight.", "normal", 3 * 3600, false);
    add("Timers", "A timer finished and nobody noticed.", "urgent", 600, false);
    add("Notes", "Old thing already told.", "normal", 7200, true);
    add("Conversations", std::string(300, 'x'), "low", 60, false);

    std::printf("== peek\n");
    json peek = json::parse(subcon_build_peek(notes, now));
    check(peek["pending"] == 3, "counts only the pending notes (not the delivered one)");
    check(peek["notes"][0]["label"] == "Timers" && peek["notes"][1]["label"] == "Weather" && peek["notes"][2]["label"] == "Conversations", "urgent first, then oldest first - the order a pull would use");
    check(peek["notes"][0]["age_minutes"] == 10 && peek["notes"][1]["age_minutes"] == 180, "ages in minutes");
    check(peek["notes"][2]["summary"].get<std::string>().size() == 143, "a long note is cut to 140 bytes + '...'");
    check(subcon_count_pending(notes) == 3, "nothing was marked delivered by the peek");

    std::printf("== utf-8 safe cut\n");
    std::string accents; for (int i = 0; i < 100; ++i) accents += "\xC3\xA9";   // 100 x 'é', 2 bytes each
    std::string cut = subcon_short_text(accents, 141);   // 141 is in the middle of a character
    check(cut.size() == 140 + 3 && cut.substr(140) == "...", "stops before a half character: " + std::to_string(cut.size()));
    check(subcon_short_text("short", 140) == "short", "short text untouched");

    std::printf("== invalid UTF-8 in a note does not throw\n");
    std::vector<SUBCON_NOTE> bad; SUBCON_NOTE b; b.label = "X"; b.content = "caf\xE9 and more"; b.created_unix = now; bad.push_back(b);
    bool threw = false; std::string out;
    try { out = subcon_build_peek(bad, now); } catch (...) { threw = true; }
    check(!threw && json::parse(out)["pending"] == 1, "dump() with replace handles it");

    std::printf("== empty queue\n");
    check(json::parse(subcon_build_peek({}, now))["pending"] == 0, "pending 0");

    std::printf("\n%s\n", fails == 0 ? "ALL OK" : "SOME FAILED");
    return fails == 0 ? 0 : 1;
}
