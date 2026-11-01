#!/bin/sh
#
# Create the AMIX device node for the ZZ9000 framebuffer driver.
# This matches the documented cdevsw major 49 integration.
# 0660 root:sys: the X server runs as root; the node gives others nothing.
#
# Copyright (c) 2026 Ignacio Soriano; changes Copyright (c) 2026 Jussi Alanärä.
# SPDX-License-Identifier: MIT

rm -f /dev/zz9000 /dev/zz9000i
mknod /dev/zz9000 c 49 0
chown root /dev/zz9000
chgrp sys /dev/zz9000
chmod 660 /dev/zz9000
# the information node (minor 128): board info and sensors only, for everyone (ztop)
mknod /dev/zz9000i c 49 128
chown root /dev/zz9000i
chgrp sys /dev/zz9000i
chmod 444 /dev/zz9000i
ls -l /dev/zz9000 /dev/zz9000i
