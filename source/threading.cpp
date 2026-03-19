#ifndef THREADING_CPP
#define THREADING_CPP

#include "threading.h"

using namespace std;

// ------------------------------------------------------------------------- //

bool THREADING_INFO::is_running()
{
  return IS_RUNNING;
}

void THREADING_INFO::create(int Time_in_FPS)
{
  THREAD_TIMER.set( 1000 / Time_in_FPS );
}

double THREADING_INFO::get_ready_time()
{
  return THREAD_TIMER.get_ready_time();
}

void THREADING_INFO::check_for_completition()
{
  if(IS_RUNNING == true)
  // Check to see if render thread was started before checking the completion status.
  {
    if(RENDER_THREAD.wait_for(0ms) == future_status::ready)
    // Check to verify the thread is complete before allowing the render to start again. 
    {
      //cout << "Thread closed" << endl;
      IS_RUNNING = false;
    }
    else
    {
      //cout << "Thread still open" << endl;
    }
  }
}

bool THREADING_INFO::check_to_run_routine_on_thread(double Time_Frame)
{
  bool ret_ready = false;

  //  Make sure this is the first check or the system will never sleep when running.
  if (THREAD_TIMER.is_ready(Time_Frame) == true)
  {
    if (IS_RUNNING == false)
    {
      ret_ready = true;
    }
  }

  return ret_ready;
}

void THREADING_INFO::start_render_thread(std::function<void()> Function)
{
  RENDER_THREAD = async(launch::async, Function);
  IS_RUNNING = true;
}

void THREADING_INFO::wait_for_thread_to_finish(string Name_Or_Description)
{
  if(IS_RUNNING == true)
  // Check to see if render thread was started before checking the completion status.
  {
    cout  << "Closing open threads ... (" << Name_Or_Description << ")" << endl;

    while(RENDER_THREAD.wait_for(10ms) != future_status::ready)
    {
      //cout << "Waiting for Render Thread to finish" << endl;
    }

    cout << "  Thread Closed"  << endl;

    IS_RUNNING = false;
  }
  else
  {
    cout << "No open threads to close ... (" << Name_Or_Description << ")" << endl;
  }
}


// ------------------------------------------------------------------------- //

#endif //THREADING_CPP