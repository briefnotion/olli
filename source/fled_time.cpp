// ***************************************************************************************
// *
// *    Core       | Everything within this document is proprietary to Core Dynamics.
// *    Dynamics   | Any unauthorized duplication will be subject to prosecution.
// *
// *    Department : (R+D)^2                        Name: fled_time.cpp
// *       Sub Dept: Programming
// *    Location ID: 856-45B
// *                                                      (c) 2856 - 2858 Core Dynamics
// ***************************************************************************************
// *
// *  PROJECTID: gi6$b*E>*q%;    Revision: 00000000.01A
// *  TEST CODE:                 QACODE: A565              CENSORCODE: EQK6}Lc`:Eg>
// *
// ***************************************************************************************

#ifndef FLED_TIME_CPP
#define FLED_TIME_CPP

#include "fled_time.h"

using namespace std;

// ---------------------------------------------------------------------------------------


void STAT_DATA_DOUBLE::set_data(double data)
// Provide minor min max stats over time of resets.
// I real stats ever necessary, then weighted average 
//  routine needed.
{
  DATA = data;
  
  if (set == false)
  {
    set = true;
    MIN = DATA;
    MAX = DATA;
  }
  else if (data < MIN)
  {
    MIN = data;
  }
  else if (data > MAX)
  {
    MAX = data;
  }
}

double STAT_DATA_DOUBLE::get_data()
{
  return DATA;
}

  double STAT_DATA_DOUBLE::get_min()
{
  return MIN;
}

  double STAT_DATA_DOUBLE::get_max()
{
  return MAX;
}

void STAT_DATA_DOUBLE::reset_minmax()
{
  set = false;
  MIN = DATA;
  MAX = DATA;
}

// ---------------------------------------------------------------------------------------

void EFFICIANTCY_TIMER::start_timer(double dblCurrent_Time)
{
  TIMER_STARTED = dblCurrent_Time;
}

double EFFICIANTCY_TIMER::elapsed_timer_time(double dblCurrent_Time)
{
  return dblCurrent_Time - TIMER_STARTED;
}

double EFFICIANTCY_TIMER::elapsed_time(double dblCurrent_Time)
{
  double time_elapsed = dblCurrent_Time - LAST_ASKED_TIME;
  LAST_ASKED_TIME = dblCurrent_Time;
  return time_elapsed;
}

double EFFICIANTCY_TIMER::simple_elapsed_time(double dblCurrent_Time)
{
  return dblCurrent_Time - TIMER_STARTED;
}

// -------------------------------------------------------------------------------------
// Time Variable

void FLED_TIME_VAR::update_time()
{
  THE_TIME = static_cast<time_t>(SECONDS);
  PTM = localtime(&THE_TIME);
  TIME_UPDATED = true;
}

void FLED_TIME_VAR::put_seconds(double Seconds)
{
  SECONDS = Seconds;
  TIME_UPDATED = false;
}

void FLED_TIME_VAR::put_deciseconds(double Deciseconds)
{
  MICRO_SECONDS = Deciseconds *100000;
  TIME_UPDATED = false;
}

void FLED_TIME_VAR::put_miliseconds(double Miliseconds)
{
  MICRO_SECONDS = Miliseconds *1000;
  TIME_UPDATED = false;
}

double FLED_TIME_VAR::get_seconds()
{
  return SECONDS;
}

int FLED_TIME_VAR::get_deciseconds()
{
  return static_cast<int>(std::round(MICRO_SECONDS / 100000.0));
}

int FLED_TIME_VAR::get_miliseconds()
{
  return static_cast<int>(std::round(MICRO_SECONDS / 1000.0));
}

int FLED_TIME_VAR::get_year()
{

  if (TIME_UPDATED == false)
  {
    update_time();
  }

  return (1900 + PTM->tm_year);
}

int FLED_TIME_VAR::get_month()
{

  if (TIME_UPDATED == false)
  {
    update_time();
  }

  return (PTM->tm_mon +1);
}

int FLED_TIME_VAR::get_day()
{
  if (TIME_UPDATED == false)
  {
    update_time();
  }

  return (PTM->tm_mday);
}

int FLED_TIME_VAR::get_hour()
{
  if (TIME_UPDATED == false)
  {
    update_time();
  }

  return (PTM->tm_hour);
}

int FLED_TIME_VAR::get_minute()
{
  if (TIME_UPDATED == false)
  {
    update_time();
  }

  return (PTM->tm_min);
}

int FLED_TIME_VAR::get_second()
{
  if (TIME_UPDATED == false)
  {
    update_time();
  }

  return (PTM->tm_sec);
}

// -------------------------------------------------------------------------------------

void FLED_TIME::request_ready_time(double Ready_Time)
{
  double requested_sleep_time = Ready_Time - now();

  if (requested_sleep_time > 0)
  {
    if (requested_sleep_time < SLEEP_TIME)
    {
      SLEEP_TIME = requested_sleep_time;
    }
  }
  else
  {
    SLEEP_TIME = 0;
  }
}

void FLED_TIME::request_ready_time(double Ready_Time, char ID)
{
  double requested_sleep_time = Ready_Time - now();
  if (requested_sleep_time <= 0)
  {
    cout << ID << flush;
  }
  request_ready_time(Ready_Time);
}

double FLED_TIME::error()
{
  return ERROR;
}

void FLED_TIME::clear_error()
{
  ERROR_EXIST = false;
}

