// RAG_EMBEDDER - talks to Ollama's /api/embeddings endpoint directly, over
// libcurl (same pattern ../../hue/hue.cpp uses for the Hue bridge's REST
// API). Not part of the olli<->tool wire protocol (see ../../PROTOCOL.md) -
// this is a separate HTTP call this program makes on its own, to turn text
// into the vectors rag_db.hpp stores and searches.

#pragma once

#include <string>
#include <vector>

class RAG_EMBEDDER {
    public:
        // Defaults match olli's own Ollama instance (port 11434, see
        // source/olla.h) and the embedding model already pulled for this
        // (nomic-embed-text, 768-dim).
        explicit RAG_EMBEDDER(std::string host = "127.0.0.1", int port = 11434, std::string model = "nomic-embed-text");

        // Two entry points, not one - nomic-embed-text (and several other
        // embedding models) are trained with task-prefixed inputs and rank
        // noticeably worse without them: a real chunk containing the exact
        // answer to a test query scored *below* an unrelated intro
        // paragraph with a bare embed() call, and came out ahead once
        // "search_document:"/"search_query:" were added (see the design
        // discussion this came out of). Both return an empty vector on
        // failure (network error, non-200, malformed response) - check
        // last_error() for what went wrong.
        std::vector<float> embed_document(const std::string& text);
        std::vector<float> embed_query(const std::string& text);

        std::string last_error() const { return error; }

    private:
        std::vector<float> embed(const std::string& prefixed_text);

        std::string host;
        int port;
        std::string model;
        std::string error;
};
