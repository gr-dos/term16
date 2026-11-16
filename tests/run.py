#!/usr/bin/env python3
"""
term16 tests under DOSBox-X.

Each DOSBox-X session runs one batch file on drive C: (a fresh work
directory) with COM1 as a nullmodem connected, over TCP, to a Python
"remote host" running in a thread here.  t16test.exe (term16 built with
-DTERM16_TEST) reads its keys from %T16KEYS% and appends screen dumps
to %T16SCR% (src/screen.c); the checks run here afterwards.

Usage: tests/run.py TERM16.EXE T16TEST.EXE
Environment: DOSBOX (default dosbox-x), SZ (default sz; the ZMODEM
interoperability tests are skipped without it), ZBAUD (the zmodem_sz
speed, default 115200), KEEP=1 to keep work
directories, ONLY=name to run tests whose name contains it.
"""

import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import traceback

DOSBOX = os.environ.get("DOSBOX", "dosbox-x")
SZ = os.environ.get("SZ") or shutil.which("sz") or shutil.which("lsz")
TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ZBAUD = os.environ.get("ZBAUD", "115200")   # zmodem_sz's speed
VERSION = open(os.path.join(TOP, "VERSION")).read().strip()

# Test keyboard codes (src/screen.c)
IDLE, DUMP, PAUSE, ZDONE = 0xFFFF, 0xFFFE, 0xFFFD, 0xFFFC
K_UP, K_DOWN, K_LEFT, K_RIGHT = 0x4800, 0x5000, 0x4B00, 0x4D00
K_HOME, K_END, K_DEL, K_PGUP = 0x4700, 0x4F00, 0x5300, 0x4900
K_F1, K_F5 = 0x3B00, 0x3F00
ALT_M, ALT_X = 0x3200, 0x2D00
ESC, ENTER, BKSP = 0x011B, 0x1C0D, 0x0E08


def keys(*items):
    """Key words: ints are INT 16h codes, strings are typed characters."""
    out = []
    for it in items:
        if isinstance(it, str):
            out.extend(ord(c) for c in it)
        else:
            out.append(it)
    return b"".join(struct.pack("<H", k) for k in out)


class Remote:
    """The TCP end of COM1."""

    def __init__(self, conn):
        self.c = conn
        self.buf = b""

    def send(self, data):
        if isinstance(data, str):
            data = data.encode("latin-1")
        self.c.sendall(data)

    def read(self, timeout=0.2):
        self.c.settimeout(timeout)
        try:
            d = self.c.recv(65536)
            if not d:
                raise EOFError("connection closed")
            self.buf += d
        except socket.timeout:
            pass

    def expect(self, want, timeout=30):
        if isinstance(want, str):
            want = want.encode("latin-1")
        end = time.time() + timeout
        while want not in self.buf:
            if time.time() > end:
                raise AssertionError("remote: timed out waiting for %r, got %r"
                                     % (want, self.buf[-200:]))
            self.read()
        i = self.buf.index(want) + len(want)
        got, self.buf = self.buf[:i], self.buf[i:]
        return got


def run_dos(batch, files=None, remote=None, timeout=240, name="t"):
    """Run batch lines in DOSBox-X; returns (work dir, remote error)."""
    global LAST_WORK
    work = tempfile.mkdtemp(prefix="term16-%s." % name)
    LAST_WORK = work
    for fname, data in (files or {}).items():
        path = os.path.join(work, fname)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
    with open(os.path.join(work, "RC.BAT"), "w", newline="") as f:
        f.write("@echo off\r\nif errorlevel 2 goto rc2\r\nif errorlevel 1 goto rc1\r\n"
                "echo rc0 >c:\\r%1.txt\r\ngoto end\r\n:rc1\r\necho rc1 >c:\\r%1.txt\r\n"
                "goto end\r\n:rc2\r\necho rc2 >c:\\r%1.txt\r\n:end\r\n")
    with open(os.path.join(work, "STEPS.BAT"), "w", newline="") as f:
        f.write("@echo off\r\nset T16SCR=C:\\SCR.TXT\r\n")
        for line in batch:
            f.write(line + "\r\n")

    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]
    result = {}

    def serve():
        try:
            srv.settimeout(60)
            conn, _ = srv.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            result["conn"] = conn
            if remote is not None:
                remote(Remote(conn), work)
        except EOFError:
            pass
        except Exception:
            result["error"] = traceback.format_exc()

    t = threading.Thread(target=serve, daemon=True)
    t.start()
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    # No "cycles=max": it stalls DOSBox-X's serial port events.
    cmd = [DOSBOX, "-defaultconf", "-silent", "-nogui", "-nomenu",
           "-set", "serial serial1=nullmodem server:127.0.0.1 port:%d "
                   "transparent:1" % port,
           "-c", 'mount c "%s"' % work, "-c", "c:", "-c", "STEPS.BAT",
           "-c", "exit"]
    with open(os.path.join(work, "dosbox.log"), "wb") as log:
        try:
            subprocess.run(cmd, env=env, stdout=log, stderr=log,
                           timeout=timeout)
        except subprocess.TimeoutExpired:
            result.setdefault("error", "DOSBox-X timed out")
    t.join(5)
    if "conn" in result:
        result["conn"].close()
    srv.close()
    return work, result.get("error")


