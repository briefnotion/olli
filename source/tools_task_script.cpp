#include "tools_task_script.h"

#include "comms.h"
#include "helper_olli.h"
#include "olla.h"
#include "stringthings.h"
#include "tools.h"

#include <thread>

// Confines a task-authored name to a single path segment - see the header's
// own comment for the full rationale. '/' and '\\' stripped outright so the
// result can never create or escape into a subdirectory; spaces become
// underscores per request.
std::string sanitize_path_segment(const std::string& raw)
{
    std::string result;
    result.reserve(raw.size());
    for (char c : raw)
    {
        if (c == '/' || c == '\\') continue;
        result += (c == ' ') ? '_' : c;
    }

    // With '/' gone, only an exact ".."/"." match still means anything
    // special to the filesystem - anything else containing dots (e.g.
    // "..evil") is just a literal, harmless name.
    if (result.empty() || result == ".." || result == ".") result = "_";

    return result;
}

void command_pause(SCRIPT_STATE& state, COMMS& instance_comms, bool& keyboard_was_enabled)
{
    keyboard_was_enabled = instance_comms.ENABLE_KEYBOARD_INPUT;
    instance_comms.ENABLE_KEYBOARD_INPUT = true;
    instance_comms.INPUT_FROM_LLM = "--------------------------\nPRESS ENTER TO CONTINUE\n";
    state = SCRIPT_STATE::WAIT_ENTER;
}

void command_ask(SCRIPT_STATE& state, COMMS& instance_comms, const std::string& command, bool& keyboard_was_enabled)
{
    keyboard_was_enabled = instance_comms.ENABLE_KEYBOARD_INPUT;
    instance_comms.ENABLE_KEYBOARD_INPUT = true;
    instance_comms.INPUT_FROM_LLM = "--------------------------\nREQUEST: " + command.substr(5) + "\n";
    state = SCRIPT_STATE::WAIT_ASK;
}

void command_print(size_t& i, COMMS& instance_comms, const std::string& command)
{
    instance_comms.INPUT_FROM_LLM = command.substr(7) + "\n";
    ++i;
}

// files_dir is OLLI_DIRECTORY/files/<sanitized TASK_DIRECTORY or TASK_NAME>
// (see TOOL_TASK_RUNNER::handle_tool(), tools.cpp) - a plain local there,
// not working_dir, and not cleaned up when handle_tool() returns: unlike
// working_dir's own scratch space, whatever FILE_IN/FILE_APPEND leaves in
// files_dir is meant to persist indefinitely.
void command_file_in(SCRIPT_STATE& state, std::string& current_input, COMMS& instance_comms, const std::string& command, const std::filesystem::path& files_dir, bool& keyboard_was_enabled)
{
    // "[FILE_IN:" is 9 chars; the trailing "]" is 1 more.
    std::string raw_name = command.substr(9, command.size() - 9 - 1);
    std::filesystem::path file_path = files_dir / sanitize_path_segment(raw_name);

    std::string file_content;
    if (read_file(file_path, file_content))
    {
        current_input = file_content;
        state = SCRIPT_STATE::EXECUTE_COMMAND;
    }
    else
    {
        // Falls back to the same WAIT_ASK state [ASK] uses -
        // pause, show a request, take whatever the user
        // types/pastes as current_input, then continue the
        // script normally (i still advances via
        // WAIT_RESPONSE below, same as any other line) -
        // rather than aborting the whole automation over one
        // missing file.
        keyboard_was_enabled = instance_comms.ENABLE_KEYBOARD_INPUT;
        instance_comms.ENABLE_KEYBOARD_INPUT = true;
        instance_comms.INPUT_FROM_LLM = "--------------------------\nCould not find '" + raw_name + "'. Please paste it here:\n";
        state = SCRIPT_STATE::WAIT_ASK;
    }
}

void command_file_append(size_t& i, ollama_system& instance, const std::string& command, const std::filesystem::path& files_dir)
{
    // "[FILE_APPEND:" is 13 chars; the trailing "]" is 1 more.
    std::string raw_name = command.substr(13, command.size() - 13 - 1);
    std::filesystem::path file_path = files_dir / sanitize_path_segment(raw_name);

    // Keep it simple for now: plain concatenation of
    // whatever the previous line's response was, no
    // separator between entries.
    std::filesystem::create_directories(files_dir);
    write_file(file_path, instance.last_received.response, /*append=*/true);
    ++i;
}

