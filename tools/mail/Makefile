# Standalone build for the mail remote tool - deliberately independent of
# olli's own CMake build (see ../PROTOCOL.md's "Repo / build layout" section).
# Same nlohmann/json sibling-repo dependency every remote tool needs, plus
# libcurl (the IMAP STATUS request to Gmail) and pthread (the check runs on
# its own thread so a slow connection can't stall the heartbeat). No display,
# so no ncurses.

CXX ?= g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread -I../../../json/include -I../olli_link
LDLIBS = -lcurl

mail: mail.cpp ../olli_link/olli_link.cpp ../olli_link/olli_link.hpp
	$(CXX) $(CXXFLAGS) -o mail mail.cpp ../olli_link/olli_link.cpp $(LDLIBS)

clean:
	rm -f mail

.PHONY: clean
