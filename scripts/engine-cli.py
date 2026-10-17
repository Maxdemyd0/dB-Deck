"""Tiny command-line client for the dB-Deck engine IPC (development aid).

Usage:
  python scripts/engine-cli.py [--pipe NAME] CMD [JSON_ARGS] [CMD [JSON_ARGS] ...]
  python scripts/engine-cli.py --events SECONDS   # print events for a while

The default pipe name is the per-user engine pipe.
"""
import json
import subprocess
import sys
import time


def default_pipe():
    sid = subprocess.run(["whoami", "/user", "/fo", "csv", "/nh"], capture_output=True, text=True).stdout
    sid = sid.strip().split(",")[-1].strip('"')
    return r"\\.\pipe\dB-Deck.Engine.v1." + sid


class Client:
    def __init__(self, pipe):
        for _ in range(50):
            try:
                self.f = open(pipe, "r+b", buffering=0)
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise SystemExit("engine pipe not available: " + pipe)
        self.next_id = 1
        self.buf = b""
        self.events = []

    def _line(self):
        while b"\n" not in self.buf:
            chunk = self.f.read(65536)
            if not chunk:
                raise SystemExit("engine closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def call(self, cmd, args=None):
        rid = self.next_id
        self.next_id += 1
        self.f.write((json.dumps({"id": rid, "cmd": cmd, "args": args or {}}) + "\n").encode())
        while True:
            msg = self._line()
            if msg.get("id") == rid:
                return msg
            self.events.append(msg)

    def read_event(self):
        return self._line()


def main(argv):
    pipe = default_pipe()
    if argv[:1] == ["--pipe"]:
        pipe = r"\\.\pipe" + "\\" + argv[1]
        argv = argv[2:]
    c = Client(pipe)
    print(json.dumps(c.call("hello", {"protocol": 1}), indent=1)[:400])
    if argv[:1] == ["--events"]:
        end = time.time() + float(argv[1])
        while time.time() < end:
            ev = c.read_event()
            if ev.get("event") != "meters":
                print(json.dumps(ev)[:300])
        return
    i = 0
    while i < len(argv):
        cmd = argv[i]
        args = {}
        if i + 1 < len(argv) and argv[i + 1].startswith("{"):
            args = json.loads(argv[i + 1])
            i += 1
        print(json.dumps(c.call(cmd, args), indent=1))
        i += 1


if __name__ == "__main__":
    main(sys.argv[1:])
