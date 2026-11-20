import json, os, subprocess, sys, time, signal, shutil, tempfile, threading
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fake_olli import Endpoint
ROOT = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "mcp_bridge")
PY = ["python3", "-I"]
fails = 0
def check(cond, msg):
    global fails
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond: fails += 1
def mkdir(cfg):
    d = tempfile.mkdtemp(prefix="mcp_bridge_test_"); p = os.path.join(d, "mcp_bridge.json")
    json.dump(cfg, open(p, "w")); os.chmod(p, 0o600); return d, p
def start(d):
    p = subprocess.Popen([BRIDGE, "--dir", d, "--olli-port", "47701", "--subcon-port", "47703"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
    lines = []; threading.Thread(target=lambda: [lines.append(l.rstrip()) for l in p.stdout], daemon=True).start(); return p, lines
def call(ep, name, args, cid, timeout=20):
    ep.send({"type": "call", "call_id": cid, "name": name, "arguments": args})
    return ep.wait(lambda m: m.get("type") == "result" and m.get("call_id") == cid, timeout)
def registered(olli, subcon, timeout=40):
    olli.registers.clear(); subcon.registers.clear()
    end = time.time() + timeout
    while time.time() < end and not olli.registers: olli.pump(0.1); subcon.pump(0.05)
    return bool(olli.registers)
olli = Endpoint(47701, "olli"); subcon = Endpoint(47703, "subcon")

print("== a server that lists a 2,000,000-level-deep schema")
d, cp = mkdir({"servers": {"deep": {"command": PY + [os.path.join(ROOT, "nested_list_server.py")], "allow_tools": ["*"], "startup_timeout_seconds": 5},
                             "ok": {"command": PY + [os.path.join(ROOT, "rogue_server.py"), "oldver"], "allow_tools": ["echo"]}}})
p, out = start(d)
check(registered(olli, subcon) and "mcp_oldver_echo" not in olli.tool_names() and "mcp_ok_echo" in olli.tool_names(), "other server's tools registered: %s" % olli.tool_names())
time.sleep(7)
check(p.poll() is None and any("nested more than" in l for l in out) and any("no answer after 5 s" in l for l in out), "bridge alive; deep tools/list ignored and the server reported as not answering")
p.send_signal(signal.SIGTERM); p.wait(10); shutil.rmtree(d)

print("== a grandchild that ignores SIGTERM does not survive the bridge")
d, cp = mkdir({"servers": {"stubborn": {"command": ["sh", "-c", "trap '' TERM; sleep 997 & exec python3 -I %s oldver" % os.path.join(ROOT, "rogue_server.py")], "allow_tools": ["echo"]}}})
p, out = start(d)
check(registered(olli, subcon), "registered")
check(subprocess.run(["pgrep", "-fx", "sleep 997"], capture_output=True).returncode == 0, "the stubborn grandchild is running")
p.send_signal(signal.SIGTERM); t0 = time.time(); p.wait(15)
time.sleep(1)
gone = subprocess.run(["pgrep", "-fx", "sleep 997"], capture_output=True).returncode != 0
check(gone, "grandchild killed on bridge shutdown (bridge took %.1fs)" % (time.time() - t0))
if not gone: subprocess.run(["pkill", "-KILL", "-fx", "sleep 997"])
shutil.rmtree(d)

print("== the model may not start a server the person switched off; edits to the file are kept")
d, cp = mkdir({"servers": {"on": {"command": PY + [os.path.join(ROOT, "rogue_server.py"), "oldver"], "allow_tools": ["echo"]},
                           "off": {"command": PY + [os.path.join(ROOT, "rogue_server.py"), "oldver"], "enabled": False, "allow_tools": ["echo"]}}})
p, out = start(d)
check(registered(olli, subcon), "registered")
r = call(olli, "mcp_set_server_enabled", {"server": "off", "enabled": True}, "t1")
check(r and "error" in r and "switched off" in r["error"], "starting a switched-off server refused: %s" % (r and (r.get("error") or r.get("result"))[:120],))
check(json.load(open(cp))["servers"]["off"]["enabled"] is False and not any("[off] started" in l for l in out), "it did not start and the file is unchanged")
# the person edits the file while the bridge runs
cfg = json.load(open(cp)); cfg["servers"]["added_later"] = {"command": ["true"], "allow_tools": ["*"]}; cfg["servers"]["on"]["allow_tools"] = ["echo", "get-sum"]
json.dump(cfg, open(cp, "w"))
r = call(olli, "mcp_set_server_enabled", {"server": "on", "enabled": False}, "t2")
saved = json.load(open(cp))
check(r and "result" in r and saved["servers"]["on"]["enabled"] is False, "model can still switch a running server off (and it is saved)")
check("added_later" in saved["servers"] and saved["servers"]["on"]["allow_tools"] == ["echo", "get-sum"], "the person's edits made meanwhile were kept")
check(oct(os.stat(cp).st_mode & 0o777) == "0o600" and not os.path.exists(cp + ".tmp"), "file 0600, no temp file left")
r = call(olli, "mcp_set_server_enabled", {"server": "on", "enabled": True}, "t3")
check(r and "result" in r, "model can switch it back on (it was on in the file when the bridge started)")
p.send_signal(signal.SIGTERM); p.wait(10); shutil.rmtree(d)

print("== oversized arguments are refused for that call only")
d, cp = mkdir({"servers": {"rogue": {"command": PY + [os.path.join(ROOT, "rogue_server.py"), "oldver"], "allow_tools": ["echo"]}}})
p, out = start(d)
check(registered(olli, subcon), "registered")
for i in range(6):
    r = call(olli, "mcp_rogue_echo", {"m": "x" * 300000}, "big%d" % i)
    if i == 0: check(r and "too large" in r.get("error", ""), "300 KB of arguments refused: %s" % (r and r.get("error")[:100],))
r = call(olli, "mcp_status", {}, "st")
check(r and "rogue: ready" in r["result"], "server still healthy after 6 oversized calls (not counted as failures): %s" % (r["result"].splitlines()[1] if r else None,))
p.send_signal(signal.SIGTERM); p.wait(10); shutil.rmtree(d)

print("== a server writing megabytes to stderr gets its log trimmed")
d, cp = mkdir({"servers": {"spam": {"command": PY + [os.path.join(ROOT, "spam_server.py")], "allow_tools": ["*"]}}})
p, out = start(d)
check(registered(olli, subcon), "registered")
log = os.path.join(d, "mcp_bridge", "spam.stderr.log")
end = time.time() + 36; peak = 0
while time.time() < end:
    olli.pump(0.2); subcon.pump(0.05); peak = max(peak, os.path.getsize(log))
size1 = os.path.getsize(log)
check(size1 < 1536 * 1024 and peak < 5 * 1024 * 1024, "log kept small while the server keeps writing (3 MB at start + 100 KB/s; peak %d, now %d bytes)" % (peak, size1))
r = call(olli, "mcp_spam_t", {}, "s1"); check(r and r.get("result") == "ok", "server still works after its log was trimmed")
p.send_signal(signal.SIGTERM); p.wait(10); shutil.rmtree(d)
print("\nALL PASSED" if not fails else "\nFAILURES: %d" % fails); sys.exit(1 if fails else 0)
