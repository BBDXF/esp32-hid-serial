#!/usr/bin/env python3
"""Host SDK + CLI for esp32-hid-serial (JSON Lines over UART).

    pip install pyserial
    python host.py --port /dev/ttyUSB0 sys.info
    python host.py --port COM5 wifi.set '{"ssid":"Lab","pass":"secret"}'
    python host.py --port COM5 hid.click '{"button":"left","x":0.5,"y":0.5}'
"""
import argparse
import itertools
import json
import queue
import sys
import threading
import time

import serial


class RpcError(Exception):
    def __init__(self, code, message):
        super().__init__(f"{code}: {message}")
        self.code, self.message = code, message


class HidAgent:
    def __init__(self, port, baud=115200, on_event=None, on_log=None):
        self.ser = serial.Serial(port, baud, timeout=0.05)
        # CP2102 DTR/RTS are wired to EN/IO0: keep them deasserted so we don't reset the board
        self.ser.dtr = False
        self.ser.rts = False
        self.on_event = on_event or (lambda name, data: None)
        self.on_log = on_log or (lambda line: None)
        self._ids = itertools.count(1)
        self._pending = {}
        self._lock = threading.Lock()
        self._alive = True
        self.ser.write(b"\n")  # flush any half frame on the device
        threading.Thread(target=self._reader, daemon=True).start()

    def close(self):
        self._alive = False
        self.ser.close()

    def _reader(self):
        buf = b""
        while self._alive:
            try:
                buf += self.ser.read(4096)
            except serial.SerialException:
                break
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.rstrip(b"\r")
                if line.startswith(b"#"):
                    self.on_log(line[1:].decode(errors="replace"))
                    continue
                if not line.startswith(b"{"):
                    continue  # boot ROM noise
                try:
                    msg = json.loads(line)
                except ValueError:
                    continue
                if "event" in msg:
                    self.on_event(msg["event"], msg.get("data"))
                    continue
                with self._lock:
                    q = self._pending.pop(msg.get("id"), None)
                if q:
                    q.put(msg)

    def call(self, method, params=None, timeout=3.0):
        rid = next(self._ids)
        q = queue.Queue(1)
        with self._lock:
            self._pending[rid] = q
        frame = {"id": rid, "method": method}
        if params is not None:
            frame["params"] = params
        self.ser.write(json.dumps(frame, separators=(",", ":"), ensure_ascii=False).encode() + b"\n")
        try:
            msg = q.get(timeout=timeout)
        except queue.Empty:
            with self._lock:
                self._pending.pop(rid, None)
            raise TimeoutError(method)
        if "error" in msg:
            raise RpcError(msg["error"]["code"], msg["error"]["message"])
        if "warn" in msg:
            print(f"warning: {msg['warn']}", file=sys.stderr)
        return msg.get("result")

    def notify(self, method, params=None):
        frame = {"method": method, "params": params or {}}
        self.ser.write(json.dumps(frame, separators=(",", ":")).encode() + b"\n")

    # convenience helpers
    def key(self, *keys):          return self.call("hid.key", {"keys": list(keys)})
    def type(self, text):          return self.call("hid.type", {"text": text}, timeout=10)
    def click(self, x=None, y=None, button="left", count=1):
        p = {"button": button, "count": count}
        if x is not None:
            p.update(x=x, y=y)
        return self.call("hid.click", p)
    def move(self, dx, dy):        return self.call("hid.mouse_move", {"dx": dx, "dy": dy})
    def scroll(self, v=0, h=0):    return self.call("hid.scroll", {"v": v, "h": h})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--monitor", action="store_true", help="print events/logs after call")
    ap.add_argument("method")
    ap.add_argument("params", nargs="?", help="JSON object")
    a = ap.parse_args()
    dev = HidAgent(a.port, a.baud,
                   on_event=lambda n, d: print(f"event {n} {json.dumps(d, ensure_ascii=False)}"),
                   on_log=lambda l: print(f"log {l}"))
    try:
        res = dev.call(a.method, json.loads(a.params) if a.params else None)
        print(json.dumps(res, ensure_ascii=False, indent=2))
        if a.monitor:
            while True:
                time.sleep(1)
    except (RpcError, TimeoutError) as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        pass
    finally:
        dev.close()


if __name__ == "__main__":
    main()
