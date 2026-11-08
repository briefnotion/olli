#ifndef tools_task_script_h
#define tools_task_script_h

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "tools_helper.h" // TASK_SIMPLE

// Forward declarations only - same reasoning as tools.h's own identical
// list: everything below only ever takes these by reference, never needing
// a complete type in this header (the .cpp does, to actually call methods
// on them).
class ai_root;
class COMMS;
class TOOL_BASE;
class TOOL_WORKER_CLASS;

// The states TOOL_TASK_RUNNER::handle_tool()'s while loop (tools.cpp) steps
// through to drive one .task script from GET_COMMAND to DONE.
enum class SCRIPT_STATE
{
    GET_COMMAND,    // pull the next line, classify it
    EXECUTE_COMMAND,// hand a line to the LLM
    WAIT_RESPONSE,  // drain the LLM's turn (incl. any tool calls) to completion
    WAIT_ENTER,     // [PAUSE] - pure local pause, no LLM involved
    WAIT_ASK,       // [ASK] - same wait, but the typed answer becomes the next input

    // [WAIT_FOR_RESULT] - pause until the remote-tool call the previous
    // line triggered actually answers, instead of the default (a script's
    // held results/events all wait until the whole run is done - see
    // TASK_SIMPLE::delay_tool_returns, tools_helper.h). advance_script_
    // state()'s own case for this is a no-op - resolving it needs
    // TOOL_TASK_RUNNER::handle_tool()'s own held_results/held_events
    // (tools.cpp), which this file deliberately has no access to.
    WAIT_TOOL_RESULT,

    // [IF]<question> - EXECUTE_IF sends the question the way EXECUTE_COMMAND
    // sends a line, except asking for a yes/no/maybe answer
    // (YES_NO_MAYBE_FORMAT, ai_root.h). WAIT_IF_RESPONSE waits for it, then
    // jumps i to whichever branch matches the answer.
    EXECUTE_IF,
    WAIT_IF_RESPONSE,

    // [RUN:name] - run another task to completion, then carry on. Like
    // WAIT_TOOL_RESULT, advance_script_state()'s own case for this is a no-op:
    // starting a task needs TOOL_TASK_RUNNER::handle_tool() itself (tools.cpp),
    // which this file has no access to.
    RUN_TASK,

    DONE
};

// Confines a task-authored name to a single path segment - used both for a
// task's own files_dir (TASK_DIRECTORY or TASK_NAME, TOOL_TASK_RUNNER::
// handle_tool()) and for a [FILE_IN:.../FILE_APPEND:...] argument
// (command_get_command(), tools_task_script.cpp). '/' and '\\' stripped
// outright so the result can never create or escape into a subdirectory;
// spaces become underscores per request. Never applied to OLLI_DIRECTORY
// itself - that's program-controlled, not task-authored free text, so it's
// already trusted.
std::string sanitize_path_segment(const std::string& raw);

// One function per [MARKER] a GET_COMMAND line can start with
// (tools_task_script.cpp) - command_get_command() itself is just the
// prefix-matching dispatch, same relationship advance_script_state() has to
// the command_*() functions below it. Each takes only the command line's
// text plus whatever else it actually touches, not a uniform signature.
//
// keyboard_was_enabled (command_pause/command_ask/command_file_in, and the
// matching command_wait_enter/command_wait_ask below) is a save slot for
// ENABLE_KEYBOARD_INPUT, persistent across the many ticks a WAIT_ENTER/
// WAIT_ASK wait can span - same reason i/current_input/state themselves are
// threaded through as references rather than locals. Any command that drops
// into one of those two wait states inherently needs a human to actually
// press a key, so it silently saves whatever ENABLE_KEYBOARD_INPUT was,
// forces it on for the duration of the wait, then the matching
// command_wait_*() restores the saved value once the wait resolves - a
// script only has to manage this itself for stretches where NONE of these
// commands run, not around every single one of them.
void command_pause(SCRIPT_STATE& state, COMMS& instance_comms, bool& keyboard_was_enabled);
// [ASK]<text>, or [ASK:seconds]<text> / [ASK:seconds,sub_name]<text> - the
// second and third forms give up waiting after that many seconds. ask_deadline
// is where that time is kept between ticks (a plain local in TOOL_TASK_RUNNER::
// handle_tool(), tools.cpp, like i and call_stack); a default-constructed
// time_point means "no time limit". What a timeout does is command_ask_timed_out()
// below.
void command_ask(SCRIPT_STATE& state, COMMS& instance_comms, const std::string& command, bool& keyboard_was_enabled, std::chrono::steady_clock::time_point& ask_deadline);

