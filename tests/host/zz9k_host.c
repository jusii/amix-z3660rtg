/*
 * zz9k_host.c -- host tests of the driver (tests/host/run.sh builds it three ways).
 *
 * Copyright (c) 2026 Jussi Alanärä.  SPDX-License-Identifier: MIT
 *
 * The driver source is included whole, with kstub.h as its kernel, and with
 * `long` read as `int` while it compiles, so its longs are 32 bits as on m68k
 * (the X side's band-test does the same).  The board windows are host memory
 * mapped at the Amiga addresses: 0x10000000 (128 MB, the Z3660 window; the
 * ZZ9000 rows use its first 4 MB) and the custom-chip page at 0xdff000 for the
 * chipset check.  Registers are plain memory: a write is kept, a blitter
 * trigger draws nothing, so drawing is checked on the CPU paths.
 *
 * Configurations (one per build, run.sh): ZZ9K_MODULE with the Z3660 rows (the
 * module), the static ZZ9000-only driver, the static Z3660 driver.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/mman.h>

#define long int
#define printf kprintf
#include "kstub.h"
#include "zz9000.c"
#undef printf
#undef long

struct zz9k_bootinfo bootinfo;
struct cred kroot = { 0 }, kuser = { 100 };
char uspace[USPACE];
#ifdef ZZ9K_MODULE
struct screen kscreen, kother;
struct screen *displayedscreen = &kscreen;
struct console kconsole = { &kscreen }, kconsole2 = { &kother };
#else
void (*conputc)();
#endif
#ifndef ZZ9K_NO_BUILTINFONT
struct zz9k_font *builtinfont[1];
#endif

#define WIN     0x10000000UL
#define WINSZ   0x08000000UL
#define RW      (FREAD | FWRITE)

static int nfail, npass;

#define CHECK(cond, what) \
    do { if (cond) ++npass; else { ++nfail; \
        fprintf(stdout, "FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)
#define EQ(a, b, what) \
    do { long long _a = (long long)(a), _b = (long long)(b); if (_a == _b) ++npass; \
        else { ++nfail; fprintf(stdout, "FAIL %s:%d: %s: got %lld, want %lld\n", \
            __FILE__, __LINE__, what, _a, _b); } } while (0)

static volatile unsigned int *
reg32(off)
unsigned int off;
{
    return (volatile unsigned int *)(WIN + off);
}

static volatile unsigned short *
reg16(off)
unsigned int off;
{
    return (volatile unsigned short *)(WIN + off);
}

/* ioctl with an argument copied to user space at uoff (0: arg is the value itself) */
static int krval;

static int
ioc(cmd, uoff, p, n, mode, cr)
int cmd;
unsigned int uoff;
void *p;
int n;
int mode;
struct cred *cr;
{
    int e;

    if (p)
        memcpy(uspace + uoff, p, n);
    krval = -12345;
    e = zz9000ioctl(0, cmd, (int)uoff, mode, cr, &krval);
    if (p)
        memcpy(p, uspace + uoff, n);
    return e;
}

static int
iocv(cmd, value, mode, cr)
int cmd, value, mode;
struct cred *cr;
{
    krval = -12345;
    return zz9000ioctl(0, cmd, value, mode, cr, &krval);
}

static int
kread(off, n, cr)
long off, n;
struct cred *cr;
{
    static char buf[4096];
    struct uio u;

    u.uio_offset = (int)off;
    u.uio_resid = (int)n;
    u.uio_ubase = buf;
    return zz9000read(0, &u, cr);
}

static void
setup()
{
    void *p;

    p = mmap((void *)WIN, WINSZ, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
    if (p != (void *)WIN) {
        fprintf(stderr, "zz9k_host: cannot map the board window at 0x%lx\n", WIN);
        exit(2);
    }
    p = mmap((void *)0xdff000UL, 4096, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)0xdff000UL) {
        fprintf(stderr, "zz9k_host: cannot map the custom-chip page\n");
        exit(2);
    }
    *(volatile unsigned short *)0xdff004UL = 0x2300;     /* Alice: AGA */
#ifdef ZZ9K_TARGET_Z3660
    *reg32(ZZ9K_Z3660_REG_FW_VERSION) = 0x0103;
    kboards[0].pc = ZZ9K_Z3660_PRODUCT_PC;
    kboards[0].base = (long)WIN;
    kboards[0].size = (long)WINSZ;
#else
    *reg16(ZZ9K_REG_HW_VERSION) = 0x0002;
    *reg16(ZZ9K_REG_FW_VERSION) = 0x010d;
    kboards[0].pc = ZZ9K_PRODUCT_PC_Z2;
    kboards[0].base = (long)WIN;
    kboards[0].size = 0x00400000L;
#endif
}

