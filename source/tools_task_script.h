#ifndef tools_task_script_h
#define tools_task_script_h

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
class ollama_system;
class COMMS;
class TOOL_BASE;

// The states TOOL_TASK_RUNNER::handle_tool()'s while loop (tools.cpp) steps
// through to drive one .task script from GET_COMMAND to DONE.
enum class SCRIPT_STATE
{
    GET_COMMAND,    // pull the next line, classify it
    EXECUTE_COMMAND,// hand a line to the LLM
    WAIT_RESPONSE,  // drain the LLM's turn (incl. any tool calls) to completion
    WAIT_ENTER,     // [PAUSE] - pure local pause, no LLM involved
    WAIT_ASK,       // [ASK] - same wait, but the typed answer becomes the next input
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
void command_ask(SCRIPT_STATE& state, COMMS& instance_comms, const std::string& command, bool& keyboard_was_enabled);
void command_print(size_t& i, COMMS& instance_comms, const std::string& command);
void command_file_in(SCRIPT_STATE& state, std::string& current_input, COMMS& instance_comms, const std::string& command, const std::filesystem::path& files_dir, bool& keyboard_was_enabled);
void command_file_append(size_t& i, ollama_system& instance, const std::string& command, const std::filesystem::path& files_dir);
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
void command_plain(SCRIPT_STATE& state, std::string& current_input, const std::string& command);

// One function per SCRIPT_STATE (tools_task_script.cpp) - each takes only
// the references it actually touches, not a uniform signature, so what a
// given command can and can't reach is visible right here at a glance.
// Free functions, not TOOL_TASK_RUNNER methods, deliberately: none of them
// can reach chat/comms/tc_id/io_worker/task_manager/OLLI_DIRECTORY even by
// accident, only exactly what's passed in below.
void command_get_command(SCRIPT_STATE& state, size_t& i, std::string& current_input, const TASK_SIMPLE& found_task, ollama_system& instance, COMMS& instance_comms, const std::filesystem::path& files_dir, bool& keyboard_was_enabled);
void command_wait_enter(SCRIPT_STATE& state, size_t& i, COMMS& instance_comms, bool& keyboard_was_enabled);
void command_wait_ask(SCRIPT_STATE& state, std::string& current_input, COMMS& instance_comms, bool& keyboard_was_enabled);
void command_execute_command(SCRIPT_STATE& state, const std::string& current_input, ollama_system& instance, COMMS& instance_comms, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list);
void command_wait_response(SCRIPT_STATE& state, size_t& i, ollama_system& instance);

// Drives the state machine one tick - called once per iteration of
// TOOL_TASK_RUNNER::handle_tool()'s while loop (tools.cpp). Just dispatch -
// each state's actual logic lives in its own command_*() function above.
// keyboard_was_enabled is TOOL_TASK_RUNNER::handle_tool()'s own persistent
// local (declared alongside i/current_input/state), threaded through the
// same way - see command_pause()'s own comment for what it's for.
void advance_script_state(SCRIPT_STATE& state, size_t& i, std::string& current_input, const TASK_SIMPLE& found_task, ollama_system& instance, COMMS& instance_comms, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, const std::filesystem::path& files_dir, bool& keyboard_was_enabled);

#endif
