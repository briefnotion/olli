# The bridge learns its profile from olli's "identity" message (like the other tools): vacant until olli names a
# profile, loads that profile's mcp_bridge.json, switches when olli comes back as another profile, and unloads every
# MCP server when olli goes away. --dir / --profile pin a folder instead. No network needed (python3 only).
import json, os, subprocess, sys, time, signal, shutil, tempfile, threading
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fake_olli import Endpoint
ROOT = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(os.path.dirname(ROOT), "mcp_bridge")
ROGUE = os.path.join(ROOT, "rogue_server.py")
SLOW = os.path.join(ROOT, "slow_server.py")
fails = 0
def check(cond, msg):
    global fails
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond: fails += 1

home = tempfile.mkdtemp(prefix="mcp_bridge_home_")
def write_profile(name, config):
    d = os.path.join(home, "olli_files_" + name if name else "olli_files")
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, "mcp_bridge.json")
    open(path, "w").write(json.dumps(config)); os.chmod(path, 0o600)
    return d
def rogue_cfg(server, tag):
    return {"call_timeout_seconds": 100, "servers": {server: {"command": ["python3", "-I", ROGUE, "rogue", "tag_" + tag], "allow_tools": ["echo"]}}}
def running(tag):
    r = subprocess.run(["pgrep", "-fc", "rogue_server.py rogue tag_" + tag], capture_output=True, text=True)
    return int(r.stdout.strip() or 0)
