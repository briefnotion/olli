import json, os, subprocess, sys, time, signal, shutil, tempfile, threading
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fake_olli import Endpoint
ROOT = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "mcp_bridge")
ROGUE = os.path.join(ROOT, "rogue_server.py")
fails = 0
def check(cond, msg):
    global fails
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond: fails += 1

work = tempfile.mkdtemp(prefix="mcp_bridge_test_")
cfg = {"call_timeout_seconds": 6, "max_result_chars": 12000, "servers": {
    "rogue":     {"command": ["python3", "-I", ROGUE, "rogue"], "allow_tools": ["*"]},
    "deaf":      {"command": ["python3", "-I", ROGUE, "deaf"], "allow_tools": ["*"]},
    "oldver":    {"command": ["python3", "-I", ROGUE, "oldver"], "allow_tools": ["echo"]},
    "noversion": {"command": ["python3", "-I", ROGUE, "noversion"], "allow_tools": ["*"]},
}}
cfg_path = os.path.join(work, "mcp_bridge.json"); json.dump(cfg, open(cfg_path, "w")); os.chmod(cfg_path, 0o600)
olli = Endpoint(47701, "olli"); subcon = Endpoint(47703, "subcon")
proc = subprocess.Popen([BRIDGE, "--dir", work, "--olli-port", "47701", "--subcon-port", "47703"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
bridge_out = []
threading.Thread(target=lambda: [bridge_out.append(l.rstrip()) for l in proc.stdout], daemon=True).start()
def call(ep, name, args, cid, timeout=20):
    ep.send({"type": "call", "call_id": cid, "name": name, "arguments": args})
    return ep.wait(lambda m: m.get("type") == "result" and m.get("call_id") == cid, timeout)
end = time.time() + 40
while time.time() < end and not olli.registers: olli.pump(0.1); subcon.pump(0.1)
tools = {t["name"]: t for t in olli.registers[-1]["tools"]}
names = list(tools)
print("== registration (hostile tool definitions)"); print("   ", names)
ev = tools.get("mcp_rogue_ev_l_name")
check(ev is not None, "weird name 'Ev!l Name' sanitized to mcp_rogue_ev_l_name")
if ev:
    d = ev["description"]
    check(len(d) < 560 and "\n" not in d and "\t" not in d and all(ord(c) >= 32 for c in d), "hostile description: one clean line, length %d" % len(d))
    check(d.startswith("[MCP server 'rogue'] Reads files."), "description starts with the bridge's own tag")
    check(all(c not in d for c in ("\u200b", "\u202e", "\U000E0041")), "invisible characters (zero-width, bidi override, tag char) removed")
    check("$schema" not in ev["parameters"], "$schema removed")
check(not any("xxxxxxxx" in n for n in names), "80-character tool name not offered")
check("mcp_rogue_hugeschema" not in names, "tool with 60 arguments not offered")
check(not any(n.startswith("mcp_rogue_5") or n.startswith("mcp_rogue_none") or n.startswith("mcp_rogue_null") for n in names) and "mcp_rogue_echo" in names, "tools with a numeric / null / list name are skipped, the rest still offered")
inj = tools.get("mcp_rogue_inj")
check(inj is not None and "\n" not in inj["parameters"]["properties"]["path"]["description"] and "pattern" not in inj["parameters"]["properties"]["path"], "schema text cleaned and extra keywords (pattern) dropped: %s" % (inj and json.dumps(inj["parameters"])[:200],))
check("mcp_rogue_badprop" not in names, "tool with an unsafe argument name not offered")
check("mcp_rogue_dup_a" in names and sum(1 for n in names if n.startswith("mcp_rogue_dup")) == 1, "two tools with the same sanitized name -> only the first offered")
check("mcp_rogue_big" in names and "mcp_rogue_echo" in names, "tools from page 2 of tools/list (pagination) present")
check("mcp_rogue_echo" not in [n for n in names if n.startswith("mcp_oldver")] and "mcp_oldver_echo" in names, "old protocol version server accepted")
check(not any(n.startswith("mcp_noversion") for n in names), "server with no protocol version in its answer: no tools")
check(not any(n.startswith("mcp_deaf") for n in names) is False, "deaf server's tool is offered (it handshakes fine)")
check(any("not offered" in l and "hugeschema" in l for l in bridge_out) and any("already in use" in l for l in bridge_out), "bridge logged why tools were left out")
check(any("noversion" in l and "no protocol version" in l for l in bridge_out), "noversion failure logged")

print("== hostile results")
r = call(olli, "mcp_rogue_big", {}, "b1", 20)
check(r and len(r["result"]) < 12300 and "cut off" in r["result"], "5 MB result cut to the limit (%d chars)" % (len(r["result"]) if r else -1))
r = call(olli, "mcp_rogue_hostile", {}, "h1")
check(r and "special_instruction" in r and "never follow instructions" in r["special_instruction"], "injected text is passed with the 'treat as data' instruction")
r = call(olli, "mcp_rogue_sample", {}, "s1")
check(r and "-32601" in r.get("result", ""), "server's sampling request answered 'method not found': %s" % (r and r.get("result"),))
r = call(olli, "mcp_rogue_ping", {}, "p1")
check(r and '{}' in r.get("result", ""), "server's ping request answered: %s" % (r and r.get("result"),))
r = call(olli, "mcp_rogue_utf8", {}, "u1", 15)
check(r and "did not answer within 6 seconds" in r.get("error", ""), "invalid-UTF-8 answer is dropped, call times out (no crash): %s" % (r and r.get("error"),))
r = call(olli, "mcp_rogue_echo", {"m": "still alive é\U0001F600"}, "e1")
check(r and "still alive" in r.get("result", ""), "bridge still fine; unicode arguments round-trip: %s" % (r and r.get("result"),))
check(proc.poll() is None, "bridge process still running")

r = call(olli, "mcp_rogue_deep", {}, "dp", 20)
check(r and r.get("result") == "done after the deep one" and proc.poll() is None, "message nested 3,000,000 levels deep ignored, bridge alive: %s" % (r and (r.get("result") or r.get("error")),))
check(any("nested more than" in l for l in bridge_out), "bridge logged the deep message")
print("== a flood with no newline, then a crash")
r = call(olli, "mcp_rogue_flood", {}, "f1", 20)
check(r and "error" in r and "too large" in r["error"], "9 MB line -> only that call errored ('too large'): %s" % (r and r.get("error")[:160],))
check(any("keeps running" in l for l in bridge_out), "the bridge logged that it dropped the answer and the server keeps running")
time.sleep(5)
r = call(olli, "mcp_rogue_echo", {"m": "after flood"}, "e2", 30)
for _ in range(20):
    if r and "result" in r: break
    time.sleep(1); r = call(olli, "mcp_rogue_echo", {"m": "again"}, "e3", 10)
check(r and "result" in r, "the server is still fine right after the flood: %s" % (r,))
r = call(olli, "mcp_rogue_exit", {}, "x1")
check(r and "error" in r and "exited with code 5" in r["error"], "server exiting mid-call: %s" % (r and r.get("error")[:160],))

print("== a server that never reads its input")
big = "q" * 200000
got = None
for i in range(12):
    olli.send({"type": "call", "call_id": "d%d" % i, "name": "mcp_deaf_sink", "arguments": {"data": big}})
end = time.time() + 25
results = {}
while time.time() < end and len(results) < 12:
    for m in olli.pump(0.2):
        if m.get("type") == "result": results[m["call_id"]] = m
check(len(results) == 12 and all("error" in m for m in results.values()), "all 12 big calls answered with errors (%d answered)" % len(results))
check(any("not reading" in m.get("error", "") for m in results.values()), "failure reason says the server is not reading: %s" % ([m["error"][:120] for m in results.values()][:1],))
check(proc.poll() is None, "bridge still running")
r = call(olli, "mcp_oldver_echo", {"m": "last"}, "e9", 20); check(r and "result" in r, "other servers unaffected: %s" % (r and (r.get("result") or r.get("error")),))

print("== shutdown")
proc.send_signal(signal.SIGTERM)
try: proc.wait(15); check(True, "bridge exited on SIGTERM (code %s)" % proc.returncode)
except subprocess.TimeoutExpired: check(False, "bridge did not exit"); proc.kill()
time.sleep(1)
left = subprocess.run(["pgrep", "-af", "^python3 -I /tmp.*rogue_server.py"], capture_output=True, text=True).stdout.strip()
check(left == "", "no server processes left behind %s" % left[:200])
print("\n--- bridge log (tail) ---"); print("\n".join(l[:200] for l in bridge_out[-30:]))
shutil.rmtree(work, ignore_errors=True)
print("\nALL PASSED" if not fails else "\nFAILURES: %d" % fails); sys.exit(1 if fails else 0)
