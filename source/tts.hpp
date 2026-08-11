#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

// Offline text-to-speech via the local espeak-ng + aplay binaries.
// No network access, no model downloads - everything runs on-device.
class TextToSpeech {
public:
    explicit TextToSpeech(std::string voice = "en-us", int wordsPerMinute = 170);
    ~TextToSpeech();

    TextToSpeech(const TextToSpeech&) = delete;
    TextToSpeech& operator=(const TextToSpeech&) = delete;

    // Synthesizes and plays text, blocking until playback finishes or stop() is called.
    bool speak(const std::string& text);

    // Queues text to be spoken on a background worker thread and returns immediately.
    // If nothing is currently speaking, playback starts right away. If something is
    // already speaking, this text is appended and plays after everything queued ahead
    // of it. Does not interrupt speech already in progress - call stop() for that.
    void speakAsync(const std::string& text);

    // Interrupts speech currently playing and clears anything queued via speakAsync().
    // If called while text is still being synthesized (before playback starts), that
    // utterance is dropped once synthesis finishes rather than being played.
    void stop();

    bool isSpeaking() const;

    void setVoice(const std::string& voice);
    void setSpeed(int wordsPerMinute);

private:
    bool synthesize(const std::string& text, const std::string& wavPath);
    bool playback(const std::string& wavPath);
    void workerLoop();

    std::string voice_;
    int speed_;
    std::atomic<bool> speaking_{false};
    std::atomic<int> playerPid_{-1};
    std::atomic<bool> stopRequested_{false};
    std::mutex speakMutex_;

    std::deque<std::string> queue_;
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    bool shuttingDown_ = false;
    std::thread workerThread_;
};
