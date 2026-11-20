# mcp_bridge

Connects olli (and subcon) to **MCP servers**.

MCP - the Model Context Protocol - is the industry-standard way for a program to offer tools to an AI. There are ready-made MCP servers for
time zones, files, databases, calendars, GitHub, and a great many other things. `mcp_bridge` is a normal olli remote tool
([`../PROTOCOL.md`](../PROTOCOL.md)) that runs the MCP servers you choose and offers their tools to olli as if they were olli's own:

```
olli / subcon  <--- olli_link --->  mcp_bridge  <--- MCP (stdio) --->  MCP servers (npx / uvx / any program)
```

olli's own code knows nothing about MCP. Like every remote tool it holds two links - one to olli (port 47601) and one to subcon (47603) -
so the MCP tools can be used from both. It does not need olli to be running when it starts, and it reconnects by itself.

## Build and run

```bash
cd tools/mcp_bridge
make
./mcp_bridge                    # settings in ~/olli_files/mcp_bridge.json
./mcp_bridge --profile ron      # settings in ~/olli_files_ron/mcp_bridge.json (the folder of olli's profile "ron")
./mcp_bridge 192.168.1.20       # olli on another machine
```

Run it next to olli, like the other tools (it prints a short timestamped log). The profile is chosen when it starts, because the list of
tools has to be known when it registers with olli - so give it the same profile name you give olli. The first start writes an empty
`mcp_bridge.json` (owner-only, mode 600) and tells you where it is.

## Setting up an MCP server

Servers are listed in `<profile folder>/mcp_bridge.json`. **They are only ever added by editing this file** - never by anything the
model can call - because a server is a command that gets run on this machine as you.

```json
{
  "max_result_chars": 12000,
  "call_timeout_seconds": 120,
  "servers": {
    "time": {
      "command": ["uvx", "mcp-server-time==2026.11.18"],
      "allow_tools": ["get_current_time", "convert_time"]
    },
    "files": {
      "enabled": false,
      "command": ["npx", "-y", "@modelcontextprotocol/server-filesystem@2.4.0", "/home/me/olli_notes"],
      "allow_tools": ["read_text_file", "list_directory"],
      "prefix": "notes_",
      "env": {"SOME_KEY": "value"},
      "startup_timeout_seconds": 180
    }
  }
}
```

| Setting | Meaning |
|---------|---------|
| `command` | The program and its arguments, as a list. Run directly - **never through a shell**. Pin exact package versions (`@2.4.0`, `==2026.11.18`), not "latest": the package runs as you. |
| `allow_tools` | Which of the server's tools olli may see. **Empty or missing = none** (a small model copes badly with dozens of tools, and every tool is something a prompt-injected model could call). `["*"]` = all of them. |
| `enabled` | `false` = configured but not started. Default `true`. |
| `prefix` | Put in front of each tool name. Default `mcp_<server>_`, so `get_current_time` becomes `mcp_time_get_current_time` and can never clash with one of olli's own tools. |
| `env` | Extra environment variables for that server (API keys, ...). The server does **not** get the bridge's own environment - only `PATH`, `HOME`, locale, proxy/certificate settings and the XDG folders. |
| `cwd` | Working folder. Default: a private folder `<profile folder>/mcp_bridge/<server>/`. |
| `startup_timeout_seconds` | How long to wait for the server to become ready (the first `npx`/`uvx` run downloads the package). Default 120. |
| `max_result_chars` / `call_timeout_seconds` | Longest answer passed on (longer is cut off, with a note) / longest wait for one call. Defaults 12000 / 120. |

The settings are read when the bridge starts. Problems in the file (a bad server name, a command that is not a list) are skipped and
listed on start-up. The file is refused outright if it is writable by anyone but you, or owned by someone else.

Server names are lower-case letters, digits and underscores (at most 24).

## What olli gets

- **The servers' tools** - only those in `allow_tools`, named `<prefix><tool>` (lower-case, `-`/`.` become `_`). Their descriptions get a
  `[MCP server 'time']` tag in front and are cleaned; argument descriptions are cleaned too (see Safety).
- **`mcp_status`** - which servers are set up, whether each is running, which tools are offered, how many exist but are not allowed.
- **`mcp_set_server_enabled`** - turn one *already configured* server off or on. The model can switch a server off; it can switch one on
  only if it was on in the settings file when the bridge started (a server you left `"enabled": false` stays off until *you* change the file).
  The change is saved to the file (only that one flag; edits you made meanwhile are kept).

