/*
 * ztop.c - the Z3660's temperatures, voltages and CPU clock on a terminal,
 * from the driver's ZZ9KIOC_SENSORS (the values the AmigaOS ZTop shows).
 *
 * Copyright (c) 2026 Jussi Alanärä.  SPDX-License-Identifier: MIT
 *
 *   ztop [-1] [-i seconds] [-d device]
 *
 * Integer arithmetic only: it runs on a 68LC060 without FP emulation.
 * See ztop(1).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include "zz9000.h"

#define INFODEV "/dev/zz9000i"
#define MAINDEV "/dev/zz9000"

#define K_TEMP  0
#define K_VOLT  1
#define K_MHZ   2

struct sensor {
    char *key;          /* the one-shot name */
    char *label;
    int kind;
};

static struct sensor sensors[ZZ9K_SENS_N] = {
    { "fpga_temp",  "FPGA temperature", K_TEMP },
    { "board_temp", "Board temperature", K_TEMP },
    { "cpu_temp",   "CPU temperature", K_TEMP },
    { "vcore",      "FPGA core voltage", K_VOLT },
    { "vaux",       "FPGA aux voltage", K_VOLT },
    { "v3v3",       "3.3 V rail", K_VOLT },
    { "v5",         "5 V rail", K_VOLT },
    { "vcc",        "Monitor supply", K_VOLT },
    { "cpu_mhz",    "CPU clock (preset)", K_MHZ }
};

static char *progname = "ztop";

static void
usage(void)
{
    fprintf(stderr, "usage: %s [-1] [-i seconds] [-d device]\n", progname);
    exit(2);
}

/* v in the ABI's unit as text: "41.2 C", "5.02 V", "50 MHz" */
static char *
fmt(long v, int kind)
{
    static char buf[4][24];
    static int n;
    char *b;

    b = buf[n = (n + 1) & 3];
    if (v < 0)
        v = 0;
    if (kind == K_TEMP)
        sprintf(b, "%ld.%ld C", v / 100, (v % 100) / 10);
    else if (kind == K_VOLT)
        sprintf(b, "%ld.%02ld V", v / 1000, (v % 1000) / 10);
    else
        sprintf(b, "%ld MHz", v);
    return b;
}

static int
opendev(char *name)
{
    int fd;

    if (name)
        return open(name, O_RDONLY);
    fd = open(INFODEV, O_RDONLY);
    if (fd < 0)
        fd = open(MAINDEV, O_RDONLY);
    return fd;
}

static int
readsens(int fd, struct zz9k_sensors *ss)
{
    memset((char *)ss, 0, sizeof *ss);
    ss->size = sizeof *ss;
    return ioctl(fd, ZZ9KIOC_SENSORS, ss);
}

int
main(int argc, char **argv)
{
    struct zz9k_sensors ss;
    struct zz9k_info info;
    long avg[ZZ9K_SENS_N], lo[ZZ9K_SENS_N], hi[ZZ9K_SENS_N];
    int seen[ZZ9K_SENS_N];
    char *dev = 0, *board;
    int once = 0, interval = 2, tty, fd, i, c;

    progname = argv[0];
    while ((c = getopt(argc, argv, "1i:d:")) != -1) {
        switch (c) {
        case '1':
            once = 1;
            break;
        case 'i':
            interval = atoi(optarg);
            if (interval < 1 || interval > 60)
                usage();
            break;
        case 'd':
            dev = optarg;
            break;
        default:
            usage();
        }
    }
    if (optind != argc)
        usage();

    fd = opendev(dev);
    if (fd < 0) {
        fprintf(stderr, "%s: %s: %s\n", progname, dev ? dev : INFODEV, strerror(errno));
        return 1;
    }
    if (readsens(fd, &ss) < 0) {
        if (errno == EINVAL)
            fprintf(stderr, "%s: this driver has no sensors call\n", progname);
        else
            fprintf(stderr, "%s: sensors: %s\n", progname, strerror(errno));
        return 1;
    }
    if (ss.count == 0) {
        fprintf(stderr, "%s: no sensors on this board\n", progname);
        return 3;
    }
    board = "board";
    memset((char *)&info, 0, sizeof info);
    if (ioctl(fd, ZZ9KIOC_GETINFO, &info) == 0)
        board = (info.zorro == 3 && info.product == 1) ? "Z3660" : "ZZ9000";

    if (once) {
        for (i = 0; i < ZZ9K_SENS_N; ++i)
            if (ss.valid & (1UL << i))
                printf("%s %s\n", sensors[i].key, fmt(ss.value[i], sensors[i].kind));
        return 0;
    }

    tty = isatty(1);
    memset((char *)seen, 0, sizeof seen);
    for (;;) {
        for (i = 0; i < ZZ9K_SENS_N; ++i) {
            if (!(ss.valid & (1UL << i)))
                continue;
            if (!seen[i]) {
                avg[i] = lo[i] = hi[i] = ss.value[i];
                seen[i] = 1;
            }
            avg[i] = (ss.value[i] + 9 * avg[i]) / 10;     /* as ZTop filters */
            if (ss.value[i] < lo[i])
                lo[i] = ss.value[i];
            if (ss.value[i] > hi[i])
                hi[i] = ss.value[i];
        }
        if (tty)
            fputs("\033[H\033[2J", stdout);
        printf("ztop - %s firmware %d.%02d, every %d s (interrupt to stop)\n\n",
            board, info.fw_version >> 8, info.fw_version & 0xff, interval);
        printf("%-20s %10s %10s %10s %10s\n", "", "now", "average", "lowest", "highest");
        for (i = 0; i < ZZ9K_SENS_N; ++i) {
            if (!seen[i])
                continue;
            if (!(ss.valid & (1UL << i)))
                printf("%-20s %10s\n", sensors[i].label, "-");
            else if (sensors[i].kind == K_MHZ)
                printf("%-20s %10s\n", sensors[i].label, fmt(ss.value[i], K_MHZ));
            else
                printf("%-20s %10s %10s %10s %10s\n", sensors[i].label,
                    fmt(ss.value[i], sensors[i].kind), fmt(avg[i], sensors[i].kind),
                    fmt(lo[i], sensors[i].kind), fmt(hi[i], sensors[i].kind));
        }
        if (!tty)
            putchar('\n');
        fflush(stdout);
        sleep(interval);
        if (readsens(fd, &ss) < 0) {
            fprintf(stderr, "%s: sensors: %s\n", progname, strerror(errno));
            return 1;
        }
    }
}
