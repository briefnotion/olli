# Standalone build for the Hue remote tool - deliberately independent of
# olli's own CMake build (see ../PROTOCOL.md's "Repo / build layout"
# section). Same nlohmann/json sibling-repo dependency every remote tool
# needs, plus: libcurl (talks to the Hue bridge's REST API directly - see
# hue.cpp's HUE_LIGHT_CLASS), pthread (the flash-cancel path in
# handle_call() runs on a detached std::thread), and the same from-source
# static ncursesw build olli's own CMake build uses for its display (see
# source/CMakeLists.txt's ncursesw section and BUILD_AND_INSTALL.md's
# "Building ncursesw") - built once, shared by every remote tool that needs
# a display, not just olli itself.

CXX ?= g++
NCURSES_PREFIX = ../../../ncurses-snapshots/install
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread -I../../../json/include -I../olli_link -I../olli_display -I$(NCURSES_PREFIX)/include
LDLIBS = -lcurl -L$(NCURSES_PREFIX)/lib -lncursesw

hue: hue.cpp ../olli_link/olli_link.cpp ../olli_link/olli_link.hpp ../olli_display/olli_display.cpp ../olli_display/olli_display.hpp
	$(CXX) $(CXXFLAGS) -o hue hue.cpp ../olli_link/olli_link.cpp ../olli_display/olli_display.cpp $(LDLIBS)

clean:
	rm -f hue

.PHONY: clean
