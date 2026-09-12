# Standalone build for rag_admin - deliberately independent of olli's own
# CMake build (see ../../PROTOCOL.md's "Repo / build layout" section), same
# as every other tool here. Never talks to olli over the wire, so unlike
# ../../olli_link/-based tools, this only links ../rag_db/'s shared storage/
# embedding/chunking code - no olli_link.
#
# Needs: the same header-only nlohmann/json checkout every tool here expects
# as a sibling of this repo, plus libsqlite3-dev and libcurl (already
# required by ../rag_db/'s own smoke test).

CXX ?= g++
CXXFLAGS = -std=c++17 -Wall -Wextra -I../../../../json/include -I../rag_db
LDLIBS = -lsqlite3 -lcurl

SOURCES = rag_admin.cpp ../rag_db/rag_db.cpp ../rag_db/rag_embed.cpp ../rag_db/rag_chunk.cpp ../rag_db/rag_sync.cpp
HEADERS = ../rag_db/rag_db.hpp ../rag_db/rag_embed.hpp ../rag_db/rag_chunk.hpp ../rag_db/rag_sync.hpp

rag_admin: $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o rag_admin $(SOURCES) $(LDLIBS)

clean:
	rm -f rag_admin

.PHONY: clean