def find(work, name):
    """A file in work by case-insensitive relative path, or None."""
    d = work
    for part in name.replace("\\", "/").split("/"):
        hits = [e for e in os.listdir(d) if e.upper() == part.upper()]
        if not hits:
            return None
        d = os.path.join(d, hits[0])
    return d


def read(work, name):
    p = find(work, name)
    if p is None:
        return None
    with open(p, "rb") as f:
        return f.read()


def screens(work):
    data = read(work, "SCR.TXT") or b""
    out = []
    for chunk in data.split(b"----\r\n"):
        if not chunk:
            continue
        lines = chunk.split(b"\r\n")
        out.append([l.decode("cp437") for l in lines[:25]])
    return out


def status_line(port="COM1", baud="9600", frame="8N1", flow="None",
                term="ANSI", state="OPEN"):
    s = " %s  %s  %s  Flow:%s  %s  %s" % (port, baud, frame, flow, term, state)
    hints = "Alt-M Menu  Alt-X Exit "
    return s.ljust(80 - len(hints)) + hints


# ------------------------------------------------------------------
# A minimal ZMODEM sender, for cases sz cannot produce

ZPAD, ZDLE = b"*", 0x18
ZRQINIT, ZRINIT, ZFILE, ZSKIP, ZNAK, ZFIN, ZRPOS, ZDATA, ZEOF = 0, 1, 4, 5, 6, 8, 9, 10, 11
ZACK, ZCAN = 3, 16


def crc16(data, crc=0):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


def zesc(data):
    out = bytearray()
    for b in data:
        if b in (0x18, 0x10, 0x11, 0x13, 0x90, 0x91, 0x93):
            out += bytes([ZDLE, b ^ 0x40])
        else:
            out.append(b)
    return bytes(out)


def hexhdr(t, arg=0):
    b = bytes([t]) + struct.pack("<I", arg)
    c = crc16(b)
    return b"**\x18B" + (b + bytes([c >> 8, c & 255])).hex().encode() + b"\r\n\x11"


def binhdr(t, arg=0):
    b = bytes([t]) + struct.pack("<I", arg)
    c = crc16(b)
    return b"*\x18A" + zesc(b + bytes([c >> 8, c & 255]))


def subpacket(data, end=b"i", corrupt=False):
    c = crc16(data + end)
    if corrupt:
        c ^= 0x5555
    return zesc(data) + bytes([ZDLE]) + end + zesc(bytes([c >> 8, c & 255]))


def read_hex_header(r, timeout=30):
    """The next hex header from the receiver: (type, arg)."""
    r.expect(b"**\x18B", timeout)
    end = time.time() + timeout
    while len(r.buf) < 14:
        if time.time() > end:
            raise AssertionError("short header")
        r.read()
    h = bytes.fromhex(r.buf[:14].decode())
    r.buf = r.buf[14:]
    assert crc16(h) == 0, "bad header CRC from receiver"
    return h[0], struct.unpack("<I", h[1:5])[0]


