#ifndef AUDIO_CONTTROL_H
#define AUDIO_CONTTROL_H

#include <iostream>
#include <filesystem>
#include <thread>
#include <chrono>

#include "fled_time.h"
#include "threading.h"

class AUDIO_CONTROL_CLASS
{
    private:
        double INTERVAL = 500;  //ms
        THREADING_INFO  THREAD_CONTROL;  // Controls: update_frame_thread()

        void adjust_audio_files();

    public:
        bool RUN = false;

        std::filesystem::path settings_path;

        void thread_main();

        void thread_start();
        void thread_stop();
};

#endif