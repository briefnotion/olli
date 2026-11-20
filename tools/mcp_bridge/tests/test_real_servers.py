import json, os, subprocess, sys, time, signal, shutil, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fake_olli import Endpoint

ROOT = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "mcp_bridge")
fails = 0
def check(cond, msg):
    global fails
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond: fails += 1

work = tempfile.mkdtemp(prefix="mcp_bridge_test_")
cfg = {
  "call_timeout_seconds": 6,
  "servers": {
    "everything": {"command": ["npx", "-y", "@modelcontextprotocol/server-everything@2.3.1"],
                   "allow_tools": ["echo", "get-sum", "get-tiny-image", "get-structured-content", "trigger-long-running-operation"]},
    "time": {"command": ["uvx", "mcp-server-time==2026.11.18"], "allow_tools": ["*"]},
    "broken": {"command": ["/no/such/binary"], "allow_tools": ["*"]},
  }
}
cfg_path = os.path.join(work, "mcp_bridge.json")
json.dump(cfg, open(cfg_path, "w")); os.chmod(cfg_path, 0o600)

olli = Endpoint(47701, "olli"); subcon = Endpoint(47703, "subcon")
proc = subprocess.Popen([BRIDGE, "--dir", work, "--olli-port", "47701", "--subcon-port", "47703"],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
import threading
bridge_out = []
threading.Thread(target=lambda: [bridge_out.append(l.rstrip()) for l in proc.stdout], daemon=True).start()

def both(timeout=60):
    end = time.time() + timeout
    while time.time() < end and not (olli.registers and subcon.registers):
        olli.pump(0.1); subcon.pump(0.1)
def call(ep, name, args, cid, timeout=20):
    ep.send({"type": "call", "call_id": cid, "name": name, "arguments": args})
    return ep.wait(lambda m: m.get("type") == "result" and m.get("call_id") == cid, timeout)

print("== registration")
both(90)
names = olli.tool_names()
print("   tools:", names)
check(names[0] == "mcp_status" and "mcp_set_server_enabled" in names, "management tools first")
check("mcp_everything_echo" in names and "mcp_everything_get_sum" in names, "allowed everything tools offered")
check("mcp_everything_get_env" not in names and "mcp_everything_gzip_file_as_resource" not in names, "tools not on the allow-list are NOT offered")
check("mcp_time_get_current_time" in names and "mcp_time_convert_time" in names, "time server tools offered (allow *)")
check(olli.registers[-1] == subcon.registers[-1], "same register message on both links")
check(olli.connects == 1, "registered once (startup settle: one connection, not several) - connects=%d" % olli.connects)
echo = [t for t in olli.registers[-1]["tools"] if t["name"] == "mcp_everything_echo"][0]
print("   echo def:", json.dumps(echo)[:300])
check(echo["description"].startswith("[MCP server 'everything']") and echo["parameters"]["required"] == ["message"] and "$schema" not in echo["parameters"], "tool definition converted")
check(any("not start" in l or "could not start" in l for l in bridge_out), "broken server reported in the log")

print("== calls")
r = call(olli, "mcp_everything_echo", {"message": "hello from olli"}, "c1")
check(r and r.get("result") == "Echo: hello from olli" and "special_instruction" in r, "echo via olli link: %s" % (r,))
r = call(subcon, "mcp_everything_get_sum", {"a": 20, "b": 22}, "c2")
check(r and "42" in r.get("result", ""), "get-sum via SUBCON link answered on subcon link: %s" % (r,))
r = call(olli, "mcp_everything_get_tiny_image", {}, "c3")
check(r and "image (image/png)" in r.get("result", "") and "iVBOR" not in r["result"], "image block -> short note, no base64: %s" % (r and r.get("result")[:120],))
r = call(olli, "mcp_everything_get_structured_content", {"location": "New York"}, "c4")
check(r and "temperature" in r.get("result", ""), "structured content: %s" % (r and r.get("result"),))
r = call(olli, "mcp_time_get_current_time", {"timezone": "Asia/Tokyo"}, "c5", 60)
check(r and "Asia/Tokyo" in r.get("result", ""), "time server: %s" % (r and r.get("result")[:150].replace("\n", " "),))
r = call(olli, "mcp_everything_echo", {}, "c6")
check(r and "error" in r and "message" in r["error"], "bad arguments -> error from the server: %s" % (r and r.get("error")[:150],))
r = call(olli, "no_such_tool", {}, "c7")
check(r and "Unknown tool" in r.get("error", ""), "unknown tool -> error")

print("== slow call keeps the heartbeat going (pings before: olli=%d)" % olli.pings)
pings_before = olli.pings
olli.send({"type": "call", "call_id": "slow", "name": "mcp_everything_trigger_long_running_operation", "arguments": {"duration": 5, "steps": 1}})
olli.send({"type": "call", "call_id": "fast", "name": "mcp_everything_get_sum", "arguments": {"a": 1, "b": 1}})
got = {}
t0 = time.time()
while time.time() - t0 < 12 and len(got) < 2:
    for m in olli.pump(0.2):
        if m.get("type") == "result": got[m["call_id"]] = (m, time.time() - t0)
check("fast" in got and got["fast"][1] < 2, "fast call answered while the slow one is pending (%.1fs)" % got.get("fast", (0, 99))[1])
check("slow" in got and 4 < got["slow"][1] < 8, "slow call answered when done (%.1fs)" % got.get("slow", (0, 99))[1])
check(olli.connects == 1, "no reconnect during a slow call")

print("== call timeout (config: 6 s; the call takes 12 s)")
t0 = time.time()
r = call(olli, "mcp_everything_trigger_long_running_operation", {"duration": 12, "steps": 1}, "to", 20)
check(r and "did not answer within 6 seconds" in r.get("error", "") and 5 < time.time() - t0 < 9, "timeout error after ~6 s: %s" % (r and r.get("error"),))

print("== mcp_status")
r = call(olli, "mcp_status", {}, "st")
print("\n".join("   | " + l for l in r["result"].splitlines()))
check("everything: ready" in r["result"] and "time: ready" in r["result"] and "broken: failed" in r["result"] and "not allowed" in r["result"], "status text")

print("== disable / enable (re-registration)")
connects = olli.connects
r = call(olli, "mcp_set_server_enabled", {"server": "everything", "enabled": False}, "d1")
check(r and "off" in r.get("result", ""), "disable answered: %s" % (r and r.get("result"),))
# the bridge drops the link and registers again with a shorter list
end = time.time() + 15
while time.time() < end and not (olli.connects > connects and olli.registers and "mcp_everything_echo" not in olli.tool_names() and olli.conn):
    olli.pump(0.2); subcon.pump(0.05)
check(olli.connects == connects + 1 and "mcp_everything_echo" not in olli.tool_names() and "mcp_time_convert_time" in olli.tool_names(), "re-registered without 'everything': %s" % olli.tool_names())
check(json.load(open(cfg_path))["servers"]["everything"].get("enabled") is False, "enabled:false saved in the config file")
check(oct(os.stat(cfg_path).st_mode & 0o777) == "0o600", "config file still 0600")
r = call(olli, "mcp_set_server_enabled", {"server": "nonexistent", "enabled": True}, "d2")
check(r and "no MCP server called" in r.get("error", ""), "unknown server -> error")
r = call(olli, "mcp_set_server_enabled", {"server": "everything", "enabled": True}, "d3")
end = time.time() + 40
while time.time() < end and "mcp_everything_echo" not in olli.tool_names():
    olli.pump(0.2); subcon.pump(0.05)
check("mcp_everything_echo" in olli.tool_names(), "enabling brings its tools back")

print("== server dies mid-call")
olli.send({"type": "call", "call_id": "doomed", "name": "mcp_everything_trigger_long_running_operation", "arguments": {"duration": 20, "steps": 1}})
time.sleep(1); subprocess.run(["pkill", "-KILL", "-f", "server-everything"])
m = olli.wait(lambda m: m.get("type") == "result" and m.get("call_id") == "doomed", 10)
check(m and "error" in m and "stopped" in m["error"], "in-flight call gets an error: %s" % (m and m.get("error")[:160],))
r = call(olli, "mcp_everything_echo", {"message": "x"}, "after", 40)
for _ in range(100):
    if r and "result" in r: break
    time.sleep(0.5); r = call(olli, "mcp_everything_echo", {"message": "x"}, "after", 10)
check(r and r.get("result") == "Echo: x", "server restarted by itself and works again: %s" % (r,))

print("== shutdown")
def _descendants(pid):
    out, todo = [], [pid]
    while todo:
        kids = subprocess.run(["pgrep", "-P", str(todo.pop())], capture_output=True, text=True).stdout.split()
        out += kids; todo += kids
    return out
_my_children = _descendants(proc.pid)   # this bridge's own server processes - another bridge on the machine may run the same servers
proc.send_signal(signal.SIGTERM)
try: proc.wait(10); check(True, "bridge exited on SIGTERM (code %s)" % proc.returncode)
except subprocess.TimeoutExpired: check(False, "bridge did not exit"); proc.kill()
time.sleep(1)
left = [p for p in _my_children if os.path.exists("/proc/%s" % p) and open("/proc/%s/stat" % p).read().split(")")[-1].split()[0] != "Z"]
check(not left, "none of this bridge's %d server processes is left behind %s" % (len(_my_children), left[:5]))
print("\n--- bridge log ---"); print("\n".join(bridge_out[-40:]))
shutil.rmtree(work, ignore_errors=True)
print("\nALL PASSED" if not fails else "\nFAILURES: %d" % fails)
sys.exit(1 if fails else 0)
