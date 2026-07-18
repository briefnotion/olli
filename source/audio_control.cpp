#ifndef AUDIO_CONTTROL_CPP
#define AUDIO_CONTTROL_CPP

#include "audio_control.h"

void AUDIO_CONTROL_CLASS::VOCA_set(int Command)
{
    switch (Command)
    {
        case DEF_VOCA_SLEEP:
            VOCA_COMMAND_SETTINGS.command = "sleep";
            break;
        case DEF_VOCA_PAUSE:
            VOCA_COMMAND_SETTINGS.command = "pause";
            break;
        case DEF_VOCA_LISTEN:
            VOCA_COMMAND_SETTINGS.command = "listen";
            break;
        default:
            std::cerr << "Invalid VOCA command: " << Command << std::endl;
            return;
    }
    writeFile(settings_path / "voca_command.json", VOCA_COMMAND_SETTINGS);
}

void AUDIO_CONTROL_CLASS::adjust_audio_files(double Time)
{
    /*
    VOCA Status's

        SLEEP
        VOCA Sleeping   VOCA_SETTINGS.is_awake = false
        LISTEN
        Voca Listening  VOCA_SETTINGS.is_awake = true
            
        PAUSE

    LIRA Status's

        INTERRUPT
        LIRA Interrupt   LIRA_SETTINGS.interrupt = true     
        SPEAKING
        LIRA Speaking    LIRA_SETTINGS.is_speaking = true   
    */

    VOCA_STATUS_CHANGED = checkAndLoadFile(settings_path / "voca_status.json", VOCA_lastKnownTime, VOCA_SETTINGS);
    LIRA_STATUS_CHANGED = checkAndLoadFile(settings_path / "lira_control.json", LIRA_lastKnownTime, LIRA_SETTINGS);

    // Wake command recieved from VOCA status.
    if (CONTROL_AWAKE == false)
    {
        if (VOCA_SETTINGS.is_awake)
        {
            //cout << "CONTROL_AWAKE = true          " << Time <<endl;
            CONTROL_AWAKE = true;
            VOCA_STATUS_CHANGED = false;

            AUDIO_TIMER.set(Time, 10000);
        }
    }

    // Sleep command recieved from VOCA status.
    if (CONTROL_AWAKE == true)
    {
        if (VOCA_STATUS_CHANGED && VOCA_SETTINGS.is_awake == false)
        {
            //cout << "CONTROL_SLEEPING = true" << endl;
            CONTROL_AWAKE = false;
            VOCA_STATUS_CHANGED = false;
        }
    }

    //  If LIRA starts speaking, then we pause VOCA. If LIRA stops speaking, then we set VOCA to listen.
    if (CONTROL_AWAKE)
    {
        if (LIRA_STATUS_CHANGED && LIRA_SETTINGS.is_speaking)
        {
            //cout << "VOCA_set(Time, DEF_VOCA_PAUSE)" << endl;
            VOCA_set(DEF_VOCA_PAUSE);
            LIRA_STATUS_CHANGED = false;
        }

        if (LIRA_STATUS_CHANGED && LIRA_SETTINGS.is_speaking == false)
        {
            //cout << "VOCA_set(Time, DEF_VOCA_LISTEN)" << endl;
            VOCA_set(DEF_VOCA_LISTEN);
            LIRA_STATUS_CHANGED = false;

            AUDIO_TIMER.set(Time, 10000);
        }
    }

    // Timer Control.
    if (AUDIO_TIMER.is_ready(Time))
    {
        if (CONTROL_AWAKE)
        {
            if (LIRA_SETTINGS.is_speaking == false)
            {
                //cout << "Timer Control: Setting VOCA to Sleep" << endl;
                VOCA_set(DEF_VOCA_SLEEP);
            }
        }
    }

    // Manual override control from user input.
    if (VOCA_REQUESTED_CHANGE > -1)
    {
        VOCA_set(VOCA_REQUESTED_CHANGE);
        VOCA_REQUESTED_CHANGE = -1;
    }
}

AUDIO_CONTROL_CLASS::AUDIO_CONTROL_CLASS()
{
}

void AUDIO_CONTROL_CLASS::create(const std::filesystem::path& filePath)
{
    settings_path =  filePath;
    if (std::filesystem::exists(settings_path / "lira_control.json")) {
        LIRA_lastKnownTime = std::filesystem::last_write_time(settings_path / "lira_control.json");
    }
    if (std::filesystem::exists(settings_path / "voca_status.json")) {
        VOCA_lastKnownTime = std::filesystem::last_write_time(settings_path / "voca_status.json");
    }
}

void AUDIO_CONTROL_CLASS::VOCA_manual_set(int Command)
{
    VOCA_REQUESTED_CHANGE = Command;
}

void AUDIO_CONTROL_CLASS::thread_main()
{
    TIMED_IS_READY  frame_limit;     // Controls sleep time
    FLED_TIME thread_time;           // Thread gets its own Time 
    thread_time.create();

    RUN = true;
    while (RUN)
    {
        // prepare thread
        thread_time.setframetime();
        frame_limit.set(thread_time.current_frame_time(), INTERVAL);

        adjust_audio_files(thread_time.current_frame_time());

        //sleep thread
        thread_time.request_ready_time(frame_limit.get_ready_time());
        thread_time.sleep_till_next_frame();
    }
    std::cout << "Audio Control Thread Ended" << std::endl;
}

void AUDIO_CONTROL_CLASS::thread_start()
{
    {
        THREAD_CONTROL.create(1000);
        // Start the camera update on a separate thread.
        // This call is non-blocking, so the main loop can continue immediately.
        THREAD_CONTROL.start_render_thread([&]() 
                  {  thread_main();  });
    }
}

void AUDIO_CONTROL_CLASS::thread_stop()
{
    while (RUN)
    {
        RUN = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}


#endif