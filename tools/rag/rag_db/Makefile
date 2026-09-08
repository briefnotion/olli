# Phase 1 smoke test for the rag_db shared library (rag_db.hpp/cpp,
# rag_embed.hpp/cpp, rag_chunk.hpp/cpp) - proves schema creation, real
# Ollama embedding, and cosine search round-trip end to end before rag_admin
# (Phase 2) or rag_tool (Phase 3) get built on top of these same files. See
# ../../PROTOCOL.md for how those two will eventually use this directory.
#
# rag_db itself has no standalone binary of its own - like ../../olli_link/,
# it's meant to be compiled directly into whichever program needs it. This
# Makefile only builds the throwaway smoke test (rag_db_test).

CXX ?= g++
CXXFLAGS = -std=c++17 -Wall -Wextra -I../../../../json/include
LDLIBS = -lsqlite3 -lcurl

SOURCES = rag_db_test.cpp rag_db.cpp rag_embed.cpp rag_chunk.cpp
HEADERS = rag_db.hpp rag_embed.hpp rag_chunk.hpp

rag_db_test: $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o rag_db_test $(SOURCES) $(LDLIBS)

test: rag_db_test
	./rag_db_test

clean:
	rm -f rag_db_test

.PHONY: test clean
