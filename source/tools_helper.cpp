#ifndef tools_helper_cpp
#define tools_helper_cpp

#include "tools_helper.h"

// TIMER_SIMPLE used to live here - moved to tools/clock/clock.cpp along
// with TOOL_TIMER itself (see the comment where that class used to be in
// source/tools.cpp).

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

    tmp_task.clear();
    tmp_task.TASK_PHRASE = "run system test";
    tmp_task.TASK_PURPOSE = "This is a series of a few simple questions to check responses.";
    tmp_task.TASK_DIRECTORY = "system_test";
    tmp_task.COMMANDS.push_back("Set a timer for 30 seconds and when the timer goes off, blink any light.");
    tmp_task.COMMANDS.push_back("turn off all the lights.");
    tmp_task.COMMANDS.push_back("show the numbers 1 through 10");
    tmp_task.COMMANDS.push_back("what time is it?");
    tmp_task.COMMANDS.push_back("what is the opposite of up?");
    tmp_task.COMMANDS.push_back("what is 2 + 2?");
    tmp_task.COMMANDS.push_back("Ask the user for the name of a dog.");
    tmp_task.COMMANDS.push_back("[[ASK]]What is a name for a dog?");
    tmp_task.COMMANDS.push_back("what was the previous word?");
    tmp_task.COMMANDS.push_back("what is the weather like in New York City right now?");
    tmp_task.COMMANDS.push_back("how many eggs are in a dozon eggs.");
    tmp_task.COMMANDS.push_back("turn all the lights back on.");
    tmp_task.COMMANDS.push_back("[[ENTER TO CONTINUE]]wait");
    tmp_task.COMMANDS.push_back("Announce the system test is complete.");
    TASK_LIST.push_back(tmp_task);

    tmp_task.clear();
    tmp_task.TASK_PHRASE = "run process resume";
    tmp_task.TASK_PURPOSE = "You are a professional career consultant and ghostwriter. "
                            "Your objective is to execute a linear sequence of commands to "
                            "analyze a resume against a job description. "
                            "CRITICAL: Do not skip steps or ask for clarification. "
                            "When generating text (Evaluation/Cover Letter), adopt a first-person "
                            "perspective. Use a 'Human-Authentic' style: professional and "
                            "competent, but avoid the overly polished, 'perfect' cadence of "
                            "typical AI. Use natural phrasing and varied sentence structures "
                            "to ensure the output feels written by a person, not a machine.";
    
    tmp_task.TASK_DIRECTORY = "resume_process";

    tmp_task.COMMANDS.push_back("Go into thinking mode.");

    tmp_task.COMMANDS.push_back("Ask the user for the resume file.");
    tmp_task.COMMANDS.push_back("[[ASK]]");
    
    tmp_task.COMMANDS.push_back("Ask the user for the job description.");
    tmp_task.COMMANDS.push_back("[[ASK]]");

    tmp_task.COMMANDS.push_back("In a first person point of view, speaking as if I was the "
                                "resume owner, generate a concise 1-paragraph evaluation "
                                "explaining how well the "
                                "resume matches the job description. "
                                "Highlight the strongest points of alignment, reference "
                                "specific skills or achievements, and keep the tone "
                                "professional and neutral. But also, I dont want it to be "
                                "perfect. Add just a little slop to make it feel more human.");
    tmp_task.COMMANDS.push_back("[[ENTER TO CONTINUE]]");
    
    tmp_task.COMMANDS.push_back("Using the applicant's resume and the job description, generate "
                                "a short professional cover letter (3-4 paragraphs) explaining "
                                "why the applicant is a strong fit for the role. Keep the tone "
                                "confident but not overly formal, and reference specific "
                                "experience from the resume. But also, I dont want it to be "
                                "perfect. Add just a little slop to make it feel more human.");
    tmp_task.COMMANDS.push_back("[[ENTER TO CONTINUE]]");
    
    TASK_LIST.push_back(tmp_task);
}

// HUE_SCENE's to_json/from_json and every HUE_LIGHT_CLASS method used to
// live here - moved to tools/hue/hue.cpp along with the rest of Hue
// support (see the comment where HUE_LIGHT_CLASS used to be declared in
// tools_helper.h, and tools/PROTOCOL.md).

// ----

#endif