def zsend(r, files, corrupt_at=None, cancel_at=None, slow=0.0):
    """Send [(name, data)].  corrupt_at: a subpacket offset sent once
    with a bad CRC; cancel_at: offset where the sender cancels."""
    r.send(b"rz\r" + hexhdr(ZRQINIT))
    t, _ = read_hex_header(r)
    assert t == ZRINIT, "wanted ZRINIT, got %d" % t
    for name, data in files:
        info = name.encode("latin-1") + b"\0" + b"%d 0 100644 0" % len(data) + b"\0"
        r.send(binhdr(ZFILE) + subpacket(info, b"k"))
        t, pos = read_hex_header(r)
        if t == ZSKIP:
            continue
        assert t == ZRPOS, "wanted ZRPOS, got %d" % t
        corrupted = False
        while True:
            r.send(binhdr(ZDATA, pos))
            restart = None
            while pos < len(data):
                if cancel_at is not None and pos >= cancel_at:
                    r.send(b"\x18" * 8 + b"\x08" * 8)
                    return "cancelled"
                chunk = data[pos:pos + 256]
                bad = corrupt_at is not None and pos >= corrupt_at and not corrupted
                last = pos + len(chunk) >= len(data)
                r.send(subpacket(chunk, b"h" if bad or last else b"i", bad))
                if slow:
                    time.sleep(slow)
                    r.read(0.01)
                    if b"\x18" * 5 in r.buf:
                        return "receiver cancelled"
                if bad:
                    corrupted = True
                    t, restart = read_hex_header(r)
                    assert t == ZRPOS, "wanted ZRPOS after bad CRC, got %d" % t
                    break
                pos += len(chunk)
            if restart is not None:
                pos = restart
                continue
            if not data:
                r.send(subpacket(b"", b"h"))
            break
        r.send(hexhdr(ZEOF, len(data)))
        end = time.time() + 30
        while b"**\x18B" not in r.buf and b"\x18" * 5 not in r.buf:
            if time.time() > end:
                raise AssertionError("no answer to ZEOF")
            r.read()
        if b"**\x18B" not in r.buf:
            return "receiver cancelled"
        t, _ = read_hex_header(r)
        assert t == ZRINIT, "wanted ZRINIT after ZEOF, got %d" % t
    r.send(hexhdr(ZFIN))
    t, _ = read_hex_header(r)
    assert t == ZFIN
    r.send(b"OO")
    return "done"


# ------------------------------------------------------------------
# tests

TESTS = []
LAST_WORK = None


def test(fn):
    TESTS.append(fn)
    return fn


