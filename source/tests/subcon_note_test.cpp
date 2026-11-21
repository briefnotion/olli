// Stand-alone test of source/subcon_note.h (not part of the olli build). From the source/ folder:
//   g++ -std=c++17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror -I../../json/include -o tests/subcon_note_test tests/subcon_note_test.cpp && ./tests/subcon_note_test
#include "../subcon_note.h"

#include <cstdio>

static int fails = 0;
static void check(bool ok, const std::string& what) { std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) ++fails; }
static bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

int main()
{
    using namespace subcon_note;

    std::printf("== reading subcon's answer\n");
    PEEK p = parse_peek(R"({"pending":3,"notes":[{"label":"Timers","urgency":"urgent","age_minutes":10,"summary":"x"},{"label":"Weather","urgency":"normal"},{"label":"Weather","urgency":"low"}]})");
    check(p.pending == 3 && p.any_urgent && p.topics.size() == 2 && p.topics[0] == "Timers" && p.topics[1] == "Weather", "count, urgent flag, each topic once in order");
    PEEK none = parse_peek(R"({"pending":0,"notes":[]})");
    check(none.pending == 0 && none.topics.empty() && !none.any_urgent, "nothing waiting -> pending 0");
    check(parse_peek("Error: no response within 300 seconds.").pending == -1, "an error text is unreadable (-1)");
    check(parse_peek("").pending == -1 && parse_peek("[]").pending == -1 && parse_peek("{}").pending == -1, "empty / array / no pending -> -1");
    check(parse_peek(R"({"pending":"3"})").pending == -1 && parse_peek(R"({"pending":-1})").pending == -1 && parse_peek(R"({"pending":99999999999})").pending == -1, "wrong type, negative, absurd -> -1");
    PEEK odd = parse_peek(R"({"pending":2,"notes":[1,"x",{"label":5},{"label":"","urgency":3},{"label":"Notes"}]})");
    check(odd.pending == 2 && odd.topics.size() == 1 && odd.topics[0] == "Notes" && !odd.any_urgent, "odd notes entries are skipped, not fatal");
    PEEK longlabel = parse_peek(std::string(R"({"pending":1,"notes":[{"label":")") + std::string(500, 'a') + R"("}]})");
    check(longlabel.topics.size() == 1 && longlabel.topics[0].size() == 60, "a long label is cut to 60 bytes");
    std::string deep(100000, '['); deep += std::string(100000, ']');
    check(parse_peek(deep).pending == -1, "deeply nested garbage does not crash");

    std::printf("== finding the tool among tool_worker's definitions (the real shape)\n");
    nlohmann::json defs = nlohmann::json::parse(R"([
        {"type":"function","function":{"name":"get_subcon_notes","description":"x","parameters":{}}},
        {"type":"function","function":{"name":"peek_subcon_notes","description":"y","parameters":{}}}])");
    check(tool_offered(defs, "peek_subcon_notes") && tool_offered(defs, "get_subcon_notes"), "found in {type: function, function: {name}}");
    check(!tool_offered(defs, "count_subcon_notes"), "a tool that is not there is not found");
    check(!tool_offered(nlohmann::json::parse(R"([{"name":"peek_subcon_notes"}])"), "peek_subcon_notes"), "the bare {name} shape is NOT what tool_worker returns - not matched (this was the bug)");
    check(!tool_offered(nlohmann::json::array(), "x") && !tool_offered(nlohmann::json::object(), "x") && !tool_offered(nlohmann::json(), "x"), "empty / wrong types");
    check(!tool_offered(nlohmann::json::parse(R"([1,"a",null,{"function":5},{"function":{"name":7}}])"), "x"), "odd entries are skipped, not fatal");

    std::printf("== the nudge\n");
    std::string one = build_nudge(parse_peek(R"({"pending":1,"notes":[{"label":"Weather","urgency":"normal"}]})"));
    check(has(one, "has 1 thing waiting") && has(one, "(topics: Weather)") && !has(one, "urgent") && has(one, "get_subcon_notes"), "one normal note");
    std::string many = build_nudge(p);
    check(has(many, "has 3 things waiting") && has(many, "(topics: Timers, Weather)") && has(many, "at least one is urgent") && has(many, "Tell the user soon."), "urgent: said, and 'tell soon'");
    PEEK no_topics; no_topics.pending = 2;
    check(!has(build_nudge(no_topics), "topics:") && has(build_nudge(no_topics), "2 things"), "no topics (old notes without labels) still reads fine");
    check(!has(one, "summary") && !has(many, "age_minutes"), "carries no note text, only count and topics");

    std::printf("\n%s\n", fails == 0 ? "ALL OK" : "SOME FAILED");
    return fails == 0 ? 0 : 1;
}