void command_keyboard_input(size_t& i, COMMS& instance_comms, const std::string& command)
{
    // "[KEYBOARD_INPUT:" is 16 chars; the trailing "]" is 1 more.
    std::string value = command.substr(16, command.size() - 16 - 1);
    instance_comms.ENABLE_KEYBOARD_INPUT = (value == "on");
    ++i;
}

void command_tts_output(size_t& i, COMMS& instance_comms, const std::string& command)
{
    // "[TTS_OUTPUT:" is 12 chars; the trailing "]" is 1 more.
    std::string value = command.substr(12, command.size() - 12 - 1);
    instance_comms.ENABLE_TTS_OUTPUT = (value == "on");
    ++i;
}

void command_plain(SCRIPT_STATE& state, std::string& current_input, const std::string& command)
{
    current_input = command;
    state = SCRIPT_STATE::EXECUTE_COMMAND;
}

// Prefix-matching dispatch only - each [MARKER]'s actual logic lives in its
// own command_*() function above.
void command_get_command(SCRIPT_STATE& state, size_t& i, std::string& current_input, const TASK_SIMPLE& found_task, ollama_system& instance, COMMS& instance_comms, const std::filesystem::path& files_dir, bool& keyboard_was_enabled)
{
    if (i >= found_task.COMMANDS.size())
    {
        state = SCRIPT_STATE::DONE;
        return;
    }

    const std::string& command = found_task.COMMANDS[i];

    if (starts_with(command, "[PAUSE]"))
    {
        command_pause(state, instance_comms, keyboard_was_enabled);
    }
    else if (starts_with(command, "[ASK]"))
    {
        command_ask(state, instance_comms, command, keyboard_was_enabled);
    }
    else if (starts_with(command, "[PRINT]"))
    {
        command_print(i, instance_comms, command);
    }
    else if (starts_with(command, "[FILE_IN:") && command.back() == ']')
    {
        command_file_in(state, current_input, instance_comms, command, files_dir, keyboard_was_enabled);
    }
    else if (starts_with(command, "[FILE_APPEND:") && command.back() == ']')
    {
        command_file_append(i, instance, command, files_dir);
    }
    else if (starts_with(command, "[KEYBOARD_INPUT:") && command.back() == ']')
    {
        command_keyboard_input(i, instance_comms, command);
    }
    else if (starts_with(command, "[TTS_OUTPUT:") && command.back() == ']')
    {
        command_tts_output(i, instance_comms, command);
    }
#if 0
    // Draft, not yet implemented - sketched 2026-09-06 during a
    // "what commands are worth adding" discussion, deliberately
    // left inert until actually being built. Each one below notes
    // what it would additionally need beyond what's already
    // threaded through this function's own parameter list.

    else if (starts_with(command, "[WAIT:") && command.back() == ']')
    {
        // Pure timed pause - no Enter needed, unlike [PAUSE].
        // Would need a new SCRIPT_STATE::WAIT_TIMER value and a
        // deadline to check on later ticks - meaning a new
        // std::chrono::steady_clock::time_point& parameter
        // threaded in from handle_tool()'s while loop the same
        // way i/current_input already are, since this function
        // has no state of its own between calls.
        std::string seconds_text = command.substr(6, command.size() - 6 - 1);
        double seconds = std::stod(seconds_text);
        // wait_until = std::chrono::steady_clock::now() +
        //     std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        //         std::chrono::duration<double>(seconds));
        (void)seconds;
        // state = SCRIPT_STATE::WAIT_TIMER;
    }
    else if (starts_with(command, "[SET:") && command.back() == ']')
    {
        // Captures the most recent answer/response under a name
        // so later lines can reference it - e.g. [ASK]What's your
        // name? ... [SET:name] ... three lines later,
        // [PRINT]thanks, {name}. Needs a
        // std::map<std::string, std::string>& variables parameter
        // threaded through (another new argument), AND a
        // substitution pass applied wherever text is currently
        // used verbatim - [PRINT], [ASK]'s REQUEST text, and the
        // plain-command branch's current_input would all need to
        // run their text through a small
        // substitute_variables(text, variables) helper first. Not
        // a single self-contained case the way the others are -
        // touches multiple existing branches.
        std::string var_name = command.substr(5, command.size() - 5 - 1);
        // variables[var_name] = instance.last_received.response;
        (void)var_name;
        ++i;
    }
    else if (starts_with(command, "[RANDOM:") && command.back() == ']')
    {
        // Picks one of several '|'-separated options at random,
        // then treats the chosen one exactly like a plain command
        // line. Self-contained - no new parameters needed, just a
        // <random> include and a static/thread_local RNG (this is
        // a free function with no member state to keep one on).
        std::string options_text = command.substr(8, command.size() - 8 - 1);
        std::vector<std::string> options;
        // split options_text on '|' into options...
        // static std::mt19937 rng(std::random_device{}());
        // std::uniform_int_distribution<size_t> dist(0, options.size() - 1);
        // current_input = options[dist(rng)];
        (void)options;
        // state = SCRIPT_STATE::EXECUTE_COMMAND;
    }
    else if (starts_with(command, "[RUN:") && command.back() == ']')
    {
        // Invokes another named task from inside this one, so
        // scripts can compose instead of duplicating commands.
        // The hardest of the four to actually wire up: this
        // function only ever sees found_task.COMMANDS, a fixed
        // const reference into the already-matched task from
        // handle_tool() - there's no access to
        // task_manager.TASK_LIST here to look another task up by
        // name at all (deliberately not threaded through - see
        // this function's own top comment on why it can't reach
        // OLLI_DIRECTORY/task_manager/chat). Splicing in another
        // task's commands would also mean found_task/i can no
        // longer just index one fixed COMMANDS vector - needs
        // something like a small call stack of (task, index)
        // pairs so a sub-task can finish and control returns to
        // the right place in the caller.
        std::string sub_task_name = command.substr(5, command.size() - 5 - 1);
        (void)sub_task_name;
    }
#endif
#if 0
    // Draft, not yet implemented - sketched 2026-09-06 for the
    // "timer callback lands mid-[ASK]" race (a remote tool's
    // event - e.g. a timer expiring - can interleave with a
    // script's own EXECUTE_COMMAND/WAIT_ASK turns today, since
    // neither pending_tool_calls' drain (tools.cpp,
    // handle_instance_tools()) nor integrate_tool_result()'s
    // direct, unconditional send() (called from
    // TOOL_REMOTE::monitor_tool() via remote_tools.cpp) know or
    // care what SCRIPT_STATE a running automation is in).
    //
    // The idea: while a script is running, redirect BOTH of
    // those capture points into one FIFO queue on the instance
    // (a real queue, not a single pending slot - confirmed
    // multiple queued interrupts should just be handled one by
    // one, in order, not batched) instead of acting immediately.
    // A new [FLUSH_INTERRUPTS] marker, placed by the script
    // author wherever they judge it safe (not hardcoded to
    // "only at the very end" - that would also delay the timer's
    // real action, e.g. the light actually blinking, not just
    // its narration), drains the queue at that point.
    //
    // Real hookup would need, beyond what's sketched here:
    //   1. A queue member on the instance - something like
    //      struct PENDING_INTERRUPT { std::string narration;
    //      bool has_action; ToolCall action; }; plus a
    //      std::queue<PENDING_INTERRUPT>. Where exactly it lives
    //      (ollama_system itself vs. threaded through as a new
    //      advance_script_state() parameter) is still open.
    //   2. remote_tools.cpp's event handler (~line 339-356)
    //      redirecting into that queue instead of calling
    //      integrate_tool_result()/pushing pending_tool_calls
    //      directly, gated on "a script is currently running" -
    //      not sketched here at all, lives in a different file.
    //   3. Draining one queued item at a time can't happen in a
    //      single GET_COMMAND tick the way the cases above do -
    //      each queued item's own narration is itself an async
    //      send() that needs its own WAIT_RESPONSE-style wait.
    //      Needs its own new SCRIPT_STATE (e.g. FLUSHING_
    //      INTERRUPTS) that pops and dispatches one item, waits
    //      for it to finish, then loops back for the next item
    //      until the queue's empty, only then advancing i.
    else if (command == "[FLUSH_INTERRUPTS]")
    {
        // if (interrupt_bucket.empty())
        // {
        //     ++i;
        // }
        // else
        // {
        //     state = SCRIPT_STATE::FLUSHING_INTERRUPTS;
        // }
    }
#endif
    else
    {
        command_plain(state, current_input, command);
    }
}

