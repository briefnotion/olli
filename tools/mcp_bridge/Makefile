# Standalone build for the MCP bridge - deliberately independent of olli's
# own CMake build (see ../PROTOCOL.md's "Repo / build layout" section).
# Only dependency is the same header-only nlohmann/json checkout olli's own
# build already expects as a sibling of this repo. No libcurl, no ncurses:
# the bridge only talks to olli over a socket and to MCP servers over pipes.

CXX ?= g++
CXXFLAGS = -std=c++17 -Wall -Wextra -I../../../json/include -I../olli_link
SOURCES = mcp_bridge.cpp mcp_pure.cpp mcp_server.cpp ../olli_link/olli_link.cpp
HEADERS = mcp_pure.hpp mcp_server.hpp ../olli_link/olli_link.hpp

mcp_bridge: $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o mcp_bridge $(SOURCES)

# Tests - see tests/README.md. Unit tests need nothing; the others start real or fake MCP servers.
test-unit:
	$(CXX) $(CXXFLAGS) -I. -Wconversion -Wsign-conversion -fsanitize=address,undefined -o tests/test_pure tests/test_pure.cpp mcp_pure.cpp
	./tests/test_pure

test-server:
	$(CXX) $(CXXFLAGS) -I. -fsanitize=address,undefined -o tests/test_server tests/test_server.cpp mcp_server.cpp mcp_pure.cpp
	mkdir -p /tmp/mcp_server_test_work && ./tests/test_server /tmp/mcp_server_test_work

clean:
	rm -f mcp_bridge tests/test_pure tests/test_server

.PHONY: clean test-unit test-server
