# Runs the real subcon binary against a fake olli (the one in tools/mcp_bridge/tests) on test ports, with a prepared
# note queue, and checks peek_subcon_notes: right content, same order as a pull, and it uses nothing up.
# From tools/subcon/:  python3 -I tests/test_peek_tool.py        (needs ./subcon built; no network, no model)
import json, os, signal, subprocess, sys, tempfile, time, shutil
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "mcp_bridge", "tests"))
from fake_olli import Endpoint
SUBCON = os.path.join(os.path.dirname(HERE), "subcon")
fails = 0
def check(cond, msg):
    global fails
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond: fails += 1

d = tempfile.mkdtemp(prefix="subcon_peek_test_")
os.makedirs(os.path.join(d, "subcon"))
now = int(time.time())
queue = [
    {"content": "Storms are coming tonight.", "label": "Weather", "urgency": "normal", "created_unix": now - 3 * 3600, "delivered": False, "expired": False},
    {"content": "A timer finished and nobody noticed.", "label": "Timers", "urgency": "urgent", "created_unix": now - 600, "delivered": False, "expired": False},
    {"content": "Already told.", "label": "Notes", "urgency": "normal", "created_unix": now - 7200, "delivered": True, "expired": False},
    {"content": "Far too old to be pending.", "label": "Conversations", "urgency": "low", "created_unix": now - 40 * 3600, "delivered": False, "expired": False},
]
json.dump(queue, open(os.path.join(d, "subcon", "queue.json"), "w"))
olli = Endpoint(47791, "olli")
p = subprocess.Popen([SUBCON, "--dir", d, "--olli-port", "47791", "--listen-port", "47793", "--no-think-cycle"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def call(name, cid, args=None):
    olli.send({"type": "call", "call_id": cid, "name": name, "arguments": args or {}})
    return olli.wait(lambda m: m.get("type") == "result" and m.get("call_id") == cid, 15)
try:
    end = time.time() + 30
    while time.time() < end and not olli.registers: olli.pump(0.1)
    check(bool(olli.registers), "subcon registered with the fake olli")
    names = olli.tool_names()
    check("peek_subcon_notes" in names and "get_subcon_notes" in names and "count_subcon_notes" in names, "peek_subcon_notes is registered next to the others: %s" % names)
    time.sleep(1)

    r = call("peek_subcon_notes", "p1")
    peek = json.loads(r["result"]) if r else {}
    check(peek.get("pending") == 2, "pending counts the 2 live notes (not the delivered one, not the 40-hour-old one): %s" % peek.get("pending"))
    check([n["label"] for n in peek.get("notes", [])] == ["Timers", "Weather"], "urgent first, then oldest first (the order a pull uses)")
    check(peek["notes"][0]["urgency"] == "urgent" and peek["notes"][0]["age_minutes"] in (10, 11) and "timer" in peek["notes"][0]["summary"], "label, urgency, age and the start of the text")
    check("special_instruction" in r and "get_subcon_notes" in r["special_instruction"], "tells a model reading it that nothing was told yet")

    r2 = call("peek_subcon_notes", "p2")
    check(json.loads(r2["result"])["pending"] == 2, "a second peek still sees both - peeking uses nothing up")
    r3 = call("count_subcon_notes", "c1")
    check("2 subcon notes" in r3["result"], "count agrees: %s" % r3["result"][:40])
    saved = json.load(open(os.path.join(d, "subcon", "queue.json")))
    check(sum(1 for n in saved if not n["delivered"]) == 2 or sum(1 for n in saved if not n["delivered"]) == 2, "queue.json still has 2 pending")

    r4 = call("get_subcon_notes", "g1")
    check("timer" in r4["result"].lower() and "Storms" in r4["result"], "a real pull hands both over")
    r5 = call("peek_subcon_notes", "p3")
    check(json.loads(r5["result"]) == {"pending": 0, "notes": []}, "after the pull a peek says nothing is waiting")
finally:
    p.send_signal(signal.SIGTERM)
    try: p.wait(10)
    except Exception: p.kill()
    shutil.rmtree(d, ignore_errors=True)
print("\n%s" % ("ALL OK" if fails == 0 else "%d FAILED" % fails))
sys.exit(1 if fails else 0)