/* ------------------------------------------------------------------ */

static void
t_privilege()
{
    struct zz9k_info info;
    struct zz9k_mode m;
    struct zz9k_reg zr;
    int sw;

    EQ(ioc(ZZ9KIOC_GETINFO, 64, &info, sizeof info, FREAD, &kuser), 0,
        "GETINFO, read-only, unprivileged");
    m.width = 1280; m.height = 720; m.color_mode = ZZ9K_COLOR_16BIT565; m.scale_mode = 0;
    EQ(ioc(ZZ9KIOC_SETMODE, 64, &m, sizeof m, FREAD, &kroot), EBADF,
        "SETMODE on a read-only descriptor");
    EQ(ioc(ZZ9KIOC_SETMODE, 64, &m, sizeof m, RW, &kuser), 0,
        "SETMODE, read-write, unprivileged");
    zr.offset = 0x0a; zr.value = 0;
#ifdef ZZ9K_TARGET_Z3660
    zr.offset = ZZ9K_Z3660_REG_PAN;
#endif
    EQ(ioc(ZZ9KIOC_SETREG, 64, &zr, sizeof zr, RW, &kuser), EPERM, "SETREG unprivileged");
    EQ(ioc(ZZ9KIOC_SETREG, 64, &zr, sizeof zr, FREAD, &kroot), EBADF, "SETREG read-only");
    EQ(ioc(ZZ9KIOC_SETREG, 64, &zr, sizeof zr, RW, &kroot), 0, "SETREG as root");
    EQ(ioc(ZZ9KIOC_GETREG, 64, &zr, sizeof zr, RW, &kuser), EPERM, "GETREG unprivileged");
    EQ(ioc(ZZ9KIOC_GETREG, 64, &zr, sizeof zr, FREAD, &kroot), 0, "GETREG as root, read-only");
    EQ(iocv(ZZ9KIOC_CONSOLE, ZZ9K_CONSOLE_ENABLE, RW, &kuser), EPERM, "CONSOLE unprivileged");
    EQ(iocv(ZZ9KIOC_CONSOLE, ZZ9K_CONSOLE_ENABLE, FREAD, &kroot), EBADF, "CONSOLE read-only");
    EQ(iocv(ZZ9KIOC_PROBE, 0, RW, &kuser), EPERM, "PROBE unprivileged");
    EQ(iocv(ZZ9KIOC_PROBE, 0, FREAD, &kroot), 0, "PROBE as root");
    EQ(iocv(ZZ9KIOC_SETSWITCH, ZZ9K_SWITCH_RTG, RW, &kuser), EPERM, "SETSWITCH unprivileged");
    EQ(iocv(ZZ9KIOC_SETSWITCH, ZZ9K_SWITCH_RTG, RW, &kroot), 0, "SETSWITCH as root");
    sw = SVGAMONITORSWITCH_SVGA;
    EQ(ioc(SVGAIOCSetMonitorSwitch, 64, &sw, sizeof sw, FREAD, &kroot), EBADF,
        "monitor-switch Set on a read-only descriptor");
    EQ(ioc(SVGAIOCSetMonitorSwitch, 64, &sw, sizeof sw, RW, &kuser), 0,
        "monitor-switch Set, read-write");
    EQ(ioc(SVGAIOCGetMonitorSwitch, 64, &sw, sizeof sw, FREAD, &kuser), 0,
        "monitor-switch Get, read-only");
    EQ(ioc(ZZ9KIOC_GETINFO, 4, &info, sizeof info, FREAD, &kuser), EFAULT,
        "a bad user address gives EFAULT");
    /* read(2): the register block is privileged, the framebuffer is not */
#ifdef ZZ9K_TARGET_Z3660
    EQ(kread(0L, 16L, &kuser), 0, "Z3660 device space is all framebuffer");
#else
    EQ(kread(0L, 16L, &kuser), EPERM, "register block read unprivileged");
    EQ(kread(0L, 16L, &kroot), 0, "register block read as root");
    EQ(kread((long)ZZ9K_FB_OFFSET, 16L, &kuser), 0, "framebuffer read unprivileged");
#endif
}

