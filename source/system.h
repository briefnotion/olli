#ifndef system_h
#define system_h

#include <string>

#include "helper_olli.h"

class CLASS_SYSTEM
{
    private:

    public:
        Settings setings_vars;

        // Keyboard input, screen display, and audio (text-to-speech/
        // speech-to-text) used to live here (key_input/output/
        // audio_control) - all three now live on IO_WORKER_CLASS
        // (io_worker.h/.cpp) instead, which owns them exclusively (they
        // have zero built-in thread-safety of their own, or - for audio -
        // are meant to be reached only through IO_WORKER_CLASS - see
        // IO_WORKER_CLASS's class comment). main.cpp constructs an
        // IO_WORKER_CLASS alongside this.

        // Who olli is talking to this session used to live here (USER_
        // IDENTITY user) - moved to COMMS (comms.h, comms.user) 2026-09-26
        // once it grew a genuinely live-changing field (presence) that
        // needs the same cross-thread visibility COMMS::busy_count()
        // already has, not a write-once settings-adjacent value like the
        // rest of this class. See USER_IDENTITY's own comment in
        // helper_olli.h.

        // Remote-tool connections used to be accepted here
        // (REMOTE_TOOL_LISTENER remote_tools, tools/PROTOCOL.md/
        // remote_tools.h) - that listening socket now lives inside
        // TOOL_WORKER_CLASS's own thread_main() instead (tool_worker.h/
        // .cpp), which owns and services it continuously on its own
        // background thread rather than once per main-loop tick.
};

#endif