class Fail(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Fail(msg)


def rc(work, n):
    data = read(work, "R%d.TXT" % n)
    return None if data is None else int(data.decode().strip()[2:])


@test
def cli():
    """Options that do not open the screen: help, version, errors."""
    steps = [
        ("TERM16 --help >O1.TXT", 0),
        ("TERM16 -h >O2.TXT", 0),
        ("TERM16 --version >O3.TXT", 0),
        ("TERM16 --bogus >O4.TXT", 1),
        ("TERM16 extra >O5.TXT", 1),
        ("TERM16 -b 12345 >O6.TXT", 1),
        ("TERM16 --port=COM5 >O7.TXT", 1),
        ("TERM16 -d C:\\NOSUCH >O8.TXT", 1),
        ("TERM16 --parity >O9.TXT", 1),
        ("TERM16 -p 3 >O10.TXT", 1),            # no COM3 in DOSBox-X
        ("TERM16 /? >O11.TXT", 0),
    ]
    batch = []
    for i, (cmd, _) in enumerate(steps, 1):
        batch += [cmd, "call rc %d" % i]
    work, err = run_dos(batch, files={"TERM16.EXE": EXE}, name="cli")
    check(err is None, err)
    for i, (cmd, want) in enumerate(steps, 1):
        check(rc(work, i) == want, "%s: exit status %s, expected %d"
              % (cmd, rc(work, i), want))
    help_text = read(work, "O1.TXT").decode()
    check(help_text.startswith("Usage: term16"), "--help output")
    check("--download-dir" in help_text, "--help lists --download-dir")
    check(read(work, "O2.TXT") == read(work, "O1.TXT"), "-h same as --help")
    check(read(work, "O11.TXT") == read(work, "O1.TXT"), "/? same as --help")
    check(read(work, "O3.TXT") == b"term16 (gr-dos term16) %s\r\n" % VERSION.encode(),
          "--version: %r" % read(work, "O3.TXT"))
    for i in range(4, 11):
        check(read(work, "O%d.TXT" % i) == b"", "step %d: no standard output" % i)
    return work


ANSI_DEMO = (
    "\x1b[2J\x1b[HHello, \x1b[1mbold\x1b[0m world\r\n"
    "\x1b[5;10Hat 5,10"
    "\x1b[24;1Hbottom row"
    # Everything below tries to reach the status bar on row 25
    "\x1b[25;1HROW25?"
    "\x1b[99;99HX"
    "\x1b[30B\x1b[2KY"
    "\x1b7\x1b[1;25r\x1b[25;1HZ\x1b8"
    "\x1b[r"
)


@test
def terminal():
    """ANSI rendering, cursor clipping, protected status bar, replies."""
    def remote(r, work):
        r.expect("go\r")
        r.send(ANSI_DEMO)
        r.expect("2\r")
        # Scroll the whole viewport, then erase it: the bar must survive
        r.send("".join("line %d\r\n" % i for i in range(40)))
        r.send("\x1b[2J\x1b[H\x1b[J\x1b[1;1Hafter clear\x1b[6n")
        r.expect("\x1b[1;12R")
        r.send("\x1b[c")
        r.expect("\x1b[?1;2c")
        # DEC line drawing, tabs, backspace, insert/delete
        r.send("\r\n\x1b(0lqqk\x1b(B\tT\x08U\r\nabcdef\x1b[3D\x1b[2P\r\n"
               "abc\x1b[2D\x1b[2@!\r\n")
        r.expect("3\r")
        # A wrapped line and an ignored OSC title
        r.send("\x1b]0;window title\x07" + "w" * 85 + "\r\n")
        r.expect("4\r")

    work, err = run_dos(
        ["set T16KEYS=C:\\K.BIN", "T16TEST -p 1 -b 9600"],
        files={"T16TEST.EXE": TEXE,
               "K.BIN": keys("go\r", IDLE, DUMP, "2\r", IDLE, DUMP, "3\r",
                             IDLE, DUMP, "4\r", IDLE, DUMP, ALT_X)},
        remote=remote, name="term")
    check(err is None, err)
    s = screens(work)
    check(len(s) == 4, "4 screen dumps, got %d" % len(s))
    a = s[0]
    check(a[0].startswith("Hello, bold world"), "row 1: %r" % a[0])
    check(a[4][9:16] == "at 5,10", "row 5: %r" % a[4])
    # The bottom row of the viewport holds what was aimed lower
    check(a[23] == "Z" + " " * 78 + "Y", "row 24: %r" % a[23])
    check(a[24] == status_line(), "status bar after escapes: %r" % a[24])
    b = s[1]
    check(b[0].startswith("after clear"), "after clear: %r" % b[0])
    check(b[24] == status_line(), "status bar after scrolling: %r" % b[24])
    c = s[1]
    check(c[1].startswith("\u250c\u2500\u2500\u2510"), "DEC graphics: %r" % c[1])
    check(c[1][8] == "U", "tab and backspace: %r" % c[1])
    check(c[2].startswith("abcf "), "delete characters: %r" % c[2])
    check(c[3].startswith("a! bc "), "insert characters: %r" % c[3])
    d = s[2]
    wrapped = [l for l in d if l.startswith("w" * 80)]
    check(wrapped, "an 80-column line of w")
    i = d.index(wrapped[-1])
    check(d[i + 1].startswith("wwwww "), "wrap to the next line: %r" % d[i + 1])
    check("window title" not in "".join(d), "OSC not shown")
    return work


@test
def keyboard():
    """Keys become the bytes a VT100/xterm-style host expects."""
    want = (b"ab\r\x7f\x1b[A\x1b[B\x1b[C\x1b[D\x1b[1~\x1b[4~\x1b[3~\x1b[5~"
            b"\x1bOP\x1b[15~\x03\x1b")

    def remote(r, work):
        r.expect(b"go" + want)
        r.send("\x1b[?1h")                  # application cursor keys
        r.expect("\x1bOA")

    k = keys("go", "ab", ENTER, BKSP, K_UP, K_DOWN, K_RIGHT, K_LEFT, K_HOME,
             K_END, K_DEL, K_PGUP, K_F1, K_F5, 0x2E03, ESC, PAUSE, K_UP,
             IDLE, ALT_X)
    work, err = run_dos(["set T16KEYS=C:\\K.BIN", "T16TEST -p1"],
                        files={"T16TEST.EXE": TEXE, "K.BIN": k},
                        remote=remote, name="keys")
    check(err is None, err)
    return work


@test
def menu():
    """No arguments: the menu; settings carry into the session."""
    def remote(r, work):
        r.expect("go")

    k = keys(IDLE, DUMP,
             K_HOME, K_DOWN, K_RIGHT, K_RIGHT,     # baud 9600 -> 38400
             K_DOWN, K_DOWN, K_RIGHT,              # parity none -> odd
             K_DOWN, K_DOWN, K_RIGHT,              # flow none -> RTS/CTS
             K_DOWN, ENTER,                        # terminal ANSI -> UTF8
             K_DOWN, ENTER, *([BKSP] * 70))        # edit the directory
    k += keys("C:\\NOPE", ENTER, ESC)              # refused, cancelled
    k += keys(ENTER, *([BKSP] * 70)) + keys("c:\\dl", ENTER)
    k += keys(K_DOWN, ENTER, IDLE, DUMP)           # Start session
    k += keys("go", IDLE, ALT_M, IDLE, DUMP, ESC, IDLE, DUMP, ALT_X)
    work, err = run_dos(["set T16KEYS=C:\\K.BIN", "T16TEST", "call rc 1"],
                        files={"T16TEST.EXE": TEXE, "K.BIN": k,
                               "DL/KEEP.TXT": b"x"},
                        remote=remote, name="menu")
    check(err is None, err)
    s = screens(work)
    check(len(s) == 4, "4 screen dumps, got %d" % len(s))
    first = "\n".join(s[0])
    check("term16 " + VERSION in first, "menu title")
    for label in ("Port", "Baud rate", "Parity", "Flow control",
                  "Download directory", "Start session"):
        check(label in first, "menu shows %s" % label)
    check(s[0][24] == status_line(state="CLOSED"), "menu status: %r" % s[0][24])
    want = status_line(baud="38400", frame="8O1", flow="RTS/CTS", term="UTF8")
    check(s[1][24] == want, "session status: %r" % s[1][24])
    check("Return to session" in "\n".join(s[2]), "menu from a session")
    check(s[2][24] == want, "menu status in session: %r" % s[2][24])
    check(s[3][24] == want, "status after Esc: %r" % s[3][24])
    check("C:\\DL" not in s[1][24], "no directory in the bar")
    check(rc(work, 1) == 0, "exit status %s" % rc(work, 1))
    return work


def sz_remote(paths, extra=()):
    def remote(r, work):
        r.expect("go\r")
        r.c.settimeout(None)
        fd = r.c.fileno()
        p = subprocess.run([SZ, "-q"] + list(extra) + paths, stdin=fd,
                           stdout=fd, stderr=subprocess.PIPE, timeout=180)
        check(p.returncode == 0, "sz failed: %r" % p.stderr)
        r.send("\r\nsent\r\n")
        r.expect("q")
    return remote


def make_data(n, seed):
    out = bytearray()
    x = seed
    while len(out) < n:
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out.append((x >> 16) & 255)
    # every byte value ZMODEM escapes, at the start
    return bytes(range(256)) + bytes(out)


@test
def zmodem_sz():
    """Automatic ZMODEM download from sz: one file, then a batch."""
    if SZ is None:
        return "skip"
    src = tempfile.mkdtemp(prefix="term16-src.")
    names = {"report.txt": make_data(5000, 1),
             "a-much-longer-name.tar.gz": make_data(9000, 2),
             "small": b"tiny\n",
             "empty.dat": b""}
    for n, d in names.items():
        with open(os.path.join(src, n), "wb") as f:
            f.write(d)

    def remote(r, work):
        sz_remote([os.path.join(src, "report.txt")])(r, work)
        sz_remote([os.path.join(src, n) for n in
                   ("a-much-longer-name.tar.gz", "small", "empty.dat",
                    "report.txt")])(r, work)

    k = keys("go\r", ZDONE, IDLE, DUMP, "q", "go\r", ZDONE, IDLE, DUMP, "q",
             IDLE, ALT_X)
    work, err = run_dos(["set T16KEYS=C:\\K.BIN",
                         "T16TEST -p 1 -b %s -d C:\\DL" % ZBAUD],
                        files={"T16TEST.EXE": TEXE, "K.BIN": k,
                               "DL/REPORT.TXT": b"already here"},
                        remote=remote, name="sz")
    shutil.rmtree(src)
    check(err is None, err)
    check(read(work, "DL/REPORT.TXT") == b"already here", "never overwrite")
    check(read(work, "DL/REPORT~1.TXT") == names["report.txt"], "REPORT~1.TXT")
    check(read(work, "DL/REPORT~2.TXT") == names["report.txt"], "REPORT~2.TXT")
    check(read(work, "DL/A-MUCH-L.GZ") == names["a-much-longer-name.tar.gz"],
          "8.3 name for a long name")
    check(read(work, "DL/SMALL") == b"tiny\n", "SMALL")
    check(read(work, "DL/EMPTY.DAT") == b"", "EMPTY.DAT")
    left = [f for f in os.listdir(find(work, "DL")) if f.upper().endswith(".PRT")]
    check(not left, "no partial files: %r" % left)
    s = screens(work)
    check(len(s) == 2, "2 screen dumps, got %d" % len(s))
    for sc in s:
        check(sc[24] == status_line(baud=ZBAUD), "status: %r" % sc[24])
        check("sent" in "\n".join(sc), "terminal usable after the transfer")
        check("ZMODEM" not in "\n".join(sc), "progress view closed")
        check("OO" not in "\n".join(sc), "sz's closing OO not shown")
    return work


@test
def zmodem_names():
    """Sender names cannot escape the directory or name a device."""
    files = [("../../ESCAPE.TXT", b"one"), ("/etc/passwd", b"two"),
             ("C:\\AUTOEXEC.BAT", b"three"), ("con", b"four"),
             ("..", b"five"), ("we ird*na:me?.t x t", b"six")]

    def remote(r, work):
        r.expect("go\r")
        check(zsend(r, files) == "done", "transfer")
        r.send("\r\nsent\r\n")

    k = keys("go\r", ZDONE, IDLE, ALT_X)
    work, err = run_dos(["md DL", "set T16KEYS=C:\\K.BIN",
                         "T16TEST -p 1 -b 115200 -d DL"],
                        files={"T16TEST.EXE": TEXE, "K.BIN": k},
                        remote=remote, name="names")
    check(err is None, err)
    check(read(work, "ESCAPE.TXT") is None, "no file outside DL")
    check(read(work, "DL/ESCAPE.TXT") == b"one", "ESCAPE.TXT")
    check(read(work, "DL/PASSWD") == b"two", "PASSWD")
    check(read(work, "DL/AUTOEXEC.BAT") == b"three", "AUTOEXEC.BAT in DL")
    check(read(work, "DL/_CON") == b"four", "_CON")
    check(read(work, "DL/FILE") == b"five", "FILE for '..'")
    check(read(work, "DL/ME_.TXT") == b"six", "ME_.TXT: %r" % os.listdir(find(work, "DL")))
    return work


@test
def zmodem_errors():
    """A bad CRC is retried; a sender cancel keeps the partial file."""
    data = make_data(3000, 3)

    def remote(r, work):
        r.expect("go\r")
        check(zsend(r, [("crc.bin", data)], corrupt_at=1024) == "done", "retry")
        r.send("\r\nfirst\r\n")
        r.expect("go\r")
        check(zsend(r, [("cut.bin", data)], cancel_at=1024) == "cancelled", "cancel")
        time.sleep(2)
        r.send("\r\nsecond\r\n")
        r.expect("go\r")
        r.send("**\x18B00")                 # a start, then nothing
        r.expect("\x18" * 5, 60)            # it gives up and cancels
        time.sleep(1)
        r.send("\r\nthird\r\n")

    k = keys("go\r", ZDONE, IDLE, DUMP,
             "go\r", IDLE, PAUSE, DUMP, "x", ZDONE, IDLE,
             "go\r", ZDONE, IDLE, DUMP, ALT_X)
    work, err = run_dos(["md DL", "set T16KEYS=C:\\K.BIN",
                         "T16TEST -p 1 -b 115200 --download-dir=C:\\DL"],
                        files={"T16TEST.EXE": TEXE, "K.BIN": k},
                        remote=remote, name="errors", timeout=400)
    check(err is None, err)
    check(read(work, "DL/CRC.BIN") == data, "file after a CRC retry")
    part = read(work, "DL/CUT.PRT")
    check(part is not None and part == data[:len(part)] and len(part) >= 1024,
          "partial file kept as CUT.PRT")
    check(read(work, "DL/CUT.BIN") is None, "no CUT.BIN")
    s = screens(work)
    check(len(s) == 3, "3 dumps, got %d" % len(s))
    check("first" in "\n".join(s[0]), "terminal after retry")
    box = "\n".join(s[1])
    check("Cancelled by the sender" in box, "cancel message: %s" % box)
    check("CUT.PRT" in box, "partial name shown")
    check("third" in "\n".join(s[2]), "terminal after a dead sender")
    for sc in s:
        check(sc[24] == status_line(baud="115200"), "status: %r" % sc[24])
    return work


@test
def zmodem_rename():
    """A file that cannot get its final name is kept whole as .PRT."""
    data = make_data(3000, 5)

    def remote(r, work):
        r.expect("go\r")
        got = zsend(r, [("ren.bin", data), ("next.bin", data)])
        check(got == "receiver cancelled", "rename failure cancels: %s" % got)
        time.sleep(2)
        r.send("\r\nafter\r\n")

    k = keys("go\r", IDLE, PAUSE, DUMP, "x", ZDONE, IDLE, DUMP, ALT_X)
    work, err = run_dos(["md DL", "set T16KEYS=C:\\K.BIN", "set T16NOREN=1",
                         "T16TEST -p 1 -b 115200 -d DL"],
                        files={"T16TEST.EXE": TEXE, "K.BIN": k},
                        remote=remote, name="rename")
    check(err is None, err)
    check(read(work, "DL/REN.PRT") == data, "complete file kept as REN.PRT")
    check(read(work, "DL/REN.BIN") is None, "no REN.BIN")
    check(read(work, "DL/NEXT.PRT") is None, "batch stopped")
    s = screens(work)
    check(len(s) == 2, "2 dumps, got %d" % len(s))
    box = "\n".join(s[0])
    check("Cannot rename the received file" in box, "message: %s" % box)
    check("Received file kept as REN.PRT" in box, "kept name shown")
    check("Saved as" not in box, "not reported as saved")
    check("Files done: 0" in box, "not counted: %s" % box)
    check("after" in "\n".join(s[1]), "terminal after the failure")
    return work


@test
def zmodem_esc():
    """Esc at the keyboard cancels a transfer."""
    data = make_data(20000, 4)

    def remote(r, work):
        r.expect("go\r")
        try:
            zsend(r, [("slow.bin", data)], slow=0.25)
        except (AssertionError, BrokenPipeError, EOFError):
            pass
        r.expect("\x18" * 5, 30)
        time.sleep(2)
        r.send("\r\nback\r\n")

    k = keys("go\r", PAUSE, PAUSE, ESC, IDLE, PAUSE, DUMP, "x", ZDONE, IDLE,
             DUMP, ALT_X)
    work, err = run_dos(["md DL", "set T16KEYS=C:\\K.BIN",
                         "T16TEST -p 1 -b 115200 -d DL"],
                        files={"T16TEST.EXE": TEXE, "K.BIN": k},
                        remote=remote, name="esc")
    check(err is None, err)
    s = screens(work)
    check(len(s) == 2, "2 dumps, got %d" % len(s))
    check("Cancelled at the keyboard" in "\n".join(s[0]), "cancel message")
    check("back" in "\n".join(s[1]), "terminal after cancel")
    check(read(work, "DL/SLOW.PRT") is not None, "partial SLOW.PRT")
    return work


def main():
    global EXE, TEXE
    if len(sys.argv) != 3:
        sys.exit("usage: tests/run.py TERM16.EXE T16TEST.EXE")
    if shutil.which(DOSBOX) is None:
        sys.exit("%s not found" % DOSBOX)
    EXE = open(sys.argv[1], "rb").read()
    TEXE = open(sys.argv[2], "rb").read()
    only = os.environ.get("ONLY")
    passed = failed = skipped = 0
    for fn in TESTS:
        if only and only not in fn.__name__:
            continue
        t0 = time.time()
        try:
            work = fn()
            if work == "skip":
                print("skip %s (no sz)" % fn.__name__)
                skipped += 1
                continue
            print("ok   %s (%.0fs)" % (fn.__name__, time.time() - t0))
            passed += 1
            if os.environ.get("KEEP") != "1":
                shutil.rmtree(work, ignore_errors=True)
            else:
                print("     work directory:", work)
        except Fail as e:
            print("FAIL %s: %s" % (fn.__name__, e))
            print("     work directory:", LAST_WORK)
            failed += 1
        sys.stdout.flush()
    print("%d passed, %d failed, %d skipped" % (passed, failed, skipped))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