static int
regop(cmd, off, value)
int cmd;
unsigned int off, value;
{
    struct zz9k_reg zr;

    zr.offset = (unsigned short)off;
    zr.value = (unsigned short)value;
    return ioc(cmd, 64, &zr, sizeof zr, RW, &kroot);
}

static void
t_allowlist()
{
    struct zz9k_board *bp = &zz9k_boards[0];
    unsigned int off;
    int bad;

#ifdef ZZ9K_TARGET_Z3660
    /* never: ZZ_CONFIG and the ethernet mailbox, either lane, either way */
    bad = 0;
    for (off = 0x104; off < 0x108; off += 2)
        bad += regop(ZZ9KIOC_SETREG, off, 0) != EINVAL || regop(ZZ9KIOC_GETREG, off, 0) != EINVAL;
    for (off = 0x190; off < 0x1b0; off += 2)
        bad += regop(ZZ9KIOC_SETREG, off, 0) != EINVAL || regop(ZZ9KIOC_GETREG, off, 0) != EINVAL;
    EQ(bad, 0, "0x104 and 0x190-0x1AC refused for GETREG and SETREG");
    for (off = 0x1b0, bad = 0; off < 0x1000; off += 2)
        if (off < 0x280 || off >= 0x288)
            if (off < 0x318 || off >= 0x31c)
                bad += regop(ZZ9KIOC_SETREG, off, 0) != EINVAL;
    EQ(bad, 0, "nothing from 0x1B0 is writable");
    EQ(regop(ZZ9KIOC_SETREG, 0x1000, 0), EINVAL, "beyond the page");
    EQ(regop(ZZ9KIOC_SETREG, 0x121, 0), EINVAL, "odd offset");
    EQ(regop(ZZ9KIOC_SETREG, 0x13c, 0), EINVAL, "FILLTEMPLATE");
    EQ(regop(ZZ9KIOC_GETREG, 0x318, 0), 0, "monitor_switch readable");
    EQ(regop(ZZ9KIOC_SETREG, 0x318, 0), EINVAL, "monitor_switch not writable");
    EQ(regop(ZZ9KIOC_GETREG, 0x17c, 0), 0, "VBLANK readable");
    /* PAN through the HI/LO composition: one 32-bit write */
    *reg32(0x120) = 0;
    EQ(regop(ZZ9KIOC_SETREG, 0x120, 0x0012), 0, "PAN HI latched");
    EQ(*reg32(0x120), 0, "a HI poke writes nothing");
    bp->blitvalid = ~0UL;
    EQ(regop(ZZ9KIOC_SETREG, 0x122, 0x3400), 0, "PAN LO writes");
    EQ(*reg32(0x120), 0x00123400, "the whole word written");
    EQ(bp->blitvalid, 0, "SETREG empties the blitter cache");
#else
    bad = 0;
    for (off = 0x04; off < 0x0a; off += 2)
        bad += regop(ZZ9KIOC_SETREG, off, 0) != EINVAL;
    for (off = 0x60; off < 0xc0; off += 2)
        bad += regop(ZZ9KIOC_SETREG, off, 0) != EINVAL || regop(ZZ9KIOC_GETREG, off, 0) != EINVAL;
    for (off = 0xc2; off < 0x2000; off += 2)
        bad += regop(ZZ9KIOC_SETREG, off, 0) != EINVAL;
    EQ(bad, 0, "CONFIG, ethernet/USB and the capture block refused");
    EQ(regop(ZZ9KIOC_GETREG, ZZ9K_REG_FW_VERSION, 0), 0, "FW_VERSION readable");
    EQ(regop(ZZ9KIOC_SETREG, ZZ9K_REG_FW_VERSION, 0), EINVAL, "FW_VERSION not writable");
    bp->blitvalid = ~0UL;
    EQ(regop(ZZ9KIOC_SETREG, ZZ9K_REG_BLITTER_X2, 1920), 0, "X2 writable (the pan fix)");
    EQ(*reg16(ZZ9K_REG_BLITTER_X2), 1920, "X2 written");
    EQ(bp->blitvalid, 0, "SETREG empties the blitter cache");
    EQ(regop(ZZ9KIOC_SETREG, ZZ9K_REG_PAN_LO, 0), 0, "PAN_LO writable");
#endif
}

