#include "ai_factory.h"
#include "ollama_system.h"
#include "gemini_system.h"

#include <iostream>

std::unique_ptr<ai_root> make_ai_system(const std::string& backend)
{
    if (backend == "gemini")
        return std::make_unique<gemini_system>();

    if (backend != "ollama")
        std::cerr << "[Warning] Unknown ai_backend \"" << backend << "\" in settings.json (expected \"ollama\" or \"gemini\") - using ollama." << std::endl;

    return std::make_unique<ollama_system>();
}
