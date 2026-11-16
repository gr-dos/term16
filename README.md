# term16

A small serial terminal for 16-bit DOS: roughly Minicom without the
modem parts, with automatic ZMODEM download. It is for an 8088/8086 PC
(DOS 3.3 or later) connected over a null-modem cable to a Linux host, a
serial console or a gateway. The design is in
[docs/DESIGN.md](docs/DESIGN.md) and the work is tracked in
[#1](https://github.com/gr-dos/term16/issues/1).

`term16.exe` is a small-model, real-mode 8086 executable built with Open
Watcom and [libc16](https://github.com/gr-dos/libc16), following the
[hello16](https://github.com/gr-dos/hello16) conventions for `gr-dos/*16`
utilities (layout 1, C only).

## Usage

    term16                       open the settings menu
    term16 -p COM2 -b 19200      start a session at once
    term16 --help                show the options
    term16 --version             show the version

| Option | Values | Default |
|---|---|---|
| `-p`, `--port` | `COM1` to `COM4` (or `1` to `4`) | `COM1` |
| `-b`, `--baud` | 110, 150, 300, 600, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200 | 9600 |
| `--data` | 5 to 8 | 8 |
| `--parity` | `none`, `odd`, `even`, `mark`, `space` (or `n`, `o`, `e`, `m`, `s`) | `none` |
| `--stop` | 1 or 2 | 1 |
| `--flow` | `none`, `rtscts`, `xonxoff` | `none` |
| `-t`, `--term` | `ansi` (code page 437) or `utf8` | `ansi` |
| `-d`, `--download-dir` | an existing directory | the current directory |
| `-m`, `--menu` | open the menu with the other settings filled in | |

Options take their value as the next argument or after `=`
(`--baud=19200`, `-b19200`). With any setting on the command line the
session starts at once; with none (or with `--menu`) the menu opens.
The exit status is 0, or 1 for a bad option, a missing download
directory or a port that cannot be opened.

### The menu

Up and Down choose a line; Left, Right or Enter change the setting;
Enter on *Download directory* edits it (Enter accepts, Esc cancels);
*Start session* applies the settings. Esc backs out: at startup it
exits, during a session it returns to the session unchanged. Alt-X
exits from anywhere.

### In a session

The top 24 rows are the remote terminal; the bottom row is a status
bar the remote host cannot change:

     COM1  9600  8N1  Flow:None  ANSI  OPEN                  Alt-M Menu  Alt-X Exit

It shows what reproduces the session: port, speed, framing, flow
control, terminal mode and whether the port is `OPEN`, `CLOSED` (in the
menu, before a session) or in `ERROR` (it could not be opened). `OPEN`
only means the UART is set up: term16 cannot know whether anything is
at the other end. The download directory is never shown there.

| Key | Sends |
|---|---|
| Alt-M | (local) open the menu |
| Alt-X | (local) exit |
| Backspace | DEL (7Fh); Ctrl+Backspace sends BS (08h) |
| arrows | `ESC [ A`... (`ESC O A`... after the host asks for application cursor keys) |
| Home, Ins, Del, End, PgUp, PgDn | `ESC [ 1 ~` ... `ESC [ 6 ~` |
| F1-F4, F5-F12 | `ESC O P`..., `ESC [ 15 ~`... |
| Alt+letter | ESC and the letter |
| Ctrl+letter, Enter, Esc, Tab | their control characters |

On the remote host use an 80x24 terminal, for example

    export TERM=vt100
    stty rows 24 cols 80

### Terminal emulation

A VT100/ANSI subset for shells and full-screen programs: cursor
movement and positioning (CUU, CUD, CUF, CUB, CNL, CPL, CHA, CUP, HVP,
VPA, HPA), erase (ED, EL, ECH), insert and delete of lines and
characters (IL, DL, ICH, DCH), scroll regions (DECSTBM) and SU/SD,
index and reverse index, save and restore cursor (DECSC/DECRC and
`CSI s`/`CSI u`), tab stops, REP, SGR (bold, underline, blink, reverse,
the 8 and 16 ANSI colours, the first 16 of 256), autowrap with the
VT100's deferred wrap, origin mode, DEC line drawing (`ESC ( 0`),
application cursor keys, cursor show/hide, and status replies (DSR,
cursor position, DA). OSC, DCS and other strings, the alternate screen
(cleared instead) and unknown sequences are parsed and ignored. Every
operation is clipped to the 24-row viewport.

`--term ansi` shows bytes 80h-FFh as code page 437 (BBSes, DOS hosts).
`--term utf8` decodes UTF-8 and shows each character as its code page
437 equivalent, or a near match (curly quotes, rounded corners, heavy
lines), or `?`.

On monochrome displays (mode 7) colours are dropped and bold, underline,
reverse and blink use the MDA attributes.

## ZMODEM download

When the host sends a ZMODEM start (for example `sz file1 file2`),
term16 switches to receiving by itself and opens a progress box (file,
bytes and percent, errors, files done). One file or a batch is
received into the download directory; afterwards the box closes (after
two seconds, or at once on a key) and the terminal carries on. Esc in
the box cancels the transfer.

- **Integrity:** CRC-16 or CRC-32 on every header and subpacket, as the
  sender chooses (term16 offers CRC-32). A bad subpacket, lost data or
  a timeout asks the sender to resend from the last good byte (ZRPOS);
  after 10 failures in a row the transfer is cancelled.
- **Names:** only the last component of the sender's name is used, so
  `../../AUTOEXEC.BAT`, `/etc/passwd` and `C:\X\Y` cannot leave the
  download directory. It is made a valid upper-case 8.3 name (characters
  DOS does not allow become `_`, spaces are dropped, the extension is
  the part after the last dot); `CON`, `NUL`, `COM1` and the other device
  names get a leading `_`.
- **Collisions:** an existing file is never overwritten; the new one
  gets `NAME~1.EXT`, `NAME~2.EXT`, ... (`LONGNA~1` for an 8-character
  name).
- **Partial files:** each file is written as `NAME.PRT` and renamed to
  its final name only when complete. If a transfer is cancelled, the
  disk fills or the sender stops, the `.PRT` file is kept (removed if
  empty) and the box says so. A file that is complete but cannot be
  given its final name is not counted as received: it is kept as
  `.PRT`, the transfer is cancelled and the box says so.
- **Errors:** a missing download directory, a full disk or a file that
  cannot be created cancel the transfer with a message; the terminal and
  serial port stay usable.
- Not supported (docs/DESIGN.md): sending, resuming an interrupted
  transfer, and the sender's remote commands (ZCOMMAND is refused).
  The file date is the date received.

The receiver is written from the published protocol description; no
code comes from lrzsz or other implementations.

## Serial port

COM1 to COM4 at the BIOS base addresses (3F8h, 2F8h, 3E8h, 2E8h when the
BIOS has none), IRQ 4 for COM1/COM3 and IRQ 3 for COM2/COM4. Received
bytes go through an interrupt handler into a 4 KB ring buffer; on a
16550A the FIFO is used (interrupt at 8 bytes), on an 8250 or 16450 the
handler takes one byte per interrupt. Above 3/4 full, term16 drops RTS
(`rtscts`) or sends XOFF (`xonxoff`), and releases below 1/4. Sending is
polled; with `rtscts` it waits for CTS and with `xonxoff` for XON, up to
two seconds per byte.

Whatever opening the port changed (line settings, divisor, IER, MCR,
FIFO, PIC mask, interrupt vector) is put back on exit; if no UART
answers the probe, the line control register it wrote is put back at
once. Ctrl+C is sent to the host and Ctrl+Break is ignored, so neither
ends the program, and a critical disk error fails the DOS call instead
of aborting the program with the handler still installed.

## Size

| | |
|---|---|
| `term16.exe` | 22,748 bytes |
| Memory when loaded | 36,200 bytes (code 18.8 KB, data 12.7 KB with the 4 KB ring buffer, 4 KB stack), plus the PSP |
| While running | up to 8 KB more: the DOS screen, kept to restore at exit, and the terminal screen while the menu is open |

(Version 0.01; the map is in `build/term16.map` after `make`.)

## Limitations and testing status

- Tested under DOSBox-X: emulating an 8086 (`cputype=8086`), and with
  `cycles=fixed 300` (about a 4.77 MHz 8088) receiving ZMODEM at 9600,
  19200 and 38400 baud; the null-modem link is DOSBox-X's TCP
  `nullmodem`. **Not yet tested on real hardware** (an IBM PS/2 Model
  25-class machine, a real 8250, an MDA display); see milestone 4 of
  docs/DESIGN.md.
- Writing straight to CGA video memory can cause "snow" on an original
  IBM CGA.
- Without flow control, speeds above 19200 on an 8088 can overrun the
  UART; ZMODEM recovers by resending, interactive output may lose
  characters.
- No modem features, no networking, no scripting, no XMODEM/YMODEM, no
  ZMODEM upload or resume (docs/DESIGN.md section 8).

## Building and testing

    make                        # term16.exe
    make test                   # host unit tests, then DOSBox-X tests
    make clean

`make` fetches the pinned libc16 release (`scripts/fetch-libc16`,
checked by SHA-256) and the Open Watcom snapshot it names, as in
hello16. `make LIBC16_ROOT=../libc16` uses a local libc16 checkout.

`make test` needs a C compiler, Python 3, `dosbox-x` and `sz` (Debian
and Ubuntu package `lrzsz`; without it the `sz` test is skipped):

- `tests/unit/unit.c` runs the portable modules (terminal emulation,
  settings, CRCs, file names, UART detection against simulated
  registers) on the build host;
- `tests/run.sh` (`tests/run.py`) runs `term16.exe` and the test build
  `build/t16test.exe` in a headless DOSBox-X whose COM1 is connected
  over TCP to a scripted remote host: command-line errors, ANSI
  rendering and the protected status bar, keys, the menu, downloads
  from `sz`, hostile names, CRC errors, cancels, a failed rename and
  Esc. The test build reads keys from a file and writes screen dumps
  (`src/screen.c`), and fails renames when `T16NOREN` is set
  (`src/dosfile.c`).
  `DOSBOX=` selects the emulator (for example a wrapper that adds
  `-set cputype=8086`), `ZBAUD=` the speed of the `sz` test, `KEEP=1`
  keeps the work directories and `ONLY=name` runs some tests.

DOSBox-X's `cycles=max` stalls its emulated serial ports, so the tests
use its default speed.

CI (`.github/workflows/ci.yml`) builds and tests every pull request, and
again on a slow emulated 8086. Merging a change to `VERSION` releases
`term16.exe` with `LICENSE.TXT` (`.github/workflows/release.yml`).

## Files

- `src/main.c`: command line, session loop, cleanup
- `src/config.c`: settings, their names, the status bar
- `src/uart.c`: UART, interrupt handler, ring buffer, flow control
- `src/uprobe.c`: UART detection
- `src/screen.c`: video memory, cursor, speaker, keyboard, clock (and
  the test keyboard)
- `src/term.c`: ANSI/VT100 emulation
- `src/cp437.c`: Unicode to code page 437 (generated by
  `tools/gen-cp437.py`)
- `src/menu.c`: the settings menu
- `src/zmodem.c`, `src/zmcrc.c`: ZMODEM receiver, CRCs and headers
- `src/dlname.c`, `src/dosfile.c`: download names and files
- `tests/`: unit and DOSBox-X tests
- `docs/DESIGN.md`: the design

## License

MIT. See `LICENSE`.
