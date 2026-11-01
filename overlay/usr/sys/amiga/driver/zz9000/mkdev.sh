#!/bin/sh
#
# Create the AMIX device node for the ZZ9000 framebuffer driver.
# This matches the documented cdevsw major 49 integration.
# 0660 root:sys: the X server runs as root; the node gives others nothing.

rm -f /dev/zz9000
mknod /dev/zz9000 c 49 0
chown root /dev/zz9000
chgrp sys /dev/zz9000
chmod 660 /dev/zz9000
ls -l /dev/zz9000