double FLED_TIME::now()
{
  // Returns now time in milliseconds.
  // Should be double.
  //std::chrono::time_point<std::chrono::system_clock> tmeNow = std::chrono::system_clock::now();
  //std::chrono::duration<double>  dur = tmeNow - TIME_START;
  auto tmeNow = std::chrono::steady_clock::now();
  std::chrono::duration<double> dur = tmeNow - TIME_START;

  double nowtime = dur.count();

  // This error check routine and offset by error should prevent the program from locking up 
  //  while waiting on timing triggers, if the internet decides the system clock needs to change
  //  its time by a significant amount.  The problem is that there are things, such as the graphs,
  //  that, somehow, aren't fooled by this little hack.  Not sure why yet.

  // Check for error
  if (std::abs(nowtime - OLD_NOW_TIME) > ERROR_TOLERANCE)
  {
    ERROR = nowtime - OLD_NOW_TIME;
    ERROR_EXIST = true;
  }
  
  OLD_NOW_TIME = nowtime;
  nowtime = (nowtime - ERROR) * 1000.0; // convert to milliseconds

  return nowtime;
}

void FLED_TIME::create()
{
  // Initialize as Start of Program Time.
  TIME_START = std::chrono::steady_clock::now();

  EFFIC_TIMER.start_timer(now());
}

bool FLED_TIME::setframetime()
{
  CURRENT_FRAME_TIME = now();

  return ERROR_EXIST;

  // Error can be reported as follows in true loop if necessary.
  //cout << "Frame Time: Inteupted " << endl;
  //cout << "  Current Time Frame: " << to_string(sdSystem.PROGRAM_TIME.current_frame_time()) << endl;
  //cout << "          Error (ms): " << to_string(sdSystem.PROGRAM_TIME.error()) << endl;
}

double FLED_TIME::tmeFrameElapse()
{
  // Returns, in milliseconds, the amount of time passed since frame time.
  double elapsed = static_cast<double>(now() - CURRENT_FRAME_TIME);

  return elapsed;
}

double FLED_TIME::current_frame_time()
{
  return CURRENT_FRAME_TIME;
}

void FLED_TIME::sleep_till_next_frame()
{
  // Measure how much time has passed since the previous time the program was at 
  //  this point and store that value to be displayed in diag.
  CYCLETIME.set_data(EFFIC_TIMER.elapsed_time(now()));

  // Reset the the compute timer (stopwatch) and store the value before the program sleeps. 
  COMPUTETIME.set_data(EFFIC_TIMER.elapsed_timer_time(now()));

  // Store Sleep time, probably doesnt need to be before, but here it is.
  PREVSLEEPTIME.set_data(SLEEP_TIME);

  if (SLEEP_TIME > 1000 || SLEEP_TIME < 0)
  {
    SLEEP_TIME = 1000;
  }

  // Sleep until the next frame.
  if (SLEEP_TIME > 0 && SLEEP_TIME < 1000 )
  {
    usleep(static_cast<useconds_t>(1000.0 * SLEEP_TIME));
  }

  // Reset to 1 second
  SLEEP_TIME = 1000;

  // Start the the compute timer (stopwatch) before the program as the program wakes to measure the amount of 
  //  time the compute cycle is.
  EFFIC_TIMER.start_timer(now());
}
  
// ---------------------------------------------------------------------------------------

bool TIMED_IS_READY::is_set()
{
  if (TRIGGERED_TIME == 0)
  {
    return false;
  }
  else
  {
    return true;
  }
}

void TIMED_IS_READY::set(double current_time, double delay)
// Prep the TIMED_IS_READY varable
// delay is measured in ms.
{
  TRIGGERED_TIME  = current_time;
  INTREVAL        = delay;
  READY_TIME = current_time + INTREVAL;
}

void TIMED_IS_READY::set(double delay)
// Prep the TIMED_IS_READY varable
//  If current time isn't available
// delay is measured in ms.
{
  TRIGGERED_TIME  = 0;
  INTREVAL        = delay;
}

double TIMED_IS_READY::get_ready_time()
// Return the time value of when the variable will be ready. 
{
  return READY_TIME;
}

bool TIMED_IS_READY::is_ready(double current_time)
{
  // Check to see if enough time has passed.
  //  Returns true if interval time has passed.
  //    Resets timer if returned true.
  //  Returns false if time has not elapsed.
  //    Stores last asked time.
  if(current_time >= READY_TIME)
  {
    TRIGGERED_TIME = current_time;
    return true;
  }
  else
  {
    LAST_ASKED_TIME = current_time;
    return false;
  }
}

bool TIMED_IS_READY::is_ready_no_reset(double current_time)
{
  // Check to see if enough time has passed.
  //  Returns true if interval time has passed.
  //    Does not resets timer if returned true.
  //  Returns false if time has not elapsed.
  //    Stores last asked time.
  if(current_time >= READY_TIME)
  {
    LAST_ASKED_TIME = current_time;
    return true;
  }
  else
  {
    LAST_ASKED_TIME = current_time;
    return false;
  }
}

void TIMED_IS_READY::set_earliest_ready_time(double current_time)
// Manually set the ready time of the variable. 
// Does not change time if time to set is later than the already set time.
{
  if (current_time < READY_TIME)
  {
    READY_TIME = current_time;
  }
}

// -------------------------------------------------------------------------------------
// Functions


#endif