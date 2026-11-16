#!/bin/sh
#
# Test term16 under DOSBox-X: tests/run.py does the work (a scripted
# remote host on COM1, sz for ZMODEM, screen dumps from the test build).
#
# Usage: tests/run.sh TERM16.EXE T16TEST.EXE
# Environment: DOSBOX (default dosbox-x), SZ (default sz), KEEP=1 to
#              keep work directories, ONLY=NAME to run some tests.

top=$(cd "$(dirname "$0")/.." && pwd)
exec python3 "$top/tests/run.py" "$@"
