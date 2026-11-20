# Chaos test: tries to break the bridge with concurrency, kills, hostile input and floods. Real MCP servers
# (server-everything, mcp-server-time, server-filesystem), a fake olli + fake subcon on test ports 47701/47703.
# Needs npx, uvx and internet on the first run. Takes about 8 minutes. See README.md in this folder.
import json, os, random, shutil, signal, subprocess, sys, tempfile, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fake_olli import Endpoint

BRIDGE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "mcp_bridge")
fails = 0
def check(cond, msg):
    global fails
    print(("  ok   " if cond else "  FAIL ") + msg, flush=True)
    if not cond: fails += 1

work = tempfile.mkdtemp(prefix="mcp_chaos_")
notes = os.path.join(work, "notes"); os.makedirs(notes)
open(os.path.join(notes, "small.txt"), "w").write("hello from the notes folder\n")
open(os.path.join(notes, "big.txt"), "w").write(("0123456789abcdef" * 64 + "\n") * 5000)          # about 5 MB
open(os.path.join(notes, "binary.txt"), "wb").write(b"before \xff\xfe\xfd after \xc3\x28 end\n")      # invalid UTF-8
cfg = {"call_timeout_seconds": 60, "servers": {
    "everything": {"command": ["npx", "-y", "@modelcontextprotocol/server-everything@2.3.1"], "allow_tools": ["echo", "get-sum", "trigger-long-running-operation"]},
    "time": {"command": ["uvx", "mcp-server-time==2026.11.18"], "allow_tools": ["*"]},
    "files": {"command": ["npx", "-y", "@modelcontextprotocol/server-filesystem@2.4.0", notes], "allow_tools": ["read_text_file", "list_directory"]}}}
cfg_path = os.path.join(work, "mcp_bridge.json"); json.dump(cfg, open(cfg_path, "w")); os.chmod(cfg_path, 0o600)

