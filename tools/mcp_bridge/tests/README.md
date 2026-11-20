# mcp_bridge tests

Everything here runs against the real built `mcp_bridge` (and the real `mcp_pure.cpp` / `mcp_server.cpp`) - nothing is mocked
except olli itself: `fake_olli.py` is a pair of tiny TCP listeners that speak olli's real remote-tool protocol on test ports
**47701 / 47703** (never olli's own 47601 / 47603, so a running olli is not disturbed). Run them one at a time.

| Test | Needs | What it proves |
|------|-------|----------------|
| `test_pure.cpp` (`make test-unit`) | nothing | config parsing, name/schema/description cleaning (injection text, invisible characters, odd names), result flattening, UTF-8 safety, JSON depth check - built with ASan/UBSan |
| `test_server.cpp` (`make test-server`) | `npx`, internet (first run) | the child-process layer against the real `server-everything`: handshake, calls, slow + fast calls together, cancel, kill mid-call and restart, bad command, silent server, trimmed environment, no leaked file descriptors |
| `test_real_servers.py` | `npx`, `uvx`, internet | the whole bridge with two real MCP servers: registration (one connection), allow-list, calls on the olli AND the subcon link, images, timeouts, enable/disable re-registration, restart, clean shutdown |
| `test_hostile_server.py` | python3 | a deliberately malicious server (`rogue_server.py`): hostile descriptions and schemas, huge results, invalid UTF-8, a 9 MB line, a 3,000,000-level-deep message, server requests, a server that never reads |
| `test_scenarios.py` | python3 | bad config files, bridge started before olli, olli restarting, a second copy refused, heartbeat during a 22 s call, abandoned calls |
| `test_chaos.py` | `npx`, `uvx`, internet; about 8 min | tries to break it: 400 concurrent calls over both links reusing the same call ids (each must get its own payload back on its own link), slow + fast calls together, a server killed with 20 calls in flight, olli's connection dropped three times mid-calls, 15 hostile messages from olli (garbage, wrong types, 200 KB call id, 50,000-level nesting), a 2000-call burst, a server switched off/on 6 times under load, a 5 MB file read, and a 3-minute soak watching memory, file descriptors and child processes |
| `test_review_fixes.py` | python3 | regression tests for what the code review found: deep schemas, a SIGTERM-ignoring grandchild, the model starting a server that was switched off, edits to the file kept, oversized arguments, stderr growth |

```bash
cd tools/mcp_bridge && make                 # the bridge itself
make test-unit                              # fast, no network
python3 -I tests/test_hostile_server.py     # and the other test_*.py files, one at a time
```

The two tests that start real MCP servers pin exact package versions (`server-everything@2.3.1`, `mcp-server-time==2026.11.18`).
