#ifndef AUDIO_CONTTROL_CPP
#define AUDIO_CONTTROL_CPP

#include "audio_control.h"

void AUDIO_CONTROL_CLASS::adjust_audio_files(double Time)
{
    (void)Time;

    if (voca == nullptr) return;

    bool tts_is_speaking = tts.isSpeaking();
    bool tts_speaking_changed = (tts_is_speaking != tts_was_speaking);
    tts_was_speaking = tts_is_speaking;

    // If TTS starts speaking, pause Voca so it doesn't hear olli's own
    // voice. If TTS stops speaking, resume listening. Voca handles its own
    // wake-word detection and auto-sleep timeout internally - nothing else
    // to poll here.
    if (tts_speaking_changed)
    {
        if (tts_is_speaking)
        {
            voca->pause();
        }
        else
        {
            voca->resume();
        }
    }
}

AUDIO_CONTROL_CLASS::AUDIO_CONTROL_CLASS()
{
}

void AUDIO_CONTROL_CLASS::create(const std::filesystem::path& filePath)
{
    settings_path = filePath;

    Voca::Callbacks callbacks;

    // Ordinary transcript text is pulled directly from voca's own
    // textAvailable()/getNextLine() in popVocaEvent() below - no need to
    // duplicate it into a callback-fed queue here too.

    callbacks.onInterrupt = [this](const std::string& /*text*/)
    {
        // Interrupt-only: signals the main loop to stop TTS/sidetrack
        // without submitting anything new as a chat message.
        std::lock_guard<std::mutex> lock(voca_events_mutex);
        voca_events.push_back(VOCA_EVENT{"", ""});
    };

    // Same queue/mutex onInterrupt above already uses - not a raw cout,
    // since this callback runs on Voca's own thread and a direct terminal
    // write would either get overwritten by the next ncurses redraw or
    // briefly corrupt the screen. IO_WORKER_CLASS::thread_main() (io_worker
    // .cpp) is the one place that actually turns this into a displayed
    // system message, via COMMS::log() - same as any other status text.
    callbacks.onWake = [this](const std::string& trigger)
    {
        std::lock_guard<std::mutex> lock(voca_events_mutex);
        VOCA_EVENT event;
        event.status_message = "[VOCA] awake (\"" + trigger + "\")\n";
        voca_events.push_back(event);
    };

    callbacks.onSleep = [this]()
    {
        std::lock_guard<std::mutex> lock(voca_events_mutex);
        VOCA_EVENT event;
        event.status_message = "[VOCA] asleep\n";
        voca_events.push_back(event);
    };

    voca = std::make_unique<Voca>((settings_path / "models" / "ggml-small.en.bin").string(),
                                   std::move(callbacks));
    if (!voca->start())
    {
        std::cerr << "AUDIO_CONTROL_CLASS: Voca failed to start - speech-to-text disabled.\n";
        voca.reset();
    }
}

void AUDIO_CONTROL_CLASS::VOCA_manual_set(int Command)
{
    if (voca == nullptr) return;

    switch (Command)
    {
        case DEF_VOCA_SLEEP:
            voca->sleep();
            break;
        case DEF_VOCA_PAUSE:
            voca->pause();
            break;
        case DEF_VOCA_LISTEN:
            voca->wake();
            voca->resume();
            break;
        default:
            std::cerr << "Invalid VOCA command: " << Command << std::endl;
            break;
    }
}

void AUDIO_CONTROL_CLASS::speak(const std::string& text)
{
    tts.speakAsync(text);
}

void AUDIO_CONTROL_CLASS::stop_speaking()
{
    tts.stop();
}

bool AUDIO_CONTROL_CLASS::popVocaEvent(VOCA_EVENT& out)
{
    // Interrupt-only events take priority - they signal "stop what's
    // happening now" and shouldn't wait behind queued transcript text.
    {
        std::lock_guard<std::mutex> lock(voca_events_mutex);
        if (!voca_events.empty())
        {
            out = std::move(voca_events.front());
            voca_events.pop_front();
            return true;
        }
    }

    if (voca != nullptr && voca->textAvailable())
    {
        out = VOCA_EVENT{voca->getNextLine(), ""};
        return true;
    }

    return false;
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
    if (voca != nullptr) voca->stop();

    // See SIDETRACK_CLASS::thread_stop for why this actually waits on the
    // thread instead of just flipping a flag and returning.
    RUN = false;
    THREAD_CONTROL.wait_for_thread_to_finish();
}


#endif
