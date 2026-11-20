# A minimal MCP server whose one tool ("nap") takes 22 seconds to answer - longer than olli's 15 s
# "dead peer" timeout - to prove the bridge keeps its heartbeat going while a call is pending.
import sys, json, time
def out(o): sys.stdout.write(json.dumps(o)+"\n"); sys.stdout.flush()
for line in sys.stdin:
    m = json.loads(line); mid = m.get("id"); me = m.get("method")
    if me == "initialize": out({"jsonrpc":"2.0","id":mid,"result":{"protocolVersion":"2025-06-18","capabilities":{},"serverInfo":{"name":"slow"}}})
    elif me == "tools/list": out({"jsonrpc":"2.0","id":mid,"result":{"tools":[{"name":"nap","description":"sleeps then answers","inputSchema":{"type":"object","properties":{}}}]}})
    elif me == "tools/call":
        time.sleep(22); out({"jsonrpc":"2.0","id":mid,"result":{"content":[{"type":"text","text":"rested"}]}})
