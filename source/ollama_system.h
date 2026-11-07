#ifndef ollama_system_h
#define ollama_system_h

#include "ai_root.h"

// The Ollama backend for ai_root. Only what is specific to an Ollama server
// lives here - see ollama_system.cpp. History, tools, consolidation and the
// head and tail of send() are all in ai_root.
class ollama_system : public ai_root {
    public:
        ollama_system() = default;

        std::unique_ptr<ai_root> make_sibling() const override;

        // Sends keep_alive: 0 to /api/generate so Ollama frees PROPS.model
        // now instead of after its own idle timeout.
        void unload_model() override;

    protected:
        void backend_generate(TOOL_WORKER_CLASS* tool_worker, COMMS& comms, const json& response_format, std::string& accumulated_content, std::string& accumulated_thinking) override;
};

#endif