// [WAIT_FOR_RESULT] - just the state transition; no keyboard_was_enabled
// save/restore needed (unlike command_pause/command_ask above), nothing
// here waits on a human. See SCRIPT_STATE::WAIT_TOOL_RESULT's own comment
// for why resolving the wait itself happens outside this file entirely.
void command_wait_for_result(SCRIPT_STATE& state);
void command_print(size_t& i, COMMS& instance_comms, const std::string& command);

// [RUN:task name] - just the state transition (and a check that a name was
// given); the loop in TOOL_TASK_RUNNER::handle_tool() (tools.cpp) does the
// rest. The named task runs on its own, to the end, and when it is done this
// script goes on with the next line. A task can [RUN] other tasks, or itself;
// handle_tool() stops the script with an error when that goes too deep.
void command_run(SCRIPT_STATE& state, COMMS& instance_comms, const std::string& command);
void command_file_in(SCRIPT_STATE& state, std::string& current_input, COMMS& instance_comms, const std::string& command, const std::filesystem::path& files_dir, bool& keyboard_was_enabled);
void command_file_append(size_t& i, ai_root& instance, const std::string& command, const std::filesystem::path& files_dir);
// [KEYBOARD_INPUT:on/off] / [TTS_OUTPUT:on/off] - the general boolean-toggle
// convention for this scripting language: marker names the subject, value
// is "on"/"off" (not true/false or 1/0 - reads as plain English). Instant,
// like [PRINT] - no LLM call, no waiting, advances i itself. Sets on
// instance_comms specifically because a running script's while loop
// (TOOL_TASK_RUNNER::handle_tool()) calls io_worker.exchange(instance_comms,
// ...) every tick while it's running, not the main chat's own comms - so
// this is what actually reaches the real screen/keyboard for the script's
// duration.
void command_keyboard_input(size_t& i, COMMS& instance_comms, const std::string& command);
void command_tts_output(size_t& i, COMMS& instance_comms, const std::string& command);
// [ECHO_INPUT:on/off] - same shape as the two above. Off hides the
// "INPUT: <line>" echo (and the dashed line above it) of every line sent to the
// model, [IF] questions included, so the player sees only the model's reply.
// See COMMS::ENABLE_ECHO_INPUT (comms.h).
void command_echo_input(size_t& i, COMMS& instance_comms, const std::string& command);
void command_plain(SCRIPT_STATE& state, std::string& current_input, const std::string& command);

// [END] - finish the script right here. Works anywhere, including inside an
// [IF] branch, so an ending doesn't have to be the last thing in its block.
// The script's normal wrap-up (held tool results, the report to the main chat)
// still runs afterwards, exactly as if it had run out of lines.
void command_end(SCRIPT_STATE& state);

// [SUB:name] ... [ENDSUB], [GOSUB:name], [RETURN] - subroutines. A sub is a
// block of lines that only runs when something calls it; when the main flow
// falls into a [SUB] it skips straight to after the [ENDSUB]. [GOSUB] jumps to
// the line after the matching [SUB] and remembers where to come back to;
// [ENDSUB] (or an early [RETURN]) goes back. Subs can call other subs, and
// themselves (that is how a room asks its question again); they can't take
// arguments, can't be defined inside another sub, and a call chain deeper than
// MAX_GOSUB_DEPTH (tools_task_script.cpp) stops the script with an error.
//
// call_stack holds, for each call still open, the line index to return to. It
// is a plain local in TOOL_TASK_RUNNER::handle_tool() (tools.cpp), threaded
// through advance_script_state() the same way i and current_input are - a
// static here would be shared by two scripts running at once.
void command_sub(SCRIPT_STATE& state, size_t& i, const TASK_SIMPLE& found_task, const std::vector<size_t>& call_stack, COMMS& instance_comms);
void command_gosub(SCRIPT_STATE& state, size_t& i, const TASK_SIMPLE& found_task, std::vector<size_t>& call_stack, COMMS& instance_comms, const std::string& command);
void command_return(SCRIPT_STATE& state, size_t& i, std::vector<size_t>& call_stack, COMMS& instance_comms, const std::string& command);

// [IF]<question> ... [ELSE] ... [MAYBE] ... [ENDIF] - a three-way branch on
// the model's yes/no/maybe answer. The lines after [IF] are the yes branch;
// [ELSE] is no and [MAYBE] is maybe; both are optional and can come in either
// order. A branch that isn't there means "skip to [ENDIF]". [IF]s can nest.
//
// command_if() checks there's an [ENDIF] for this [IF] before asking
// anything (no point spending an LLM call on a script that can't finish),
// then hands the question to EXECUTE_IF. command_skip_to_endif() is what
// [ELSE]/[MAYBE] do when a script reaches them by running the branch above
// them: that branch is done, so skip the rest of the block. [ENDIF] itself
// does nothing but move on.
void command_if(SCRIPT_STATE& state, size_t& i, std::string& current_input, const TASK_SIMPLE& found_task, COMMS& instance_comms, const std::string& command);
void command_skip_to_endif(SCRIPT_STATE& state, size_t& i, const TASK_SIMPLE& found_task, COMMS& instance_comms);