void command_wait_enter(SCRIPT_STATE& state, size_t& i, COMMS& instance_comms, bool& keyboard_was_enabled)
{
    if (instance_comms.ENTER_PRESSED)
    {
        instance_comms.ENTER_PRESSED = false;
        instance_comms.ENABLE_KEYBOARD_INPUT = keyboard_was_enabled;
        ++i;
        state = SCRIPT_STATE::GET_COMMAND;
    }
}

void command_wait_ask(SCRIPT_STATE& state, std::string& current_input, COMMS& instance_comms, bool& keyboard_was_enabled)
{
    if (instance_comms.ENTER_PRESSED)
    {
        instance_comms.ENTER_PRESSED = false;
        instance_comms.ENABLE_KEYBOARD_INPUT = keyboard_was_enabled;
        current_input = instance_comms.INPUT_FROM_USER;
        state = SCRIPT_STATE::EXECUTE_COMMAND;
    }
}

void command_execute_command(SCRIPT_STATE& state, const std::string& current_input, ollama_system& instance, COMMS& instance_comms, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list)
{
    instance_comms.INPUT_FROM_LLM = "--------------------------\nINPUT: " + current_input + "\n";
    instance_comms.INPUT_FROM_USER = current_input;

    // Run send() on its own thread instead of calling it
    // directly here - it's a blocking HTTP call, so calling
    // it inline would freeze this loop's own
    // io_worker.exchange() below for the whole request,
    // preventing anything from streaming to screen until it
    // returned. Same pattern as ollama_system::input()
    // (olla.cpp) and sidetrack.cpp's own
    // start_second_guess_call().
    instance.status.interrupt_signal = false;
    instance.is_processing = true;
    if (instance.chat_thread.joinable()) instance.chat_thread.join();
    instance.chat_thread = std::thread([&instance, &instance_comms, &tools_list]()
    {
        instance.send(tools_list, instance_comms, "user");
        instance.is_processing = false;
    });

    state = SCRIPT_STATE::WAIT_RESPONSE;
}

