#ifndef AUDIO_CONTTROL_H
#define AUDIO_CONTTROL_H

#include <iostream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <mutex>
#include <deque>
#include <memory>

#include "fled_time.h"
#include "threading.h"
#include "tts.hpp"
#include "voca.hpp"

#define DEF_VOCA_SLEEP  0
#define DEF_VOCA_PAUSE  1
#define DEF_VOCA_LISTEN 2

/**
 * @file audio_control.h
 * @brief Header file for audio control functionality.
 *
 * This file contains the AUDIO_CONTROL_CLASS which owns both text-to-speech
 * (via TextToSpeech) and speech-to-text (via Voca) in-process, and
 * coordinates the two: while TTS is speaking, Voca is paused so it doesn't
 * hear olli's own voice; once speech stops, Voca resumes listening.
 */

/**
 * @struct VOCA_EVENT
 * @brief One piece of voice input handed off from Voca's background
 * transcription thread to the main loop (see popVocaEvent()).
 */
struct VOCA_EVENT {
    // Text to submit as chat input. Empty for an interrupt-only event (e.g.
    // "stop talking" heard while TTS is speaking) - the caller should still
    // treat the event as an interrupt, just not submit anything new.
    std::string text;
};

/**
 * @class AUDIO_CONTROL_CLASS
 * @brief Owns text-to-speech and speech-to-text and coordinates them.
 */
class AUDIO_CONTROL_CLASS
{
    private:
        double INTERVAL = 500;  //ms
        THREADING_INFO  THREAD_CONTROL;  // Controls: update_frame_thread()
        std::filesystem::path settings_path;

        TextToSpeech tts;
        bool tts_was_speaking = false; // last-seen isSpeaking(), to detect start/stop transitions

        std::unique_ptr<Voca> voca;

        std::mutex voca_events_mutex;
        std::deque<VOCA_EVENT> voca_events;

        void adjust_audio_files(double Time);

    public:
        bool RUN = false;

        AUDIO_CONTROL_CLASS();

        void create(const std::filesystem::path& filePath);

        // Forces Voca directly into a state, bypassing wake-word detection
        // (e.g. a manual override from a chat tool - see olla.cpp).
        void VOCA_manual_set(int Command);

        // Queues text to be spoken (see TextToSpeech::speakAsync).
        void speak(const std::string& text);

        // Interrupts speech in progress and clears anything queued.
        void stop_speaking();

        // Pops the next pending voice event, if any. Called from the main
        // loop (see main.cpp) to feed transcripts into chat the same way a
        // typed line would be. Returns false if nothing is pending.
        bool popVocaEvent(VOCA_EVENT& out);

        void thread_main();

        void thread_start();
        void thread_stop();
};

#endif