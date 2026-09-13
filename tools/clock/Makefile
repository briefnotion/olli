# Standalone build for the clock stub - deliberately independent of olli's
# own CMake build (see ../PROTOCOL.md's "Repo / build layout" section).
# Depends on the same header-only nlohmann/json checkout olli's own build
# already expects as a sibling of this repo (see the main README's
# "Building the C++ core" section), plus the same from-source static
# ncursesw build olli's own CMake build uses for its display (see
# source/CMakeLists.txt's ncursesw section and BUILD_AND_INSTALL.md's
# "Building ncursesw") - built once, shared by every remote tool that needs
# a display, not just olli itself.

CXX ?= g++
NCURSES_PREFIX = ../../../ncurses-snapshots/install
CXXFLAGS = -std=c++17 -Wall -Wextra -I../../../json/include -I../olli_link -I../olli_display -I$(NCURSES_PREFIX)/include
LDFLAGS = -L$(NCURSES_PREFIX)/lib -lncursesw

clock: clock.cpp ../olli_link/olli_link.cpp ../olli_link/olli_link.hpp ../olli_display/olli_display.cpp ../olli_display/olli_display.hpp
	$(CXX) $(CXXFLAGS) -o clock clock.cpp ../olli_link/olli_link.cpp ../olli_display/olli_display.cpp $(LDFLAGS)

clean:
	rm -f clock

.PHONY: clean