void command_wait_response(SCRIPT_STATE& state, size_t& i, ollama_system& instance)
{
    if (!instance.is_processing && instance.chat_thread.joinable())
    {
        instance.chat_thread.join();
    }

    bool response_finished = !instance.is_processing &&
                              instance.last_received.complete &&
                              instance.last_received.tool_calls.empty();

    if (response_finished)
    {
        instance.last_received.complete = false;
        ++i;
        state = SCRIPT_STATE::GET_COMMAND;
    }
}

void advance_script_state(SCRIPT_STATE& state, size_t& i, std::string& current_input, const TASK_SIMPLE& found_task, ollama_system& instance, COMMS& instance_comms, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, const std::filesystem::path& files_dir, bool& keyboard_was_enabled)
{
    switch (state)
    {
        case SCRIPT_STATE::GET_COMMAND:
            command_get_command(state, i, current_input, found_task, instance, instance_comms, files_dir, keyboard_was_enabled);
            break;

        case SCRIPT_STATE::WAIT_ENTER:
            command_wait_enter(state, i, instance_comms, keyboard_was_enabled);
            break;

        case SCRIPT_STATE::WAIT_ASK:
            command_wait_ask(state, current_input, instance_comms, keyboard_was_enabled);
            break;

        case SCRIPT_STATE::EXECUTE_COMMAND:
            command_execute_command(state, current_input, instance, instance_comms, tools_list);
            break;

        case SCRIPT_STATE::WAIT_RESPONSE:
            command_wait_response(state, i, instance);
            break;

        case SCRIPT_STATE::DONE:
            break;
    }
}
