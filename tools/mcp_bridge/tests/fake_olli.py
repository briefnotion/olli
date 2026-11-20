import socket, select, json, time

class Endpoint:
    """A fake olli (or subcon): listens, accepts the bridge, speaks the newline-JSON protocol."""
    def __init__(self, port, name):
        self.name = name
        self.srv = socket.socket(); self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", port)); self.srv.listen(5); self.srv.setblocking(False)
        self.conn = None; self.buf = b""; self.log = []; self.registers = []; self.pings = 0; self.connects = 0
    def _accept(self):
        try:
            c, _ = self.srv.accept()
        except BlockingIOError:
            return
        if self.conn: self.conn.close()
        self.conn = c; c.setblocking(False); self.buf = b""; self.connects += 1
    def send(self, obj):
        self.conn.sendall((json.dumps(obj) + "\n").encode())
    def pump(self, timeout=0.2):
        rl = [self.srv] + ([self.conn] if self.conn else [])
        r, _, _ = select.select(rl, [], [], timeout)
        msgs = []
        if self.srv in r: self._accept()
        if self.conn and self.conn in r:
            try: data = self.conn.recv(65536)
            except BlockingIOError: data = None
            except (ConnectionResetError, BrokenPipeError): data = b""
            if data == b"":
                self.conn.close(); self.conn = None
                return [{"type": "_disconnected"}]
            if data: self.buf += data
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            if not line.strip(): continue
            m = json.loads(line)
            if m.get("type") == "ping":
                self.pings += 1; self.send({"type": "pong"}); continue
            if m.get("type") == "register": self.registers.append(m)
            self.log.append(m); msgs.append(m)
        return msgs
    def wait(self, pred, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            for m in self.pump(0.2):
                if pred(m): return m
        return None
    def tool_names(self):
        return [t["name"] for t in self.registers[-1]["tools"]] if self.registers else []
