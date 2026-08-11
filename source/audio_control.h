#ifndef AUDIO_CONTTROL_H
#define AUDIO_CONTTROL_H

#include <iostream>
#include <fstream>
#include <filesystem>
#include <thread>
#include <chrono>

#include <nlohmann/json.hpp>

#include "fled_time.h"
#include "threading.h"
#include "tts.hpp"

#define DEF_VOCA_SLEEP  0
#define DEF_VOCA_PAUSE  1
#define DEF_VOCA_LISTEN 2

/**
 * @file audio_control.h
 * @brief Header file for audio control functionality.
 *
 * This file contains the AUDIO_CONTROL_CLASS which owns text-to-speech
 * (via the in-process TextToSpeech class) and coordinates it with VOCA.
 */

/**
 * @struct VOCA_CONTROL_CONFIG
 * @brief Configuration structure for VOCA status settings.
 *
 * Contains status information, timestamp, and state flags from the VOCA system.
 */
struct VOCA_CONTROL_CONFIG {
    string status = "";        /**< Current status string of VOCA system */
    double timestamp = 0.0;    /**< Timestamp of last status update */
    bool is_awake = false;     /**< Flag indicating if VOCA is awake/active */
    bool is_busy = false;      /**< Flag indicating if VOCA is busy processing */
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(VOCA_CONTROL_CONFIG, status, timestamp, is_awake, is_busy)

/**
 * @struct VOCA_COMMAND_CONFIG
 * @brief Configuration structure for VOCA command settings.
 *
 * Contains command strings and update timestamps for controlling VOCA behavior.
 */
struct VOCA_COMMAND_CONFIG {
    string command = "";       /**< Command string to send to VOCA */
    double last_update = 0.0;  /**< Timestamp of last command update */
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(VOCA_COMMAND_CONFIG, command, last_update)


/**
 * @class AUDIO_CONTROL_CLASS
 * @brief Owns text-to-speech and coordinates it with VOCA (voice assistant).
 *
 * Speech is synthesized/played in-process via TextToSpeech. VOCA is a
 * separate Python process, so it's still coordinated through its status/
 * command files: while TTS is speaking, VOCA is paused so it doesn't hear
 * olli's own voice; once speech stops, VOCA resumes listening.
 */
class AUDIO_CONTROL_CLASS
{
    private:
        double INTERVAL = 500;  //ms
        THREADING_INFO  THREAD_CONTROL;  // Controls: update_frame_thread()
        std::filesystem::path settings_path;

        TextToSpeech tts;
        bool tts_was_speaking = false; // last-seen isSpeaking(), to detect start/stop transitions

        std::filesystem::file_time_type VOCA_lastKnownTime;
        VOCA_CONTROL_CONFIG VOCA_SETTINGS;

        VOCA_COMMAND_CONFIG VOCA_COMMAND_SETTINGS;

        bool VOCA_STATUS_CHANGED = false;

        bool CONTROL_AWAKE = true;

        TIMED_IS_READY  AUDIO_TIMER;

        int VOCA_REQUESTED_CHANGE = -1;

        /**
         * @brief Templated function to check and load configuration from JSON file.
         * @tparam T The type of configuration struct to load.
         * @param filePath Path to the JSON configuration file.
         * @param lastWriteTime Reference to store the last write time of the file.
         * @param config Reference to the configuration struct to populate.
         * @return true if file was loaded successfully, false otherwise.
         */
        template <typename T>
        bool checkAndLoadFile(const std::filesystem::path& filePath, 
                            std::filesystem::file_time_type& lastWriteTime, 
                            T& config) {
            
            if (!std::filesystem::exists(filePath)) return false;

            try {
                auto currentWriteTime = std::filesystem::last_write_time(filePath);

                if (currentWriteTime != lastWriteTime) {
                    lastWriteTime = currentWriteTime;

                    std::ifstream file(filePath);
                    if (!file.is_open()) return false;

                    nlohmann::json j;
                    file >> j;

                    // This single line now works for ANY struct passed in.
                    // It populates the struct 'config' with the values found in 'j'.
                    config = j.get<T>();

                    return true;
                }
            } catch (const std::exception& e) {
                std::cerr << "Load Error: " << e.what() << std::endl;
            }

            return false;
        }

        /**
         * Portable Write Function
         * -----------------------
         * Similarly, this can now take any struct and save it as JSON.
         */
        template <typename T>
        void writeFile(const std::filesystem::path& filePath, const T& config) {
            try {
                nlohmann::json j = config; // Converts struct to JSON automatically

                // Add the system timestamp
                auto now = std::chrono::system_clock::now();
                auto duration = now.time_since_epoch();
                j["last_update"] = std::chrono::duration<double>(duration).count();

                std::ofstream file(filePath);
                if (file.is_open()) {
                    file << j.dump(4);
                }
            } catch (const std::exception& e) {
                std::cerr << "Write Error: " << e.what() << std::endl;
            }
        }

        void VOCA_set(int Command);
        void adjust_audio_files(double Time);

    public:
        bool RUN = false;

        AUDIO_CONTROL_CLASS();

        void create(const std::filesystem::path& filePath);

        void VOCA_manual_set(int Command);

        // Queues text to be spoken (see TextToSpeech::speakAsync).
        void speak(const std::string& text);

        // Interrupts speech in progress and clears anything queued.
        void stop_speaking();

        void thread_main();

        void thread_start();
        void thread_stop();
};

#endif