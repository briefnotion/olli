# Standalone build for template_tool - deliberately independent of olli's
# own CMake build (see ../PROTOCOL.md's "Repo / build layout" section).
# Only dependency is the same header-only nlohmann/json checkout olli's own
# build already expects as a sibling of this repo (see the main README's
# "Building the C++ core" section).
#
# Copying this directory for a new tool? Rename template_tool.cpp and the
# two spots below (the .cpp filename and the binary name) to match - the
# olli_link.cpp/.hpp plumbing lives in ../olli_link/, shared by every tool,
# and needs no changes. See README.md in this directory for the full
# walkthrough.

CXX ?= g++
CXXFLAGS = -std=c++17 -Wall -Wextra -I../../../json/include -I../olli_link

template_tool: template_tool.cpp ../olli_link/olli_link.cpp ../olli_link/olli_link.hpp
	$(CXX) $(CXXFLAGS) -o template_tool template_tool.cpp ../olli_link/olli_link.cpp

clean:
	rm -f template_tool

.PHONY: clean
