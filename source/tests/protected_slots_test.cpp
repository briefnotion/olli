// Stand-alone test of source/protected_slots.h (not part of the olli build). From the source/ folder:
//   g++ -std=c++17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror -o tests/protected_slots_test tests/protected_slots_test.cpp && ./tests/protected_slots_test
#include "../protected_slots.h"

#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
static void check(bool ok, const std::string& what) { std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) ++fails; }

// The same two steps ai_root::set_protected_slot() does, on a list of levels (0 = an ordinary message).
static void set_slot(std::vector<int>& levels, int level)
{
    const size_t found = protected_slots::find(levels, level);
    if (found != protected_slots::NOT_FOUND) { levels[found] = level; return; }
    levels.insert(levels.begin() + static_cast<std::ptrdiff_t>(protected_slots::insert_position(levels, level)), level);
}
static bool clear_slot(std::vector<int>& levels, int level)
{
    const size_t found = protected_slots::find(levels, level);
    if (found == protected_slots::NOT_FOUND) return false;
    levels.erase(levels.begin() + static_cast<std::ptrdiff_t>(found));
    return true;
}

int main()
{
    using V = std::vector<int>;

    std::printf("== -2 behaves as before\n");
    V h = {-1, 0, 0, 0};
    set_slot(h, -2);
    check(h == V({-1, -2, 0, 0, 0}), "-2 goes right after the persona");
    set_slot(h, -2);
    check(h == V({-1, -2, 0, 0, 0}), "setting it again replaces, never duplicates");

    std::printf("== -3 with and without -2\n");
    V no2 = {-1, 0, 0};
    set_slot(no2, -3);
    check(no2 == V({-1, -3, 0, 0}), "no -2: the -3 follows the -1 (not dropped into the conversation)");
    set_slot(no2, -3);
    check(no2 == V({-1, -3, 0, 0}), "updating it does not duplicate it (the old position-based code would have)");
    set_slot(no2, -2);
    check(no2 == V({-1, -2, -3, 0, 0}), "a -2 added later goes in front of the -3");
    V with2 = {-1, -2, 0, 0};
    set_slot(with2, -3);
    check(with2 == V({-1, -2, -3, 0, 0}), "with -2: the -3 follows it");

    std::printf("== removal by id\n");
    check(clear_slot(with2, -3) && with2 == V({-1, -2, 0, 0}), "-3 removed, -1 and -2 untouched");
    check(!clear_slot(with2, -3), "removing it again says it was not there");
    V mid = {-1, -3, 0};
    check(clear_slot(mid, -3) && mid == V({-1, 0}), "-3 removed without a -2");
    V only3 = {-3, 0};
    check(clear_slot(only3, -3) && only3 == V({0}), "-3 removed when it is the only slot");

    std::printf("== odd histories\n");
    V empty;
    set_slot(empty, -3);
    check(empty == V({-3}), "empty history");
    V nopersona = {0, 0};
    set_slot(nopersona, -2);
    check(nopersona == V({-2, 0, 0}), "no persona: slot goes to the very top (as the old code did)");
    V stray = {-1, 0, -1, 0};   // a negative-level message further down is not a slot
    set_slot(stray, -3);
    check(stray == V({-1, -3, 0, -1, 0}), "a stray -1 deeper in the conversation does not move the slot");
    check(protected_slots::find(stray, -1) == 0, "find looks only at the protected prefix");
    V deep = {-1, 0, -3, 0};   // a -3 below the prefix is not the slot either
    check(protected_slots::find(deep, -3) == protected_slots::NOT_FOUND, "a -3 outside the prefix is not found");
    set_slot(deep, -3);
    check(deep == V({-1, -3, 0, -3, 0}), "so a new slot is inserted, the stray one is left alone");

    std::printf("\n%s\n", fails == 0 ? "ALL OK" : "SOME FAILED");
    return fails == 0 ? 0 : 1;
}