def wait_until(pred, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        for ep in (olli, subcon): ep.pump(0.05)
        if pred(): return True
    return False
def start(extra=()):
    env = dict(os.environ, HOME=home)
    p = subprocess.Popen([BRIDGE, "--olli-port", "47701", "--subcon-port", "47703", *extra], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1, env=env)
    lines = []
    threading.Thread(target=lambda: [lines.append(l.rstrip()) for l in p.stdout], daemon=True).start()
    return p, lines
def call(ep, name, args, cid, timeout=20):
    ep.send({"type": "call", "call_id": cid, "name": name, "arguments": args})
    return ep.wait(lambda m: m.get("type") == "result" and m.get("call_id") == cid, timeout)
def identity(name):
    olli.send({"type": "identity", "name": name, "full_name": "", "about": ""})
def names(ep): return sorted(ep.tool_names())
BASE = sorted(["mcp_status", "mcp_set_server_enabled"])

write_profile("alpha", rogue_cfg("aaa", "alpha"))
write_profile("beta", rogue_cfg("bbb", "beta"))
write_profile("gamma", {"call_timeout_seconds": 100, "servers": {"slow": {"command": ["python3", "-I", SLOW, "tag_gamma"], "allow_tools": ["*"]}}})
write_profile("", rogue_cfg("shr", "shared"))

olli = Endpoint(47701, "olli"); subcon = Endpoint(47703, "subcon")

print("== starts empty, no profile, no servers")
p, out = start()
check(wait_until(lambda: olli.registers and subcon.registers, 20), "connected to olli and subcon")
check(names(olli) == BASE and names(subcon) == BASE, "only the bridge's own two tools registered before any identity: %s" % names(olli))
r = call(olli, "mcp_status", {}, "s0"); check(r and "no profile" in r["result"], "mcp_status says no profile is loaded: %s" % (r and r["result"][:80]))
r = call(olli, "mcp_set_server_enabled", {"server": "aaa", "enabled": True}, "s1"); check(r is not None and "error" in r and "No profile" in r["error"], "mcp_set_server_enabled says no profile while vacant: %s" % (r and r.get("error", "")[:60]))
check(running("alpha") == 0 and running("beta") == 0, "no MCP server started")

print("== identity 'alpha' loads alpha's servers")
connects = olli.connects
identity("alpha")
check(wait_until(lambda: "mcp_aaa_echo" in names(olli) and "mcp_aaa_echo" in names(subcon), 40), "alpha's tool offered on both links: %s" % names(olli))
check(running("alpha") == 1, "alpha's server is running")
r = call(olli, "mcp_aaa_echo", {"m": "hi"}, "a1"); check(r and "result" in r, "a call to alpha's tool works")
r = call(olli, "mcp_status", {}, "s2"); check(r and "profile alpha" in r["result"], "mcp_status names the profile: %s" % (r and r["result"][:60]))

print("== olli reconnecting after the bridge's OWN re-registration does not unload anything")
# the bridge dropped olli on purpose to announce the list; the fake olli sees a reconnect and says who it is again
if olli.conn is None or olli.connects > connects:
    pass
identity("alpha") if olli.conn else None
time.sleep(3)
for ep in (olli, subcon): ep.pump(0.1)
check(running("alpha") == 1 and not any("no longer connected" in l for l in out), "still loaded after the identity repeat (own re-registration is not 'olli went away')")
check("mcp_aaa_echo" in names(olli), "alpha's tool still offered")

print("== olli goes away: vacant, servers stopped")
olli.conn.close(); olli.conn = None
check(wait_until(lambda: any("no longer connected" in l for l in out), 20), "bridge noticed olli went away")
check(wait_until(lambda: running("alpha") == 0, 15), "alpha's server was stopped")
check(wait_until(lambda: subcon.registers and names(subcon) == BASE, 20), "subcon link back to only the two own tools: %s" % names(subcon))
r = call(subcon, "mcp_status", {}, "s3"); check(r and "no profile" in r["result"], "mcp_status (via subcon) says no profile")
r = call(subcon, "mcp_aaa_echo", {"m": "x"}, "a2", 10); check(r is not None and "error" in r, "alpha's tool is gone: %s" % (r and r.get("error", "")[:60]))

print("== olli comes back as another profile")
olli.registers.clear()
check(wait_until(lambda: olli.conn is not None and olli.registers, 30), "bridge reconnected to olli by itself")
check(names(olli) == BASE, "re-registered with only its own tools first: %s" % names(olli))
identity("beta")
check(wait_until(lambda: "mcp_bbb_echo" in names(olli), 40), "beta's tool offered: %s" % names(olli))
check(running("beta") == 1 and running("alpha") == 0, "only beta's server runs")
check("mcp_aaa_echo" not in names(olli), "nothing of alpha is left")

print("== bad identity messages are ignored")
for bad in (123, ["x"], "../evil", "a" * 200, "has space", None):
    olli.send({"type": "identity", "name": bad})
# the hostile ones from test_chaos.py: a name nested 50,000 levels deep, a 1 MB name, a name that is an object
olli.conn.sendall(b'{"type":"identity","name":' + b"[" * 50000 + b"]" * 50000 + b"}\n")
olli.conn.sendall(b'{"type":"identity","name":"' + b"a" * 1000000 + b'"}\n')
olli.conn.sendall(b'{"type":"identity","name":{"x":1}}\n')
time.sleep(2)
for ep in (olli, subcon): ep.pump(0.1)
check(p.poll() is None and running("beta") == 1, "bridge alive, beta still loaded")
check(any("will not use" in l for l in out), "bad names reported in the log")

print("== a call still waiting when olli goes away gets an error on the other link")
olli.conn.close(); olli.conn = None
check(wait_until(lambda: running("beta") == 0, 20), "beta unloaded")
olli.registers.clear()
check(wait_until(lambda: olli.conn is not None and olli.registers, 30), "olli back")
identity("gamma")
check(wait_until(lambda: "mcp_slow_nap" in names(subcon) and "mcp_slow_nap" in names(olli), 40), "gamma's slow tool offered")
subcon.send({"type": "call", "call_id": "nap1", "name": "mcp_slow_nap", "arguments": {}})
time.sleep(2)
olli.conn.close(); olli.conn = None
m = subcon.wait(lambda m: m.get("call_id") == "nap1", 20)
check(m is not None and "unloaded" in m.get("error", ""), "pending call answered with an error when the profile unloaded: %s" % (m and m.get("error", "")[:70]))

print("== the shared default profile (name '')")
olli.registers.clear()
check(wait_until(lambda: olli.conn is not None and olli.registers, 30), "olli back")
identity("")
check(wait_until(lambda: "mcp_shr_echo" in names(olli), 40), "shared profile's tool offered: %s" % names(olli))
olli.conn.close(); olli.conn = None
check(wait_until(lambda: running("shared") == 0, 20), "unloaded again")

print("== a profile with a bad config stays vacant, the next good one loads")
bad = write_profile("broken", {})
os.chmod(os.path.join(bad, "mcp_bridge.json"), 0o666)
olli.registers.clear()
check(wait_until(lambda: olli.conn is not None and olli.registers, 30), "olli back")
identity("broken")
check(wait_until(lambda: any("cannot use profile broken" in l for l in out), 10), "bad config reported, bridge alive: %s" % [l[:100] for l in out if "broken" in l][-1:])
identity("alpha")
check(wait_until(lambda: "mcp_aaa_echo" in names(olli), 40), "a good profile loads afterwards")
p.send_signal(signal.SIGTERM); p.wait(15)
check(running("alpha") == 0, "SIGTERM leaves no server running")

print("== pinned (--dir): identity and olli dropping change nothing")
pin = os.path.join(home, "olli_files_alpha")
olli.registers.clear(); subcon.registers.clear()
if olli.conn: olli.conn.close(); olli.conn = None
p, out = start(["--dir", pin])
check(wait_until(lambda: olli.registers and "mcp_aaa_echo" in names(olli), 40), "pinned bridge offers its folder's tools on the first registration: %s" % names(olli))
n_reg = len(olli.registers)
identity("beta")
time.sleep(3)
for ep in (olli, subcon): ep.pump(0.1)
check(running("alpha") == 1 and "mcp_bbb_echo" not in names(olli), "identity ignored")
olli.conn.close(); olli.conn = None
olli.registers.clear()
check(wait_until(lambda: olli.registers, 30), "bridge reconnected")
check(running("alpha") == 1 and "mcp_aaa_echo" in names(olli), "servers stayed loaded while olli was gone")
p.send_signal(signal.SIGTERM); p.wait(15)

shutil.rmtree(home, ignore_errors=True)
print("\n%s" % ("ALL OK" if fails == 0 else "%d FAILED" % fails))
sys.exit(1 if fails else 0)
