# Standalone build for the Hue remote tool - deliberately independent of
# olli's own CMake build (see ../PROTOCOL.md's "Repo / build layout"
# section). Same nlohmann/json sibling-repo dependency every remote tool
# needs, plus two this tool is the first of the remote tools to need:
# libcurl (talks to the Hue bridge's REST API directly - see hue.cpp's
# HUE_LIGHT_CLASS) and pthread (the flash-cancel path in handle_call() runs
# on a detached std::thread).

CXX ?= g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread -I../../../json/include
LDLIBS = -lcurl

hue: hue.cpp olli_link.cpp olli_link.hpp
	$(CXX) $(CXXFLAGS) -o hue hue.cpp olli_link.cpp $(LDLIBS)

clean:
	rm -f hue

.PHONY: clean
