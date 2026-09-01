# Standalone build for the clock stub - deliberately independent of olli's
# own CMake build (see ../PROTOCOL.md's "Repo / build layout" section).
# Only dependency is the same header-only nlohmann/json checkout olli's own
# build already expects as a sibling of this repo (see the main README's
# "Building the C++ core" section).

CXX ?= g++
CXXFLAGS = -std=c++17 -Wall -Wextra -I../../../json/include -I../olli_link

clock: clock.cpp ../olli_link/olli_link.cpp ../olli_link/olli_link.hpp
	$(CXX) $(CXXFLAGS) -o clock clock.cpp ../olli_link/olli_link.cpp

clean:
	rm -f clock

.PHONY: clean
