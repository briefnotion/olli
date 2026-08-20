#ifndef system_h
#define system_h

#include <string>

#include "helper_olli.h"
#include "audio_control.h"
#include "user_io.h"

class CLASS_SYSTEM
{
    private:

    public:
        Settings setings_vars;
        KEYBOARD_INPUT key_input;
        OUTPUT_CLASS output;

        AUDIO_CONTROL_CLASS audio_control;
};

#endif