#ifndef tools_helper_h
#define tools_helper_h

#include <string>
#include <chrono>
#include <vector>

class TIMER_SIMPLE 
{
    public:
        explicit TIMER_SIMPLE(double seconds = 0.0, std::string reminder = "") 
            : m_duration(seconds), m_reminder(reminder), m_running(false) {}
        void start();
        bool isFinished() const;
        double getRemainingTime() const;
        std::string getReminder() const;

    private:
        std::chrono::duration<double> m_duration;
        std::chrono::steady_clock::time_point m_startTime;
        std::string m_reminder; 
        bool m_running;
};

struct TASK_SIMPLE
{
    public:
        std::string TASK_PHRASE = "";
        std::vector<std::string> COMMANDS;
        void clear();
};

class TASK_SIMPLE_MANAGER
{
    public:
        std::vector<TASK_SIMPLE> TASK_LIST;

        TASK_SIMPLE_MANAGER();

        void load_all_task();
};

#endif