// An [IF] placed straight after an [ASK] (or after a [FILE_IN] that fell back
// to asking) judges what the player typed, instead of what the narrator said
// about it. So in that case WAIT_ASK does not send the typed line to the model
// as its own turn: this puts the player's words into the model's history as a
// message, without asking for a reply, and starts the [IF] question right
// behind it. The narrator therefore knows what the player said (the words
// stay in history; only the question and its one-word answer are taken back
// out afterwards, command_wait_if_response()) but has not reacted to it yet -
// a plain line in the chosen branch ("React to what the player just did.")
// decides where it does. Called by advance_script_state()'s WAIT_ASK case, only
// when the next line is an [IF]; i is still on the [ASK] line.
void command_wait_ask_for_if(SCRIPT_STATE& state, size_t& i, std::string& current_input, const TASK_SIMPLE& found_task, ai_root& instance, COMMS& instance_comms, bool& keyboard_was_enabled);

// Called every tick while WAIT_ASK is waiting (advance_script_state()). Does
// nothing unless the [ASK:...] line has a time limit, no answer has come in,
// and the limit has passed. Then the wait is over: with no sub name the script
// ends; with one, it is called like [GOSUB:name], and when the sub returns the
// script carries on after the whole [ASK] block - past a following
// [IF]...[ENDIF] too, since there is no answer for it to judge.
void command_ask_timed_out(SCRIPT_STATE& state, size_t& i, const TASK_SIMPLE& found_task, COMMS& instance_comms, bool& keyboard_was_enabled, std::vector<size_t>& call_stack, std::chrono::steady_clock::time_point& ask_deadline);

// One function per SCRIPT_STATE (tools_task_script.cpp) - each takes only
// the references it actually touches, not a uniform signature, so what a
// given command can and can't reach is visible right here at a glance.
// Free functions, not TOOL_TASK_RUNNER methods, deliberately: none of them
// can reach chat/comms/tc_id/io_worker/task_manager/OLLI_DIRECTORY even by
// accident, only exactly what's passed in below.
void command_get_command(SCRIPT_STATE& state, size_t& i, std::string& current_input, const TASK_SIMPLE& found_task, ai_root& instance, COMMS& instance_comms, const std::filesystem::path& files_dir, bool& keyboard_was_enabled, std::vector<size_t>& call_stack, std::chrono::steady_clock::time_point& ask_deadline);
void command_wait_enter(SCRIPT_STATE& state, size_t& i, COMMS& instance_comms, bool& keyboard_was_enabled);
void command_wait_ask(SCRIPT_STATE& state, std::string& current_input, COMMS& instance_comms, bool& keyboard_was_enabled);
void command_execute_command(SCRIPT_STATE& state, const std::string& current_input, ai_root& instance, COMMS& instance_comms, TOOL_WORKER_CLASS* tool_worker);
void command_wait_response(SCRIPT_STATE& state, size_t& i, ai_root& instance);
// WAIT_IF_RESPONSE - when the answer arrives, show it and set i to the start
// of the matching branch.
void command_wait_if_response(SCRIPT_STATE& state, size_t& i, ai_root& instance, const TASK_SIMPLE& found_task, COMMS& instance_comms);

// Drives the state machine one tick - called once per iteration of
// TOOL_TASK_RUNNER::handle_tool()'s while loop (tools.cpp). Just dispatch -
// each state's actual logic lives in its own command_*() function above.
// keyboard_was_enabled is TOOL_TASK_RUNNER::handle_tool()'s own persistent
// local (declared alongside i/current_input/state), threaded through the
// same way - see command_pause()'s own comment for what it's for.
// tool_worker is the caller's own (nullable) - only EXECUTE_COMMAND
// actually needs it (forwarded to command_execute_command()'s own send()
// call), but every state gets it passed down for a uniform signature.
void advance_script_state(SCRIPT_STATE& state, size_t& i, std::string& current_input, const TASK_SIMPLE& found_task, ai_root& instance, COMMS& instance_comms, TOOL_WORKER_CLASS* tool_worker, const std::filesystem::path& files_dir, bool& keyboard_was_enabled, std::vector<size_t>& call_stack, std::chrono::steady_clock::time_point& ask_deadline);

#endif
