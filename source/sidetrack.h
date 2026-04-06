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

bool consolidate(std::vector<Message>& chat_history, ollama_system& config);

class SIDETRACK_CLASS
{
    private:
        double INTERVAL = 500;  //ms
        THREADING_INFO  THREAD_CONTROL;  // Controls: update_frame_thread()
        std::filesystem::path settings_path;

        TIMED_IS_READY  RESUME_TIMER;

        // 
        ollama_system SIDETRACK_CHAT_INSTANCE;

        double IDLE_WAIT_TIME_FOR_CONSOLIDATION = 10.0 * 60.0 * 1000.0; // ms
        //double IDLE_WAIT_TIME_FOR_CONSOLIDATION = 10.0 * 1000.0; // ms
        TIMED_IS_READY IDLE_WAIT_TIMER_FOR_CONSOLIDATION;
        
        //double IDLE_WAIT_TIME = 1000.0; // ms
        //TIMED_IS_READY IDLE_WAIT_TIMER;

        // Temporary Holdings
        std::vector<Message> temp_chat_history;
        bool chat_history_requested = false;
        bool chat_history_needs_processing = false;
        bool chat_history_is_processed = false;

        // Communication variables
        std::atomic<bool> PROCESSING{false};
        std::atomic<bool> INTERUPT{false};


    public:
        bool RUN = false;

        SIDETRACK_CLASS();

        void create(OLLAMA_SYSTEM_PROPERTIES Ollama_Properties);

        void thread_main();

        void thread_start();
        void thread_stop();

        void check(bool Interupt_Signal, ollama_system& main_instance);
};

#endif