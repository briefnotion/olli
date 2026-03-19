#ifndef THREADING_H
#define THREADING_H

#include <future>
#include <iostream>

#include "fled_time.h"

using namespace std;

// ------------------------------------------------------------------------- //

//  Class to manage threading-related information.
class THREADING_INFO
{
  private:

  // Timer to track the thread's readiness state.
  TIMED_IS_READY  THREAD_TIMER;

  // String buffer to contain the screen buffer. Redundant; may be consolidated with other variables.
  string RAW_STRING_BUFFER = "";

  // Flag indicating whether the threading process is running.
  bool IS_RUNNING = false;

  // Future object representing the rendering thread's completion state.
  future<void> RENDER_THREAD; 

  public:

  //Returns true if thread is currently running a procedure
  bool is_running();

  // Initializes the threading process with a specified frame rate (FPS).
  // @param Time_in_FPS The desired FPS for the threading process.
  void create(int Time_in_FPS);

  // Returns the ready time of the thread, in milliseconds.
  // @return The ready time of the thread.
  double get_ready_time();

  // Checks whether the rendering thread has completed its execution.
  void check_for_completition();

  // Starts a new render thread with the specified function as its execution target.
  // @param Function The function to be executed by the render thread.
  void start_render_thread(function<void()> Function);

  // Checks whether it's time to run the routine on the thread, based on a specified time frame.
  // @param Time_Frame The time frame (in milliseconds) within which to check for running the routine.
  // @return A boolean indicating whether it's time to run the routine on the thread.
  bool check_to_run_routine_on_thread(double Time_Frame);

  // Waits for the rendering thread to finish its execution.
  // Call at program closing.
  void wait_for_thread_to_finish(string Name_Or_Description);
};

// ------------------------------------------------------------------------- //

#endif // THREADING_H