static void
t_band()
{
    long keep = zz9k_blit_maxarea;

    EQ(ZZ9K_BLIT_MAXAREA_DEFAULT, 8192, "the default band");
    zz9k_blit_maxarea = 0;
    EQ(zz9k_band(1920, 1080), 4, "0 means the default: 1920 px rows, 4 per band");
    zz9k_blit_maxarea = ZZ9K_BLIT_MAXAREA_DEFAULT;
    EQ(zz9k_band(1920, 1080), 4, "1920x1080: 4 rows per band");
    EQ(zz9k_band(100, 81), 81, "8100 px fits one band");
    EQ(zz9k_band(100, 82), 81, "8200 px splits at 81 rows");
    EQ(zz9k_band(2560, 1440), 3, "2560 px rows");
    zz9k_blit_maxarea = 100;
    EQ(zz9k_band(2560, 2), 1, "at least one row");
    zz9k_blit_maxarea = 3686400L;
    EQ(zz9k_band(2560, 1440), 1440, "the limit never splits");
    zz9k_blit_maxarea = keep;
}

static void
t_rect()
{
    struct zz9k_board *bp = &zz9k_boards[0];

    bp->width = 1280; bp->height = 720;
    CHECK(zz9k_rect_ok(bp, 0UL, 0UL, 1280UL, 720UL), "full screen is on screen");
    CHECK(!zz9k_rect_ok(bp, 0UL, 0UL, 0UL, 1UL), "empty width");
    CHECK(!zz9k_rect_ok(bp, 1UL, 0UL, 1280UL, 1UL), "one past the right edge");
    CHECK(!zz9k_rect_ok(bp, 0UL, 719UL, 1UL, 2UL), "one past the bottom");
    CHECK(!zz9k_rect_ok(bp, 0xffffUL, 0UL, 2UL, 1UL), "x beyond the screen");
}

#ifdef ZZ9K_MODULE
static void
t_params()
{
    char buf[128];

    zz9k_blit_on = 1;
    strcpy(buf, "blit=0");
    zz9k_params(buf);
    EQ(zz9k_blit_on, 0, "blit=0");
    strcpy(buf, "blit=yes");
    zz9k_params(buf);
    EQ(zz9k_blit_on, 0, "a bad bool keeps the value");
    strcpy(buf, "  blit=1   maxarea=4096 ");
    zz9k_params(buf);
    EQ(zz9k_blit_on, 1, "blit=1 among spaces");
    EQ(zz9k_blit_maxarea, 4096, "maxarea=4096");
    strcpy(buf, "maxarea=99999999");
    zz9k_params(buf);
    EQ(zz9k_blit_maxarea, 4096, "maxarea over the limit is refused");
    strcpy(buf, "maxarea= nosuch=3 =");
    zz9k_params(buf);
    EQ(zz9k_blit_maxarea, 4096, "an empty value is refused");
    zz9k_params((char *)0);
}
#endif

int
main(argc, argv)
int argc;
char **argv;
{
    if (argc > 1 && strcmp(argv[1], "-v") == 0)
        kverbose = 1;
    setup();
#ifdef ZZ9K_MODULE
    zz9k_params((char *)0);
    if (zz9000_load((char *)0) != 0) {
        printf("FAIL: the module did not load\n%s", klog);
        return 1;
    }
    EQ(zz9k_blit_maxarea, 8192, "maxarea defaults to 8192");
    t_params();
    zz9k_blit_on = 1;
    zz9k_blit_maxarea = ZZ9K_BLIT_MAXAREA_DEFAULT;
#else
    zz9000init();
#endif
    t_privilege();
    t_allowlist();
    t_band();
    t_rect();
    printf("%s: %d passed, %d failed\n", ZZ9K_HOST_CONFIG, npass, nfail);
    return nfail != 0;
}
