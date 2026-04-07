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

struct SIDETRACK_SIGNALS
{
    bool INTERUPT_SIGNAL = false;
    bool CHAT_FINISHED_SIGNAL = false;
};

class SIDETRACK_CLASS
{
    private:
        double INTERVAL = 500;  //ms
        THREADING_INFO  THREAD_CONTROL;  // Controls: update_frame_thread()
        std::filesystem::path settings_path;

        TIMED_IS_READY  RESUME_TIMER;

        // 
        ollama_system SIDETRACK_CHAT_INSTANCE;

        double IDLE_WAIT_TIME_FOR_CONSOLIDATION = 1.0 * 60.0 * 1000.0; // ms
        //double IDLE_WAIT_TIME_FOR_CONSOLIDATION = 10.0 * 1000.0; // ms
        TIMED_IS_READY IDLE_WAIT_TIMER_FOR_CONSOLIDATION;
        
        //double IDLE_WAIT_TIME = 1000.0; // ms
        //TIMED_IS_READY IDLE_WAIT_TIMER;

        // Temporary Holdings
        std::vector<Message> temp_chat_history;

        //int ROUTINE = 0; // 0 = idle, 1 = consolidation, 2 = other routine, etc.

        // Routine 1: Consolidation Routine
        int chat_history_processing_stage = 0;
        // 0 = idle
        // 1 = chat history requested.
        // 2 = chat history ready for processing
        // 3 = chat history is processed.

        // Routine 2:

        // Communication variables
        std::atomic<bool> PROCESSING{false};
        std::atomic<bool> INTERUPT{false};
        std::atomic<bool> CHAT_FINISHED{false};


    public:

        SIDETRACK_SIGNALS SIGNALS;

        bool RUN = false;

        SIDETRACK_CLASS();

        void create(OLLAMA_SYSTEM_PROPERTIES Ollama_Properties);

        void thread_main();

        void thread_start();
        void thread_stop();

        void check(ollama_system& main_instance);
};

#endif