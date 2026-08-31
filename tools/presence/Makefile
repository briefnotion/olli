# Standalone build for presence - deliberately independent of olli's own
# CMake build (see ../PROTOCOL.md's "Repo / build layout" section). Only
# dependency is the same header-only nlohmann/json checkout olli's own build
# already expects as a sibling of this repo (see the main README's
# "Building the C++ core" section).

CXX ?= g++
CXXFLAGS = -std=c++17 -Wall -Wextra -I../../../json/include

presence: presence.cpp helper_presence.cpp helper_presence.hpp
	$(CXX) $(CXXFLAGS) -o presence presence.cpp helper_presence.cpp

clean:
	rm -f presence

.PHONY: clean
