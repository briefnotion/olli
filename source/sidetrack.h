#ifndef SIDETRACK_H
#define SIDETRACK_H

#include <iostream>
#include <fstream>
#include <filesystem>
#include <thread>
#include <chrono>

//#include <nlohmann/json.hpp>
#include "olla.h"
#include "fled_time.h"
#include "threading.h"


class SIDETRACK_CLASS
{
    private:
        double INTERVAL = 500;  //ms
        THREADING_INFO  THREAD_CONTROL;  // Controls: update_frame_thread()
        std::filesystem::path settings_path;

        TIMED_IS_READY  RESUME_TIMER;

        // 
        ollama_system SIDETRACK_CHAT_INSTANCE;

        // Communication variables

    public:
        bool RUN = false;

        SIDETRACK_CLASS();

        void create(const std::filesystem::path& filePath);

        void thread_main();

        void thread_start();
        void thread_stop();
};

#endif