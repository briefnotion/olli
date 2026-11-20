import json, os, subprocess, sys, time, signal, shutil, tempfile, threading, stat
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
def start(work, extra=()):
    p = subprocess.Popen([BRIDGE, "--dir", work, "--olli-port", "47701", "--subcon-port", "47703", *extra], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
    lines = []
    threading.Thread(target=lambda: [lines.append(l.rstrip()) for l in p.stdout], daemon=True).start()
    return p, lines
def mkdir(cfg, mode=0o600, raw=None):
    d = tempfile.mkdtemp(prefix="mcp_bridge_test_")
    path = os.path.join(d, "mcp_bridge.json")
    open(path, "w").write(raw if raw is not None else json.dumps(cfg)); os.chmod(path, mode)
    return d
def call(ep, name, args, cid, timeout=20):
    ep.send({"type": "call", "call_id": cid, "name": name, "arguments": args})
    return ep.wait(lambda m: m.get("type") == "result" and m.get("call_id") == cid, timeout)
base = {"call_timeout_seconds": 100, "servers": {"rogue": {"command": ["python3", "-I", ROGUE, "rogue"], "allow_tools": ["echo", "exit"]}}}

print("== config problems")
d = mkdir(base, 0o666); p, out = start(d); p.wait(10)
check(p.returncode == 1 and any("anyone" in l for l in out), "world-writable config refused: %s" % out[-1:]); shutil.rmtree(d)
d = mkdir(None, raw="{ not json"); p, out = start(d); p.wait(10)
check(p.returncode == 1 and any("not valid JSON" in l for l in out), "invalid JSON -> clear message, exit 1: %s" % out[-1:]); shutil.rmtree(d)
d = tempfile.mkdtemp(prefix="mcp_bridge_test_"); p, out = start(d); time.sleep(2)
check(os.path.exists(d + "/mcp_bridge.json") and oct(os.stat(d + "/mcp_bridge.json").st_mode & 0o777) == "0o600" and json.load(open(d + "/mcp_bridge.json"))["servers"] == {}, "missing config -> default written, 0600")
p.send_signal(signal.SIGTERM); p.wait(10); shutil.rmtree(d)
d = mkdir({"servers": {"BadName": {"command": ["x"]}, "ok": {"command": []}}}); p, out = start(d); time.sleep(2)
check(sum(1 for l in out if l.startswith("[") and "config:" in l) == 2, "bad server entries skipped with a warning each: %s" % [l[:90] for l in out if "config:" in l])
p.send_signal(signal.SIGTERM); p.wait(10); shutil.rmtree(d)
r = subprocess.run([BRIDGE, "--help"], capture_output=True, text=True); check(r.returncode == 0 and "--profile" in r.stdout, "--help")
r = subprocess.run([BRIDGE, "--bogus"], capture_output=True, text=True); check(r.returncode == 1, "unknown option -> exit 1")

print("== bridge starts BEFORE olli, olli restarts, second copy refused")
d = mkdir(base); p, out = start(d)
time.sleep(4)   # nobody is listening yet
olli = Endpoint(47701, "olli"); subcon = Endpoint(47703, "subcon")
end = time.time() + 30
while time.time() < end and not (olli.registers and subcon.registers): olli.pump(0.1); subcon.pump(0.1)
check(bool(olli.registers) and bool(subcon.registers), "bridge connected and registered once olli/subcon came up")
check(olli.tool_names() == ["mcp_status", "mcp_set_server_enabled", "mcp_rogue_exit", "mcp_rogue_echo"] or sorted(olli.tool_names()) == sorted(["mcp_status", "mcp_set_server_enabled", "mcp_rogue_exit", "mcp_rogue_echo"]), "allow-list respected: %s" % olli.tool_names())
p2, out2 = start(d); p2.wait(10)
check(p2.returncode == 1 and any("already running" in l for l in out2), "second copy refused: %s" % out2[-1:])
check(p.poll() is None and not subprocess.run(["pgrep", "-fc", "^python3 -I /tmp.*rogue_server.py rogue"], capture_output=True, text=True).stdout.strip() == "2", "second copy did not start a second server")
# olli goes away and comes back with a fresh listener
connects = olli.connects
olli.conn.close(); olli.conn = None
olli.registers.clear()
end = time.time() + 20
while time.time() < end and not olli.registers: olli.pump(0.1)
check(bool(olli.registers) and olli.connects == connects + 1, "bridge reconnected by itself and registered again after olli dropped it")
r = call(olli, "mcp_rogue_echo", {"m": "hi"}, "z1"); check(r and "result" in r, "calls work after the reconnect")

print("== heartbeat during a call that lasts longer than olli's 15 s dead-timeout")
# a 'sleep' tool: the rogue server's deaf mode never answers a call - reuse 'sample' which waits on a reply we never give? use exit-less long call via python server mode 'slowcall'
p.send_signal(signal.SIGTERM); p.wait(10); shutil.rmtree(d)
d = mkdir({"call_timeout_seconds": 60, "servers": {"slow": {"command": ["python3", "-I", os.path.join(ROOT, "slow_server.py")], "allow_tools": ["*"]}}})
p, out = start(d)
end = time.time() + 30; olli.registers.clear(); subcon.registers.clear()
while time.time() < end and not olli.registers: olli.pump(0.1); subcon.pump(0.05)
connects = olli.connects; pings = olli.pings
t0 = time.time()
r = call(olli, "mcp_slow_nap", {}, "nap", 40)
dt = time.time() - t0
check(r and r.get("result") == "rested" and 21 < dt < 26, "22 s call answered (%.1fs)" % dt)
check(olli.connects == connects, "olli never saw the bridge drop during the call")
check(olli.pings - pings >= 3, "bridge kept pinging olli while the call was pending (%d pings)" % (olli.pings - pings))
p.send_signal(signal.SIGTERM); p.wait(10); shutil.rmtree(d)

print("== olli drops the link while a call is pending (nobody left to answer)")
d = mkdir({"call_timeout_seconds": 60, "servers": {"slow": {"command": ["python3", "-I", os.path.join(ROOT, "slow_server.py")], "allow_tools": ["*"]}}})
p, out = start(d)
end = time.time() + 30; olli.registers.clear()
while time.time() < end and not olli.registers: olli.pump(0.1); subcon.pump(0.05)
olli.send({"type": "call", "call_id": "gone", "name": "mcp_slow_nap", "arguments": {}}); time.sleep(0.5)
olli.conn.close(); olli.conn = None; olli.registers.clear()
end = time.time() + 20
while time.time() < end and not olli.registers: olli.pump(0.1)
check(bool(olli.registers), "bridge reconnects")
time.sleep(1)
r = call(olli, "mcp_status", {}, "st2", 10); check(r and "result" in r, "bridge responsive after the abandoned call")
time.sleep(24)
late = [m for m in olli.log if m.get("call_id") == "gone"]
check(not late, "the abandoned call's late answer was not sent on the new connection")
p.send_signal(signal.SIGTERM); p.wait(10); shutil.rmtree(d)
time.sleep(1)
left = subprocess.run(["pgrep", "-af", "^python3 -I /tmp.*(rogue_server|slow_server).py"], capture_output=True, text=True).stdout.strip()
check(left == "", "no server processes left behind " + left[:150])
print("\nALL PASSED" if not fails else "\nFAILURES: %d" % fails); sys.exit(1 if fails else 0)
