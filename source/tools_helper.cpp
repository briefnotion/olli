#ifndef tools_helper_cpp
#define tools_helper_cpp

#include "tools_helper.h"

void TIMER_SIMPLE::start() {
    m_startTime = std::chrono::steady_clock::now();
    m_running = true;
}

bool TIMER_SIMPLE::isFinished() const {
    if (!m_running) return false;
    auto now = std::chrono::steady_clock::now();
    return (now - m_startTime) >= m_duration;
}

double TIMER_SIMPLE::getRemainingTime() const {
    if (!m_running) return 0.0;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = now - m_startTime;
    
    if (elapsed >= m_duration) return 0.0;
    
    std::chrono::duration<double> remaining = m_duration - elapsed;
    return remaining.count();
}

std::string TIMER_SIMPLE::getReminder() const 
{ 
    return m_reminder; 
}

// ----

void TASK_SIMPLE::clear()
{
    TASK_PHRASE = "";
    COMMANDS.clear();
}

// ----

TASK_SIMPLE_MANAGER::TASK_SIMPLE_MANAGER()
{
    load_all_task();
}

void TASK_SIMPLE_MANAGER::load_all_task()
{
    TASK_SIMPLE tmp_task;

    tmp_task.TASK_PHRASE = "I'm home";
    tmp_task.COMMANDS.push_back("Turn on the computer light.");
    tmp_task.COMMANDS.push_back("Turn on the sofa light.");
    tmp_task.COMMANDS.push_back("Turn on the sink light.");
    tmp_task.COMMANDS.push_back("Turn on the bathroom light.");
    tmp_task.COMMANDS.push_back("Turn on the patio light.");
    TASK_LIST.push_back(tmp_task);

    tmp_task.clear();
    tmp_task.TASK_PHRASE = "I'm leaving";
    tmp_task.COMMANDS.push_back("Turn off the computer light.");
    tmp_task.COMMANDS.push_back("Turn off the sofa light.");
    tmp_task.COMMANDS.push_back("Turn off the sink light.");
    tmp_task.COMMANDS.push_back("Turn on the bathroom light.");
    tmp_task.COMMANDS.push_back("Turn on the patio light.");
    TASK_LIST.push_back(tmp_task);

    tmp_task.clear();
    tmp_task.TASK_PHRASE = "run diagnostic";
    tmp_task.COMMANDS.push_back("show the numbers 1 through 10");
    tmp_task.COMMANDS.push_back("what is the opposite of up?");
    tmp_task.COMMANDS.push_back("what is 2 + 2?");
    tmp_task.COMMANDS.push_back("how many eggs are in a dozon eggs.");
    TASK_LIST.push_back(tmp_task);
}

#endif