/*
 * ztop_shim.c -- ztop on the host: open and ioctl answer from a fake board
 * (ZTOP_FAKE=full, none, old, nodev), so the one-shot output can be checked.
 *
 * Copyright (c) 2026 Jussi Alanärä.  SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "zz9000.h"

int
fake_open(const char *name, int flags, ...)
{
    char *m = getenv("ZTOP_FAKE");

    if (m && strcmp(m, "nodev") == 0) {
        errno = ENOENT;
        return -1;
    }
    return 3;
}

int
fake_ioctl(int fd, int cmd, void *arg)
{
    char *m = getenv("ZTOP_FAKE");
    static int v[ZZ9K_SENS_N] = { 4120, 3510, 4567, 1000, 1800, 3310, 5020, 4990, 50 };
    int i;

    if (cmd == ZZ9KIOC_GETINFO) {
        struct zz9k_info *in = arg;
        memset(in, 0, sizeof *in);
        in->product = 1; in->zorro = 3; in->fw_version = 0x0103;
        return 0;
    }
    if (cmd != ZZ9KIOC_SENSORS) {
        errno = EINVAL;
        return -1;
    }
    if (m && strcmp(m, "old") == 0) {
        errno = EINVAL;
        return -1;
    }
    {
        struct zz9k_sensors *ss = arg;
        if (ss->size < sizeof *ss) {
            errno = EINVAL;
            return -1;
        }
        if (m && strcmp(m, "none") == 0)
            return 0;
        ss->count = ZZ9K_SENS_N;
        for (i = 0; i < ZZ9K_SENS_N; ++i)
            ss->value[i] = v[i];
        ss->valid = 0x1ff & ~(1 << ZZ9K_SENS_VCC);
    }
    return 0;
}
