# Standalone build for the subcon tool - deliberately independent of olli's
# own CMake build (see ../PROTOCOL.md's "Repo / build layout" section). Same
# nlohmann/json sibling-repo dependency every remote tool needs, plus:
# libcurl (the Ollama chat requests and the web_search/fetch internal tools),
# pthread (the one JOB_WORKER thread behind them - see job_worker.hpp), and
# the same from-source static ncursesw build olli's own CMake build uses for
# its display (see source/CMakeLists.txt's ncursesw section and
# BUILD_AND_INSTALL.md's "Building ncursesw"), used only when stdout is a
# terminal - the tool runs headless (nohup, tmux pipe) with no display.

CXX ?= g++
NCURSES_PREFIX = ../../../ncurses-snapshots/install
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread -I../../../json/include -I../olli_link -I../olli_display -I$(NCURSES_PREFIX)/include
LDLIBS = -lcurl -L$(NCURSES_PREFIX)/lib -lncursesw

SOURCES = subcon.cpp tool_server.cpp reasoner.cpp job_worker.cpp ollama_client.cpp internal_tools.cpp subcon_store.cpp \
          ../olli_link/olli_link.cpp ../olli_display/olli_display.cpp
HEADERS = tool_server.hpp reasoner.hpp job_worker.hpp ollama_client.hpp internal_tools.hpp subcon_store.hpp subcon_tasks.h \
          ../olli_link/olli_link.hpp ../olli_display/olli_display.hpp

subcon: $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o subcon $(SOURCES) $(LDLIBS)

clean:
	rm -f subcon

.PHONY: clean
