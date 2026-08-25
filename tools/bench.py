#!/usr/bin/env python3
"""Drive apps/bench/display's or apps/bench/main's console from the host.

Both bench apps speak the same protocol -- one `OK `/`ERR ` line per command,
a parseable `HB ` heartbeat every 2 s -- so one driver covers both. They
differ only in which CPU they run on, and therefore in which USB product
string identifies the port.

`pico_stdio_usb` neither transmits nor accepts input until the host asserts
DTR (the hardware record), and it DROPS output when nothing is attached rather than
buffering it -- so this opens with DTR set and treats the heartbeat, not the
startup banner, as the source of truth for anything printed before we bound
the port.

Usage:
    python tools/bench.py <command> [...]      run one command, print the reply
    python tools/bench.py --watch <seconds>    just print whatever arrives
    python tools/bench.py --cpu main radio     talk to the MAIN CPU instead

`--cpu display` is the default, because that is where eight of the nine
drivers live; `--cpu main` reaches the CC1101 radios.
"""
import sys
import time

import serial
from serial.tools import list_ports

CPU_PREFIX = {
    "display": "FWOG display",
    "main": "FWOG main",
}


def find_cpu_port(cpu="display"):
    """One CPU's COM port, by USB product string.

    Windows hides the product string from pyserial, so this reuses fw.py's
    SetupAPI lister rather than guessing at COM numbers.
    """
    import pathlib
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    import fw
    try:
        prefix = CPU_PREFIX[cpu]
    except KeyError:
        raise SystemExit("unknown --cpu %r (want display or main)" % cpu)
    for p in fw._cpu_ports():
        if p.vid in fw.FWOG_USB_VIDS and (p.product or "").startswith(prefix):
            return p.device
    raise SystemExit("no running %s CPU found (is it enumerated?)" % cpu)


# Kept as the old name so existing callers do not break.
def find_display_port():
    return find_cpu_port("display")


class Bench:
    def __init__(self, port=None, timeout=0.2, cpu="display"):
        self.port = port or find_cpu_port(cpu)
        self.ser = serial.Serial(self.port, 115200, timeout=timeout)
        self.ser.dtr = True          # fact 17: without this the CPU stays mute
        time.sleep(0.3)
        self.ser.reset_input_buffer()
        self._buf = ""

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def _pump(self):
        data = self.ser.read(8192)
        if data:
            self._buf += data.decode("utf-8", "replace")
        lines = []
        while "\n" in self._buf:
            line, _, self._buf = self._buf.partition("\n")
            lines.append(line.rstrip("\r"))
        return lines

    def drain(self, seconds):
        """Collect every line that arrives over `seconds`."""
        out, end = [], time.time() + seconds
        while time.time() < end:
            out += self._pump()
            time.sleep(0.05)
        return out

    def send(self, cmd, wait=2.0, expect=("OK", "ERR")):
        """Send one command; return (reply_line_or_None, other_lines)."""
        self.ser.write((cmd.strip() + "\n").encode())
        self.ser.flush()
        other, end = [], time.time() + wait
        while time.time() < end:
            for line in self._pump():
                if line.startswith(expect):
                    return line, other
                other.append(line)
            time.sleep(0.02)
        return None, other


if __name__ == "__main__":
    args = sys.argv[1:]
    cpu = "display"
    if len(args) >= 2 and args[0] == "--cpu":
        cpu, args = args[1], args[2:]
    if not args:
        raise SystemExit(__doc__)
    b = Bench(cpu=cpu)
    print("# %s cpu on port %s" % (cpu, b.port), file=sys.stderr)
    try:
        if args[0] == "--watch":
            secs = float(args[1]) if len(args) > 1 else 6.0
            for line in b.drain(secs):
                print(line)
        else:
            reply, other = b.send(" ".join(args))
            for line in other:
                print("  . %s" % line)
            print(reply if reply is not None else "<no OK/ERR within timeout>")
    finally:
        b.close()
