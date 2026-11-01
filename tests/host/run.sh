#!/bin/sh
# run.sh [-v] -- build and run the driver's host tests in three configurations:
#   module  -DZZ9K_MODULE -DZZ9K_TARGET_Z3660 -DZZ9K_MONSW (the loadable module)
#   zz9000  the static ZZ9000-only driver (upstream's build)
#   z3660   the static Z3660 driver
#   module-k46map, module-k30map  the module with the board above the identity map
# Needs a host cc that can map fixed low addresses (Linux).  Exit 0 = all passed.
#
# Copyright (c) 2026 Jussi Alanärä.  SPDX-License-Identifier: MIT
set -e
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../../overlay/usr/sys/amiga/driver/zz9000
out=${TMPDIR:-/tmp}/zz9k-host.$$
mkdir -p "$out"
trap 'rm -rf "${out:?}"' 0
# the static driver includes "../../console/screen.h": give it a directory two below the stubs
mkdir -p "$out/inc/driver/zz9000"
ln -s "$here/stub/amiga/console" "$out/inc/console"
CC=${CC:-cc}
FLAGS="-std=gnu89 -O1 -g -w -fno-strict-aliasing -iquote $here/stub -iquote $out/inc/driver/zz9000"
FLAGS="$FLAGS -I$here/stub-angle -I$here -I$src"
rc=0
build() {
	name=$1; shift
	$CC $FLAGS -DZZ9K_HOST_CONFIG="\"$name\"" "$@" -o "$out/$name" "$here/zz9k_host.c"
	"$out/$name" "${V:-}" || rc=1
}
[ "${1:-}" = -v ] && V=-v
build module -DZZ9K_MODULE -DZZ9K_TARGET_Z3660 -DZZ9K_MONSW
build zz9000
build z3660 -DZZ9K_TARGET_Z3660
# the board above the identity map (autoconfig_rtg YES), mapped as each line maps it
build module-k46map -DZZ9K_MODULE -DZZ9K_TARGET_Z3660 -DZZ9K_MONSW -DZZ9K_DEV_KVMAP -DZZ9K_HOST_MAPPED
build module-k30map -DZZ9K_MODULE -DZZ9K_TARGET_Z3660 -DZZ9K_MONSW -DZZ9K_SPT_MAP -DZZ9K_HOST_MAPPED
exit $rc
