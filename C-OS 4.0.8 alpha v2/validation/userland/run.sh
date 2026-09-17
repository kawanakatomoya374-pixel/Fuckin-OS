#!/bin/sh
# run.sh - host tests for the .c-os C runtime.
#
# Same idea as validation/elfloader: compile the REAL userland sources
# and run them natively with their syscalls stubbed.
#
# Two suites:
#
#   test_userland  formatter and math, DIFFERENTIALLY against the host
#                  libc/libm. Each case runs through both implementations
#                  and the outputs are compared, plus 80,000 randomised
#                  inputs. A golden-string test only proves the formatter
#                  agrees with whatever the author believed C99 says -
#                  which, for corners like "%.0f" of 2.5 or a negative
#                  `*` width, is exactly where the author is wrong.
#
#   test_sync      the lock primitives, under REAL concurrent host
#                  threads forwarding to the REAL Linux futex. A lock
#                  that is subtly wrong passes every single-threaded
#                  assertion; what finds a missing barrier or a lost
#                  wakeup is many threads hammering one word.
#
# Usage: sh validation/userland/run.sh
set -e
cd "$(dirname "$0")"
mkdir -p build

INC="-I ../../userland/include"
LIB=../../userland/lib

echo "[1/2] formatter and math vs the host libc/libm"
gcc -o build/test_userland test_userland.c \
    $LIB/cos_fmt.c $LIB/cos_math.c syscall_stub.c \
    $INC -std=gnu11 -w -O2 -lm
./build/test_userland

echo "[2/2] locks under real concurrency"
gcc -o build/test_sync test_sync.c $LIB/cos_sync.c futex_host.c \
    $INC -std=gnu11 -w -O2 -pthread
./build/test_sync
