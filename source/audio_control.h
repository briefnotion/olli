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

/**
 * @file audio_control.h
 * @brief Header file for audio control functionality.
 *
 * This file contains the AUDIO_CONTROL_CLASS which manages audio settings
 * and controls interactions between LIRA and VOCA systems.
 */

/**
 * @struct LIRA_CONTROL_CONFIG
 * @brief Configuration structure for LIRA audio control settings.
 *
 * Contains flags for interrupt handling and speaking status from the LIRA system.
 */
struct LIRA_CONTROL_CONFIG {
    bool interrupt = false;    /**< Flag indicating if audio should be interrupted */
    bool is_speaking = false;  /**< Flag indicating if LIRA is currently speaking */
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LIRA_CONTROL_CONFIG, interrupt, is_speaking)        

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
 * @brief Main class for coordinating audio control between LIRA and VOCA systems.
 *
 * This class manages the interaction between LIRA (speech synthesis) and VOCA (voice assistant)
 * systems by monitoring their status files and sending appropriate commands to coordinate
 * their audio behavior.
 */
class AUDIO_CONTROL_CLASS
{
    private:
        double INTERVAL = 500;  //ms
        THREADING_INFO  THREAD_CONTROL;  // Controls: update_frame_thread()
        std::filesystem::path settings_path;

        std::filesystem::file_time_type LIRA_lastKnownTime;
        LIRA_CONTROL_CONFIG LIRA_SETTINGS;
        
        std::filesystem::file_time_type VOCA_lastKnownTime;
        VOCA_CONTROL_CONFIG VOCA_SETTINGS;

        VOCA_COMMAND_CONFIG VOCA_COMMAND_SETTINGS;

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

        void adjust_audio_files();

    public:
        bool RUN = false;

        AUDIO_CONTROL_CLASS(const std::filesystem::path& filePath);

        void thread_main();

        void thread_start();
        void thread_stop();
};

#endif