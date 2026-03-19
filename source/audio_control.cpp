#ifndef AUDIO_CONTTROL_CPP
#define AUDIO_CONTTROL_CPP

#include "audio_control.h"


void AUDIO_CONTROL_CLASS::adjust_audio_files()
{
    //std::cout << "." << std::flush;
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

        adjust_audio_files();

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