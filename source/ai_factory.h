#ifndef ai_factory_h
#define ai_factory_h

#include <memory>
#include <string>

#include "ai_root.h"

// Creates the AI backend named by the profile's "ai_backend" setting:
//   "ollama" -> ollama_system (a local Ollama server)
//   "gemini" -> gemini_system (Google's Gemini API)
// An unrecognised name prints a warning and falls back to ollama, so a typo
// in settings.json can't leave olli without a chat. Adding a backend later =
// one more branch in ai_factory.cpp.
std::unique_ptr<ai_root> make_ai_system(const std::string& backend);

#endif