When a server comes, goes, or changes its tool list, the bridge drops its link to olli and registers again with the new list (olli sees an
ordinary tool restart; the protocol registers once per connection). It waits for any answer olli is still owed, and for a changed list to
settle, first. At start-up it waits (up to 20 s) for the servers so olli hears about everything in one go.

A server that dies is restarted after 2, 4, 8, 16 seconds; after five failures in a row (or a command that cannot be started at all) it is
parked as *failed* until you restart the bridge or the model/you switch it off and on. While it is down, its tools stay registered and a
call answers with a clear "not running right now" error.

## Safety

The MCP servers are somebody else's programs, and what they say ends up in a language model's context. The bridge treats all of it as
untrusted:

- **Run as you, but contained as far as possible:** direct `exec` (no shell), small environment (no inherited keys), every other file
  descriptor closed in the child (olli's sockets, the instance lock), its own process group (everything an `npx` wrapper starts is stopped
  together, and force-killed if it ignores SIGTERM), ended if the bridge dies, stderr kept in `<profile folder>/mcp_bridge/<server>.stderr.log`
  (mode 600, trimmed past 1 MB) - never printed.
- **Tool descriptions and argument descriptions are sanitised** before they reach the model: one clean line, length-capped, invisible
  characters (zero-width, bidi overrides, "tag" characters) removed; only the JSON-Schema keywords a model needs are kept
  (`type`, `properties`, `required`, `enum`, `items`, `description`, ...); odd argument names or values mean the tool is not offered. This
  limits what a hostile description can *hide*; it cannot make the words themselves trustworthy - so you choose the servers and the
  `allow_tools`.
- **Results** are plain text, cut to `max_result_chars`, always valid UTF-8, and passed to olli with an instruction to report what they
  say but never follow instructions inside them. Images/audio become a one-line note; links are written out as text.
- **Limits:** a message from a server nested over 64 levels is ignored; an answer over 8 MB (say, a 5 MB file read, which comes back doubled and
  escaped) is dropped and the calls waiting on that server get an error ("too large") while the server keeps running; a server that stops reading its
  input, or sends one endless message (over 64 MB), is treated as failed. Arguments from olli over 200 KB or nested over 64 levels are refused for that one
  call. A call that takes longer than `call_timeout_seconds` is cancelled.
- **A second copy refuses to start** (single-instance lock), so two bridges never fight over the same servers.

## What it does not do (yet)

- **Text only.** MCP results can carry images, resource links and structured data; they are flattened to text. Real support needs olli's
  remote-tool protocol to carry them (links are the same gap the web tools are waiting on, see TODO.md).
- **stdio servers only** - no remote (HTTP) MCP servers yet.
- **Nothing pushed to olli.** MCP has no standard "tell the user something now" message, so MCP servers' notifications (progress, logging,
  resource updates) are ignored. Only calls and their answers go through.
- **While the bridge (or one server) is down, the model may still try to use its tools** - it remembers using them. olli answers such a call within a few
  seconds with "that tool is not available right now", and the model explains it in its own words (with both Gemini and the local Ollama model). Asking
  again once the bridge is back works. (See TODO.md, 2026-11-20 night.)
- **No live reload** of `mcp_bridge.json`: restart the bridge to pick up edits.
- If the bridge is killed hard (SIGKILL) or crashes, a server's grandchildren (the `node` behind an `npx`) can outlive it - only the
  direct child is ended automatically. A normal stop or Ctrl+C cleans everything up. (`pgrep -af mcp-server` finds strays.)
- Tools only - not MCP "resources" or "prompts".

## Layout

| File | |
|------|-|
| `mcp_bridge.cpp` | `main()`, the single `select()` loop, tool registration, the two management tools, config load/save |
| `mcp_server.hpp/.cpp` | `MCP_SERVER`: one child process, JSON-RPC over its pipes, handshake, `tools/list`, `tools/call`, restarts |
| `mcp_pure.hpp/.cpp` | Pure helpers: config parsing, name/schema/description cleaning, result flattening, UTF-8 and depth checks |
| `../olli_link/` | The shared olli connection code (two small additions for this tool: `set_register_message()`, `is_duplicate_instance()`) |
| `tests/` | See [`tests/README.md`](tests/README.md) |
