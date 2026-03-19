// ***************************************************************************************
// *
// *    Core       | Everything within this document is proprietary to Core Dynamics.
// *    Dynamics   | Any unauthorized duplication will be subject to prosecution.
// *
// *    Department : (R+D)^2                        Name: fled_time.h
// *       Sub Dept: Programming
// *    Location ID: 856-45B
// *                                                      (c) 2856 - 2858 Core Dynamics
// ***************************************************************************************
// *
// *  PROJECTID: gi6$b*E>*q%;    Revision: 00000000.01A
// *  TEST CODE:                 QACODE: A565              CENSORCODE: EQK6}Lc`:Eg>
// *
// ***************************************************************************************

#ifndef FLED_TIME_H
#define FLED_TIME_H

// Standard Header Files
#include <iostream>
#include <chrono>
#include <ctime>
#include <cmath>
#include <unistd.h>

//#include "helper.h"

using namespace std;

// ---------------------------------------------------------------------------------------
class STAT_DATA_DOUBLE
// Provide minor min max stats over time of resets.
// If real stats ever necessary, then weighted average 
//  routine needed.
{
  private:

  bool set = true;

  double DATA  = 0;
  double MIN   = 0;
  double MAX   = 0;

  public:
  void set_data(double data);

  double get_data();

  double get_min();

  double get_max();

  void reset_minmax();
};
// ---------------------------------------------------------------------------------------

class EFFICIANTCY_TIMER
// Measures time passed between calls. 
{
  private:
  double LAST_ASKED_TIME = 0;
  double TIMER_STARTED   = 0;

  public:

  void start_timer(double dblCurrent_Time);
  // Start the timer (stopwatch) by setting its the stopwatch time.
  //  The timer is a simple and can be considered always active. 

  double elapsed_timer_time(double dblCurrent_Time);
  //  Returns the amount of time passed since the reset. 

  double elapsed_time(double dblCurrent_Time);
  // Measures the amount of time elasped since the previous time the function was 
  //  called, then returns the value, then resets for next time. 

  double simple_elapsed_time(double dblCurrent_Time);
  // Only returns the calc of current time - start timer time.
};

// -------------------------------------------------------------------------------------
// Time Variable

// Stores a time that can be retreived in components, such as minutes hours and so on. 
//  This routine is not complete. Different results depending on put call.

class FLED_TIME_VAR
{
  private:
  // time starts at jan 1 1970
  double SECONDS = 0;
  double MICRO_SECONDS = 0;   // 1*10^-6 Seconds //!!!

  time_t THE_TIME;
  tm *PTM;
  bool TIME_UPDATED = false;

  void update_time();

  public:
  void put_seconds(double Seconds);
  void put_deciseconds(double Deciseconds);
  void put_miliseconds(double Miliseconds);

  double get_seconds();

  int get_deciseconds();

  int get_miliseconds();

  int get_year();

  int get_month();

  int get_day();

  int get_hour();

  int get_minute();
  
  int get_second();
};


// -------------------------------------------------------------------------------------
// Keeps track of timing variables
//  1ms accuracy is good enough for anything outside of the sciences.
//  However, watching my blinkers get out of sync after 20 seconds threads 
//  on the side of irritating.

class FLED_TIME
{
  private:
  std::chrono::time_point<std::chrono::steady_clock> TIME_START;

  EFFICIANTCY_TIMER EFFIC_TIMER;           // Diagnostic timer to measure cycle times.

  double ERROR_TOLERANCE = 1.0;
  double ERROR = 0.0;
  double OLD_NOW_TIME = 0.0;
  bool ERROR_EXIST = false;

  double CURRENT_FRAME_TIME = 0.0;

  bool test = false;

  // How long sleep will occure
  double SLEEP_TIME = 1000;

  public:
  STAT_DATA_DOUBLE COMPUTETIME;   // Loop time spent while only proceessing.
  STAT_DATA_DOUBLE CYCLETIME;     // Amount of time to complete an entire cycle.
  STAT_DATA_DOUBLE PREVSLEEPTIME; // Stored value returned on prev sleep cycle.

  public:
  void request_ready_time(double Ready_Time);          // Set sleep time to loweset
  void request_ready_time(double Ready_Time, char ID); // For debugging. will print id if 
                                                              // id if sleep time requested is <=0
 
  double error();

  void clear_error();

  double now();

  void create();

  bool setframetime();

  double tmeFrameElapse();

  double current_frame_time();

  void sleep_till_next_frame();
};

// ---------------------------------------------------------------------------------------
class TIMED_IS_READY
// Class to manage conditions of when something needs to be ran.
{
  private:
  double TRIGGERED_TIME  = 0;  //  Most recent time the ready was activated
  double LAST_ASKED_TIME = 0;  //  Most recent time the variable was asked if was ready.
  double READY_TIME      = 0;  //  Calculated time of when variable will be ready.
  double INTREVAL        = 0;  //  Time in miliseconds between ready.

  public:

  bool is_set();

  void set(double current_time, double delay);

  void set(double delay);

  double get_ready_time();

  bool is_ready(double current_time);
  
  bool is_ready_no_reset(double current_time);
  
  void set_earliest_ready_time(double current_time);
};

// -------------------------------------------------------------------------------------
// Functions

#endif