#!/bin/sh
# ztop_test.sh -- ztop's one-shot output and exit codes against a fake board (ztop_shim.c).
# Copyright (c) 2026 Jussi Alanärä.  SPDX-License-Identifier: MIT
set -e
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../../overlay/usr/sys/amiga/driver/zz9000
out=${TMPDIR:-/tmp}/ztop-test.$$
mkdir -p "$out"
trap 'rm -rf "${out:?}"' 0
CC=${CC:-cc}
# ztop and the shim share the header, so the host's long size does not matter
$CC -std=gnu89 -w -Dopen=fake_open -Dioctl=fake_ioctl -I"$src" \
	-o "$out/ztop" "$src/tools/ztop.c" "$here/ztop_shim.c"
fail=0
check() {
	if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; fail=1; fi
}
got=$(ZTOP_FAKE=full "$out/ztop" -1 | tr '\n' ';')
check "one-shot values" "$got" "fpga_temp 41.2 C;board_temp 35.1 C;cpu_temp 45.6 C;vcore 1.00 V;vaux 1.80 V;v3v3 3.31 V;v5 5.02 V;cpu_mhz 50 MHz;"
rc=0; ZTOP_FAKE=none "$out/ztop" -1 2>/dev/null || rc=$?
check "no sensors: exit 3" "$rc" 3
rc=0; ZTOP_FAKE=old "$out/ztop" -1 2>/dev/null || rc=$?
check "a driver without the call: exit 1" "$rc" 1
rc=0; ZTOP_FAKE=nodev "$out/ztop" -1 2>/dev/null || rc=$?
check "no device: exit 1" "$rc" 1
rc=0; "$out/ztop" -i 0 2>/dev/null || rc=$?
check "a bad interval: exit 2" "$rc" 2
exit $fail
