#include "io_worker.h"

#include <chrono>
#include <thread>

#include "olla.h"
#include "sidetrack.h"
#include "audio_control.h"

IO_WORKER_CLASS::~IO_WORKER_CLASS()
{
}

void IO_WORKER_CLASS::signal_interrupt()
{
    INTERUPTED.store(true);
}

void IO_WORKER_CLASS::clear_interrupt()
{
    INTERUPTED.store(false);
}

bool IO_WORKER_CLASS::interrupted() const
{
    return INTERUPTED.load();
}

void IO_WORKER_CLASS::create(ollama_system& chat_ref, SIDETRACK_CLASS& sidetrack_ref, AUDIO_CONTROL_CLASS& audio_ref)
{
    chat = &chat_ref;
    sidetrack = &sidetrack_ref;
    audio = &audio_ref;
}

void IO_WORKER_CLASS::thread_start()
{
    THREAD_CONTROL.create(1000);
    THREAD_CONTROL.start_render_thread([&]() { thread_main(); });
}

void IO_WORKER_CLASS::thread_stop()
{
    // Signal exit, then actually wait for the background thread to finish
    // before returning - key_input/output are destroyed right after this
    // object is (reverse member order), so thread_main() must be fully
    // joined first or it could still be touching them mid-teardown.
    RUN = false;
    THREAD_CONTROL.wait_for_thread_to_finish();
}

void IO_WORKER_CLASS::thread_main()
{
    RUN = true;
    while (RUN)
    {
        if (!INTERUPTED.load())
        {
            PROCESSING.store(true);

            // 0. Opposite-direction request from the main thread (see
            // COMMS::close_chat_log_requested's comment, comms.h) - not
            // part of the send/stop_requested/exit_requested relay below,
            // both sides touch this field directly since it's atomic.
            if (chat->comms.close_chat_log_requested.load())
            {
                chat->comms.close_chat_log_requested.store(false);
                output.close_chat_log();
            }

            // 1. Keyboard - non-blocking, drains whatever's available.
            key_input.keyboard_input();

            // 2. Ctrl+C - checked early, same priority it had in main.cpp's
            // old loop (wins over anything else this tick).
            if (key_input.EXIT_REQUESTED)
            {
                key_input.EXIT_REQUESTED = false;
                staged.exit_requested = true;
            }

            // 3. Pop at most one pending voice event this tick and feed it
            // into key_input the same way a typed line would arrive -
            // mirrors main.cpp's old popVocaEvent block exactly. A wake/
            // sleep status_message (see VOCA_EVENT, audio_control.h) is
            // neither a transcript nor an interrupt - just log it and
            // skip the rest of this block for that event.
            VOCA_EVENT voca_event;
            if (audio->popVocaEvent(voca_event))
            {
                if (!voca_event.status_message.empty())
                {
                    chat->comms.log(voca_event.status_message);
                }
                else
                {
                    if (!voca_event.text.empty())
                    {
                        // Trailing "\n" added here so LINE is newline-terminated
                        // the same way keyboard_input()'s own typed-line
                        // accumulation already is - step 5 below echoes LINE to
                        // output.user_input in exactly one place, uniformly for
                        // both paths, instead of each path echoing separately
                        // (the original main.cpp/input() split did this in two
                        // places for the voice path specifically, appending the
                        // text twice - once with the newline here, once without
                        // it from input()'s own echo).
                        key_input.LINE = voca_event.text + "\n";
                        key_input.ENTER_PRESSED = true;
                    }
                    key_input.INTERRUPTED = true;
                }
            }

            // 4. Interrupt handling - sidetrack/audio are interrupted
            // directly (this worker already holds those references for
            // its own draining/polling below, so this isn't part of the
            // comms/exchange() surface at all). Only the "stop chat's own
            // in-flight generation" part needs to reach the main thread,
            // since only ollama_system can stop its own chat_thread - that
            // goes through staged.stop_requested instead.
            if (key_input.INTERRUPTED)
            {
                sidetrack->SIGNALS.INTERUPT_SIGNAL = true;
                audio->stop_speaking();
                staged.stop_requested = true;
                key_input.INTERRUPTED = false; // consumed - NOT a blanket
                                                // reset(), which would also
                                                // wipe LINE mid-typing (see
                                                // step 5's own note)
            }

            // 5. Submission - echo to the screen here, once, uniformly for
            // typed and voice input (see the comment on LINE's assignment
            // above). Don't stomp a not-yet-relayed submission. Only
            // clears LINE/ENTER_PRESSED here, not via key_input.reset() -
            // that would also clear LINE on ticks where the user is still
            // mid-typing (ENTER_PRESSED still false), erasing whatever
            // they'd typed so far before they ever got to press Enter.
            if (key_input.ENTER_PRESSED)
            {
                output.user_input += key_input.LINE;

                if (!staged.send)
                {
                    staged.send = true;
                    staged.submitted_line = key_input.LINE;
                }

                key_input.LINE.clear();
                key_input.ENTER_PRESSED = false;
            }

            // 6. Drain comms into the screen - unchanged calls, existing
            // output_buffer_mutex protection, just relocated from
            // main.cpp's old loop.
            output.get_response(chat->comms);
            chat->pull_background_output(output);
            sidetrack->pull_output(output);

            // 7. Draw.
            if (USE_NCURSES)
            {
                output.display_with_ncurses(key_input, tool_names);
            }
            else
            {
                output.display();
            }

            PROCESSING.store(false);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(PROPS.INTERVAL)));
    }
}

void IO_WORKER_CLASS::exchange(COMMS& comms, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list)
{
    if (!PROPS.BLOCKING) return;

    signal_interrupt();

    // Wait out any background pass already in flight - INTERUPTED only
    // stops a NEW pass from starting, it doesn't abort one already
    // running.
    while (PROCESSING.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Safe to touch tool_names here - thread_main() is confirmed not
    // running (the wait above), so this doesn't race its own read of
    // tool_names down in the display step. Names come from actually calling
    // each tool's own register_tool() into a throwaway json array, rather
    // than adding a separate name-only accessor to TOOL_BASE - this reuses
    // the exact same logic send() itself relies on (see olla.cpp) instead of
    // a second place to keep in sync.
    if (chat != nullptr)
    {
        tool_names.clear();
        json tmp_tools = json::array();
        for (auto& tool : tools_list)
            tool->register_tool(*chat, tmp_tools);
        for (auto& entry : tmp_tools)
        {
            std::string name = entry.value("function", json::object()).value("name", "");
            if (!name.empty()) tool_names.push_back(name);
        }
    }

    if (staged.stop_requested)
    {
        comms.stop_requested = true;
        staged.stop_requested = false;
    }

    // Guarded so a submission the consuming side hasn't gotten to yet
    // (comms.send still true from a previous exchange()) never gets
    // silently overwritten by a newer one - same "at most one event
    // surfaces at a time, backlog drains over the next few ticks"
    // principle olli's voice-input handling already relies on.
    if (staged.send && !comms.send)
    {
        comms.send = true;
        comms.submitted_line = staged.submitted_line;
        staged.send = false;
        staged.submitted_line.clear();
    }

    if (staged.exit_requested)
    {
        comms.exit_requested = true;
        staged.exit_requested = false;
    }

    clear_interrupt();
}