olli = Endpoint(47701, "olli"); subcon = Endpoint(47703, "subcon")
proc = subprocess.Popen([BRIDGE, "--dir", work, "--olli-port", "47701", "--subcon-port", "47703"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
log = []
threading.Thread(target=lambda: [log.append(l.rstrip()) for l in proc.stdout], daemon=True).start()

def pump_both(t=0.1):
    out = []
    for ep in (olli, subcon):
        if ep.conn is not None or True: out += [(ep, m) for m in ep.pump(t / 2)]
    return out

def stats():
    try:
        rss = int([l for l in open(f"/proc/{proc.pid}/status") if l.startswith("VmRSS")][0].split()[1])
        fds = len(os.listdir(f"/proc/{proc.pid}/fd"))
    except Exception:
        return None
    kids = subprocess.run(["pgrep", "-P", str(proc.pid)], capture_output=True, text=True).stdout.split()
    return rss, fds, len(kids)

def wait_registered(timeout=90):
    end = time.time() + timeout
    while time.time() < end and not (olli.registers and subcon.registers and olli.conn and subcon.conn): pump_both()
    return bool(olli.registers and subcon.registers)

def run_calls(plan, timeout=60):
    """plan: list of (endpoint, call_id, tool, arguments). Sends everything at once, collects every result. Returns
    (results {(ep.name, call_id): [messages]}, unanswered keys)."""
    results = {}
    for ep, cid, tool, args in plan:
        ep.send({"type": "call", "call_id": cid, "name": tool, "arguments": args})
    want = {(ep.name, cid) for ep, cid, _, _ in plan}
    end = time.time() + timeout
    while time.time() < end and not want <= set(results):
        for ep, m in pump_both(0.2):
            if m.get("type") == "result": results.setdefault((ep.name, m.get("call_id")), []).append(m)
    return results, want - set(results)

def alive(): return proc.poll() is None

print("== start: registration", flush=True)
check(wait_registered(), "bridge registered with fake olli and subcon: %s" % olli.tool_names())
time.sleep(2); base = stats(); print("   baseline rss/fds/children:", base, flush=True)

# ---------------------------------------------------------------------------------------------
print("== A. 400 concurrent calls over both links, the SAME call ids reused on each link", flush=True)
plan, expect = [], {}
for i in range(400):
    ep = olli if i % 2 else subcon
    cid = "c%d" % (i // 2)                                   # c0..c199 exists on BOTH links, different payloads
    if i % 3 == 0:
        a, b = random.randint(0, 999), random.randint(0, 999)
        plan.append((ep, cid, "mcp_everything_get_sum", {"a": a, "b": b})); expect[(ep.name, cid)] = ("sum", a, b)
    else:
        msg = "%s-%d-%d" % (ep.name, i, random.randint(0, 10**6))
        plan.append((ep, cid, "mcp_everything_echo", {"message": msg})); expect[(ep.name, cid)] = ("echo", msg)
t0 = time.time(); results, missing = run_calls(plan, 90)
bad = 0
for key, kind in expect.items():
    got = results.get(key, [])
    ok = len(got) == 1 and "result" in got[0]
    if ok:
        text = got[0]["result"]
        ok = (text == "Echo: " + kind[1]) if kind[0] == "echo" else (text == "The sum of %d and %d is %d." % (kind[1], kind[2], kind[1] + kind[2]))
    bad += 0 if ok else 1
check(not missing and bad == 0, "400/400 answered exactly once, each with ITS OWN payload on ITS OWN link (%d wrong/missing, %.1fs)" % (bad + len(missing), time.time() - t0))

# ---------------------------------------------------------------------------------------------
print("== B. slow calls in flight while fast ones keep flowing", flush=True)
plan = [(olli, "slow%d" % i, "mcp_everything_trigger_long_running_operation", {"duration": 4, "steps": 1}) for i in range(3)]
fast = [(subcon if i % 2 else olli, "f%d" % i, "mcp_everything_echo", {"message": "fast%d" % i}) for i in range(100)]
for ep, cid, tool, args in plan: ep.send({"type": "call", "call_id": cid, "name": tool, "arguments": args})
t0 = time.time(); res_fast, miss_fast = run_calls(fast, 30); t_fast = time.time() - t0
check(not miss_fast and t_fast < 3.5, "100 fast calls finished in %.1fs while 3 slow ones (4 s) were pending" % t_fast)
got_slow = {}
end = time.time() + 15
while time.time() < end and len(got_slow) < 3:
    for ep, m in pump_both(0.2):
        if m.get("type") == "result" and str(m.get("call_id", "")).startswith("slow"): got_slow[m["call_id"]] = m
check(len(got_slow) == 3 and all("result" in m for m in got_slow.values()), "all 3 slow calls came back with results")

# ---------------------------------------------------------------------------------------------
print("== C. kill a server with 20 calls in flight", flush=True)
plan = [(olli if i % 2 else subcon, "k%d" % i, "mcp_everything_trigger_long_running_operation", {"duration": 20, "steps": 1}) for i in range(20)]
for ep, cid, tool, args in plan: ep.send({"type": "call", "call_id": cid, "name": tool, "arguments": args})
time.sleep(1.5)
subprocess.run(["pkill", "-KILL", "-f", "[s]erver-everything@2.3.1"])
results = {}; end = time.time() + 15
while time.time() < end and len(results) < 20:
    for ep, m in pump_both(0.2):
        if m.get("type") == "result": results.setdefault((ep.name, m["call_id"]), []).append(m)
check(len(results) == 20 and all(len(v) == 1 and "error" in v[0] for v in results.values()), "every in-flight call got exactly one error (%d answered)" % len(results))
end = time.time() + 40; ok = False
while time.time() < end and not ok:
    r, m = run_calls([(olli, "after%d" % int(time.time()), "mcp_everything_echo", {"message": "back"})], 6)
    ok = any("result" in v[0] for v in r.values()); time.sleep(1)
check(ok, "the killed server restarted by itself and works again")

# ---------------------------------------------------------------------------------------------
print("== D. olli's connection dropped three times with 30 slow calls pending", flush=True)
for rnd in range(3):
    for i in range(30): olli.send({"type": "call", "call_id": "old%d_%d" % (rnd, i), "name": "mcp_everything_trigger_long_running_operation", "arguments": {"duration": 6, "steps": 1}})
    time.sleep(0.5)
    olli.conn.close(); olli.conn = None; olli.registers.clear()
    check(wait_registered(30), "round %d: bridge reconnected and registered again" % (rnd + 1))
late = []
end = time.time() + 12
while time.time() < end:
    for ep, m in pump_both(0.2):
        if ep is olli and str(m.get("call_id", "")).startswith("old"): late.append(m)
check(not late, "answers for calls from the dead connections were NOT sent on the new ones (%d stray)" % len(late))
r, m = run_calls([(olli, "post%d" % i, "mcp_everything_echo", {"message": "post%d" % i}) for i in range(20)], 20)
check(not m and all(v[0].get("result") == "Echo: post%d" % int(k[1][4:]) for k, v in r.items()), "20 fresh calls after the storm all correct")
check(alive(), "bridge still running")

# ---------------------------------------------------------------------------------------------
print("== E. hostile and malformed messages FROM olli", flush=True)
raw = [b"not json at all\n", b'{"type":5}\n', b'{"type":"call","call_id":7,"name":"x"}\n', b'{"type":"call","call_id":"m1","name":null}\n',
       b'{"type":"call","call_id":"m2","name":"mcp_everything_echo","arguments":[1,2]}\n', b"[]\n", b'""\n', b'{"type":"call"}\n',
       b"\xff\xfe\xfd\n", b"\n\n\n", b'{"type":"call","call_id":"' + b"x" * 200000 + b'","name":"mcp_status"}\n',
       b'{"type":"call","call_id":"m3","name":"mcp_everything_echo","arguments":{"message":"' + b"y" * 300000 + b'"}}\n',
       b'{"type":"call","call_id":"m4","name":"mcp_set_server_enabled","arguments":{"server":5,"enabled":"yes"}}\n',
       b'{"type":"call","call_id":"m5","name":"mcp_everything_echo","arguments":{"message":' + b"[" * 50000 + b"]" * 50000 + b"}}\n",
       b'{"type":"identity","name":' + b"[" * 50000 + b"]" * 50000 + b"}\n"]
for line in raw:
    try: olli.conn.sendall(line)
    except Exception as e: print("   (send failed:", e, ")")
    pump_both(0.3)
time.sleep(2)
for ep, m in pump_both(1): pass
check(alive(), "bridge survived 15 hostile messages (garbage, wrong types, 200 KB call id, 300 KB arguments, 50,000-level nesting)")
if not alive(): print("   EXIT CODE:", proc.returncode)
if alive():
    if olli.conn is None: wait_registered(30)
    r, m = run_calls([(olli, "after_e", "mcp_everything_echo", {"message": "still fine"})], 15)
    check(not m and r[("olli", "after_e")][0].get("result") == "Echo: still fine", "and answers a normal call correctly afterwards")

# ---------------------------------------------------------------------------------------------
print("== F. flood: 2000 calls in one burst", flush=True)
if alive():
    plan = [(olli if i % 2 else subcon, "fl%d" % (i // 2), "mcp_everything_echo", {"message": "fl%d" % i}) for i in range(2000)]
    t0 = time.time(); results, missing = run_calls(plan, 120)
    wrong = sum(1 for (epn, cid), v in results.items() if len(v) != 1 or "result" not in v[0])
    check(not missing and wrong == 0, "2000/2000 answered once each (%d missing, %d wrong, %.1fs)" % (len(missing), wrong, time.time() - t0))

# ---------------------------------------------------------------------------------------------
print("== G. a server switched off and on 6 times while calls keep flying", flush=True)
if alive():
    answered = errors = lost = 0
    for rnd in range(6):
        en = bool(rnd % 2)                                   # off, on, off, on, ...
        olli.send({"type": "call", "call_id": "tg%d" % rnd, "name": "mcp_set_server_enabled", "arguments": {"server": "time", "enabled": en}})
        end = time.time() + 6; sent = 0
        while time.time() < end:
            if olli.conn is not None and sent < 20:
                try: olli.send({"type": "call", "call_id": "tc%d_%d" % (rnd, sent), "name": "mcp_time_get_current_time", "arguments": {"timezone": "UTC"}}); sent += 1
                except Exception: pass
            for ep, m in pump_both(0.2):
                if m.get("type") == "result" and str(m.get("call_id", "")).startswith("tc"): answered += 1; errors += 1 if "error" in m else 0
        wait_registered(20)
    check(alive(), "bridge survived the on/off churn (%d calls answered, %d of them errors while off)" % (answered, errors))
    wait_registered(40)
    r, m = run_calls([(olli, "final_status", "mcp_status", {})], 15)
    status = r.get(("olli", "final_status"), [{}])[0].get("result", "")
    check("time: ready" in status, "final state is consistent: 'time' is running again:  %s" % status.replace("\n", " | ")[:200])

# ---------------------------------------------------------------------------------------------
print("== H. big and invalid-UTF-8 results from the filesystem server", flush=True)
if alive():
    wait_registered(30)
    r, m = run_calls([(olli, "big", "mcp_files_read_text_file", {"path": os.path.join(notes, "big.txt")}),
                      (olli, "bin", "mcp_files_read_text_file", {"path": os.path.join(notes, "binary.txt")}),
                      (olli, "esc", "mcp_files_read_text_file", {"path": "/etc/passwd"}),
                      (olli, "trav", "mcp_files_read_text_file", {"path": os.path.join(notes, "..", "mcp_bridge.json")}),
                      (olli, "ok", "mcp_files_read_text_file", {"path": os.path.join(notes, "small.txt")})], 30)
    big = r.get(("olli", "big"), [{}])[0]
    check("error" in big and "too large" in big["error"], "5 MB file read (a >8 MB answer) -> an error for that call only: %s" % str(big.get("error", big))[:110])
    check(("olli", "bin") in r and ("result" in r[("olli", "bin")][0] or "error" in r[("olli", "bin")][0]), "invalid UTF-8 file -> a clean answer, no crash")
    check("error" in r.get(("olli", "esc"), [{}])[0] and "error" in r.get(("olli", "trav"), [{}])[0], "/etc/passwd and a ../ escape both refused by the server")
    same = r.get(("olli", "ok"), [{}])[0]
    check(same.get("result", "").startswith("hello from the notes") or "too large" in same.get("error", ""), "a small read sent at the same moment gets a clear answer, never silence (it may be cancelled with the big one: %s)" % str(same)[:70])
    r2, m2 = run_calls([(olli, "ok2", "mcp_files_read_text_file", {"path": os.path.join(notes, "small.txt")})], 15)
    check(r2.get(("olli", "ok2"), [{}])[0].get("result", "").startswith("hello from the notes"), "and a later read works too")
    check(not any("restarting" in l or "gave up" in l for l in log if "[files]" in l), "the files server was never restarted because of the big answer")

# ---------------------------------------------------------------------------------------------
print("== I. 3-minute soak (repeated mixed bursts), watching memory, descriptors and child processes", flush=True)
if alive():
    wait_registered(30); soak_end = time.time() + 180; rounds = 0; soak_bad = 0; samples = []
    while time.time() < soak_end and alive():
        plan = [(olli if i % 2 else subcon, "s%d_%d" % (rounds, i // 2), "mcp_everything_echo", {"message": "r%d_%d" % (rounds, i)}) for i in range(300)]
        results, missing = run_calls(plan, 60)
        soak_bad += len(missing) + sum(1 for v in results.values() if len(v) != 1 or "result" not in v[0])
        rounds += 1; s = stats()
        if s: samples.append(s)
        time.sleep(1)
    check(soak_bad == 0, "%d rounds x 300 calls: %d wrong or missing" % (rounds, soak_bad))
    if samples and base:
        mid = samples[len(samples) // 3][0]
        check(samples[-1][0] - mid < 8000, "memory plateaus: %.1f MB after the first third of the soak -> %.1f MB at the end (baseline %.1f MB)" % (mid / 1024, samples[-1][0] / 1024, base[0] / 1024))
        check(samples[-1][1] <= base[1] + 6, "file descriptors %d -> %d" % (base[1], samples[-1][1]))
        check(samples[-1][2] <= base[2] + 1, "child processes %d -> %d" % (base[2], samples[-1][2]))

print("== shutdown", flush=True)
def descendants(pid):
    out, todo = [], [pid]
    while todo:
        kids = subprocess.run(["pgrep", "-P", str(todo.pop())], capture_output=True, text=True).stdout.split()
        out += kids; todo += kids
    return out
my_children = descendants(proc.pid) if alive() else []
if alive():
    proc.send_signal(signal.SIGTERM)
    try: proc.wait(15); check(True, "bridge exited on SIGTERM (code %s)" % proc.returncode)
    except subprocess.TimeoutExpired: check(False, "bridge did not exit"); proc.kill()
else:
    check(False, "bridge died during the test (exit code %s)" % proc.returncode)
time.sleep(1)
left = [p for p in my_children if os.path.exists("/proc/%s" % p) and "Z" not in open("/proc/%s/stat" % p).read().split(")")[-1].split()[0]]
check(not left, "none of this bridge's %d server processes (and their children) is left behind %s" % (len(my_children), left[:5]))
print("\n--- bridge log (last 25 lines) ---"); print("\n".join(l[:180] for l in log[-25:]))
shutil.rmtree(work, ignore_errors=True)
print("\nALL PASSED" if not fails else "\nFAILURES: %d" % fails); sys.exit(1 if fails else 0)
