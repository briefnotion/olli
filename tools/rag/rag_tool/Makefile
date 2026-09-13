# Standalone build for rag_tool - deliberately independent of olli's own
# CMake build (see ../../PROTOCOL.md's "Repo / build layout" section), same
# as every other tool here. Links ../../olli_link/ (the shared olli
# connection plumbing every remote tool needs), ../../olli_display/ (the
# shared ncurses display plumbing every remote tool with a live terminal UI
# needs), plus ../rag_db/'s shared storage/embedding/chunking code.
#
# Needs: the same header-only nlohmann/json checkout every tool here expects
# as a sibling of this repo, libsqlite3-dev and libcurl (already required by
# ../rag_db/'s own smoke test and ../rag_admin/), and the same from-source
# static ncursesw build olli's own CMake build uses for its display (see
# source/CMakeLists.txt's ncursesw section and BUILD_AND_INSTALL.md's
# "Building ncursesw").

CXX ?= g++
NCURSES_PREFIX = ../../../../ncurses-snapshots/install
CXXFLAGS = -std=c++17 -Wall -Wextra -I../../../../json/include -I../../olli_link -I../../olli_display -I../rag_db -I$(NCURSES_PREFIX)/include
LDLIBS = -lsqlite3 -lcurl -L$(NCURSES_PREFIX)/lib -lncursesw

SOURCES = rag_tool.cpp ../../olli_link/olli_link.cpp ../../olli_display/olli_display.cpp ../rag_db/rag_db.cpp ../rag_db/rag_embed.cpp ../rag_db/rag_chunk.cpp ../rag_db/rag_sync.cpp
HEADERS = ../../olli_link/olli_link.hpp ../../olli_display/olli_display.hpp ../rag_db/rag_db.hpp ../rag_db/rag_embed.hpp ../rag_db/rag_chunk.hpp ../rag_db/rag_sync.hpp

rag_tool: $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o rag_tool $(SOURCES) $(LDLIBS)

clean:
	rm -f rag_tool

.PHONY: clean
