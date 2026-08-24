#ifndef system_h
#define system_h

#include <string>

#include "helper_olli.h"
#include "audio_control.h"
#include "user_io.h"
#include "remote_tools.h"

class CLASS_SYSTEM
{
    private:

    public:
        Settings setings_vars;
        KEYBOARD_INPUT key_input;
        OUTPUT_CLASS output;

        AUDIO_CONTROL_CLASS audio_control;

        // Who olli is talking to this session - see USER_IDENTITY's own
        // comment in helper_olli.h. Populated once in main.cpp, right where
        // setings_vars.profile_name is resolved.
        USER_IDENTITY user;

        // Accepts remote-tool connections (see tools/PROTOCOL.md and
        // remote_tools.h) - polled once per main-loop tick in main.cpp. A
        // passive resource like audio_control/key_input above, not business
        // logic (unlike ollama_system/SIDETRACK_CLASS, which stay separate
        // and take CLASS_SYSTEM& as a parameter instead of living on it) -
        // moved here so anything that ever needs to see every connected
        // remote tool, not just the one that just finished registering,
        // has somewhere to reach it from.
        REMOTE_TOOL_LISTENER remote_tools;
};

#endif