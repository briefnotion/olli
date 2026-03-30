#ifndef system_h
#define system_h

#include <string>

#include "helper_olli.h"
#include "audio_control.h"

class CLASS_SYSTEM
{
  private:

  public:

    Settings setings_vars;
    KEYBOARD_INPUT key_input;
    AUDIO_CONTROL_CLASS audio_control;
};

#endif