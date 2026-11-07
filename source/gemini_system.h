#ifndef gemini_system_h
#define gemini_system_h

#include "ai_root.h"

// The Google Gemini backend for ai_root. Only what is specific to Gemini's
// API lives here - see gemini_system.cpp. History, tools, consolidation and the
// head and tail of send() are all in ai_root.
//
// Setup: the API key is read from <OLLI_DIRECTORY>/gemini_key.txt (or the
// GEMINI_API_KEY environment variable) on every request - never from PROPS or
// git. PROPS.model is a Gemini model name (default gemini-3.5-flash-lite).
// PROPS.host/port/num_ctx/keep_alive_seconds/repeat_penalty are Ollama-only and
// are ignored here.
class gemini_system : public ai_root {
    public:
        gemini_system();

        std::unique_ptr<ai_root> make_sibling() const override;

        // No unload_model() override: a hosted API has nothing to unload.

    protected:
        void backend_generate(TOOL_WORKER_CLASS* tool_worker, COMMS& comms, const json& response_format, std::string& accumulated_content, std::string& accumulated_thinking) override;
};

#endif
