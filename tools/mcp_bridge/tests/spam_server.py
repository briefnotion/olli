import sys, json, time, threading
def spam():
    sys.stderr.write("E" * 3000000); sys.stderr.flush()
    while True:
        time.sleep(1); sys.stderr.write("E" * 100000); sys.stderr.flush()
threading.Thread(target=spam, daemon=True).start()
for line in sys.stdin:
    m = json.loads(line); mid = m.get("id"); me = m.get("method")
    if me == "initialize": print(json.dumps({"jsonrpc":"2.0","id":mid,"result":{"protocolVersion":"2025-06-18","serverInfo":{"name":"n"}}}), flush=True)
    elif me == "tools/list": print(json.dumps({"jsonrpc":"2.0","id":mid,"result":{"tools":[{"name":"t","description":"d"}]}}), flush=True)
    elif me == "tools/call": print(json.dumps({"jsonrpc":"2.0","id":mid,"result":{"content":[{"type":"text","text":"ok"}]}}), flush=True)
