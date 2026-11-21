#ifndef PROTECTED_SLOTS_H
#define PROTECTED_SLOTS_H

// Where a "protected slot" (ai_root::set_protected_slot()) lives in history. A slot's negative consolidation level is
// its ID - -1 the persona, -2 "about the user", -3 subcon's quick note - NOT a fixed position. The slots sit together
// at the very top of history in order -1, -2, -3 (closest to 0 first), but any of them may be missing: with no -2 yet,
// a -3 simply follows the -1, and a -2 added later goes in front of it. Pure functions on the list of levels (one int
// per history entry) so the rules can be tested on their own - see source/tests/protected_slots_test.cpp.

#include <cstddef>
#include <vector>

namespace protected_slots
{
    constexpr size_t NOT_FOUND = static_cast<size_t>(-1);

    // The slot with this level, looked for only in the protected prefix (the unbroken run of negative levels at the
    // top) - a stray negative-level message further down the conversation is not a slot.
    inline size_t find(const std::vector<int>& levels, int level)
    {
        for (size_t i = 0; i < levels.size() && levels[i] < 0; ++i) {
            if (levels[i] == level) return i;
        }
        return NOT_FOUND;
    }

    // Where a new slot with this level goes: after every protected message whose level is closer to zero.
    inline size_t insert_position(const std::vector<int>& levels, int level)
    {
        size_t position = 0;
        while (position < levels.size() && levels[position] < 0 && levels[position] > level) ++position;
        return position;
    }
}

#endif // PROTECTED_SLOTS_H
