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
static int cell_is();

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
    m.scale_mode = 3;
    EQ(ioc(ZZ9KIOC_SETMODE, 64, &m, sizeof m, RW, &kuser), 0, "scale_mode 3");
#ifdef ZZ9K_TARGET_Z3660
    EQ((*reg32(ZZ9K_Z3660_REG_MODE) >> 12) & 0xf, 3, "scale in MODE bits 12-15");
#endif
    m.scale_mode = 4;
    EQ(ioc(ZZ9KIOC_SETMODE, 64, &m, sizeof m, RW, &kuser), EINVAL, "scale_mode 4 refused");
    m.scale_mode = 0x10;
    EQ(ioc(ZZ9KIOC_SETMODE, 64, &m, sizeof m, RW, &kuser), EINVAL,
        "a scale_mode that would spill into the next field refused");
    m.scale_mode = 0;
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

/* records at user offset 4096, the header at 64 */
static int
blits(recs, n)
struct zz9k_blit *recs;
int n;
{
    struct zz9k_blits bs;

    memcpy(uspace + 4096, recs, n * sizeof *recs);
    bs.recs = (struct zz9k_blit *)4096;
    bs.n = n;
    return ioc(ZZ9KIOC_BLITS, 64, &bs, sizeof bs, RW, &kuser);
}

static void
setmode(w, h)
int w, h;
{
    struct zz9k_mode m;

    m.width = w; m.height = h; m.color_mode = ZZ9K_COLOR_16BIT565; m.scale_mode = 0;
    EQ(ioc(ZZ9KIOC_SETMODE, 64, &m, sizeof m, RW, &kroot), 0, "SETMODE");
}

#ifdef ZZ9K_TARGET_Z3660
static int kcopyins;

static void
other_process()
{
    if (++kcopyins == 3)
        *reg32(ZZ9K_Z3660_REG_ROW_PITCH) = 0;
}
#endif

static void
t_blits()
{
    static struct zz9k_blit r[40];
    int i;

#ifdef ZZ9K_TARGET_Z3660
    setmode(1920, 1080);
#else
    setmode(1280, 720);
#endif
    for (i = 0; i < 40; ++i) {
        memset(&r[i], 0, sizeof r[i]);
        r[i].op = ZZ9K_BLIT_FILL;
        r[i].width = 1920;
        r[i].height = 1080;
    }
#ifndef ZZ9K_TARGET_Z3660
    EQ(blits(r, 2), ENXIO, "no blitter on the ZZ9000 rows");
    return;
#endif
    EQ(blits(r, 0), 0, "an empty batch");
    EQ(krval, 0, "returns 0");
    EQ(blits(r, 8), 0, "eight full-screen fills");
    EQ(krval, 2, "the 4 Mpx budget ends the call after two");
    r[0].width = 100; r[0].height = 100;
    r[1].width = 100; r[1].height = 100;
    EQ(blits(r, 2), 0, "two small fills");
    EQ(krval, 2, "both run");
    r[0].width = 1921;
    EQ(blits(r, 2), EINVAL, "record 0 off screen");
    r[0].width = 100;
    r[0].op = 7;
    EQ(blits(r, 2), EINVAL, "record 0 neither FILL nor COPY");
    r[0].op = ZZ9K_BLIT_FILL;
    for (i = 0; i < 40; ++i) {
        r[i].width = 10; r[i].height = 10;
    }
    r[11].op = ZZ9K_BLIT_COPY;
    r[11].src_x = 1915;
    EQ(blits(r, 20), 0, "a bad copy source at record 11");
    EQ(krval, 11, "returns 11: records 0-10 ran");
    r[11].src_x = 0;
    EQ(blits(r, 40), 0, "forty small records");
    EQ(krval, 40, "all run");
    EQ(blits(r, ZZ9K_BLITS_MAX + 1), EINVAL, "more than ZZ9K_BLITS_MAX");
    {
        struct zz9k_blits bs;

        bs.recs = (struct zz9k_blit *)(USPACE - 3 * sizeof r[0]);
        bs.n = 16;
        memcpy(uspace + USPACE - 3 * sizeof r[0], r, 3 * sizeof r[0]);
        EQ(ioc(ZZ9KIOC_BLITS, 64, &bs, sizeof bs, RW, &kuser), EFAULT,
            "records past the end of user space: nothing ran");
        bs.recs = (struct zz9k_blit *)(USPACE - 8 * sizeof r[0]);
        memcpy(uspace + USPACE - 8 * sizeof r[0], r, 8 * sizeof r[0]);
        EQ(ioc(ZZ9KIOC_BLITS, 64, &bs, sizeof bs, RW, &kuser), 0,
            "the second group unreadable");
        EQ(krval, 8, "returns the first group's 8");
    }
#ifdef ZZ9K_TARGET_Z3660
    kcopyins = 0;
    kcopyin_hook = other_process;
    EQ(blits(r, 9), 0, "nine records: three copyins");
    kcopyin_hook = 0;
    EQ(*reg32(ZZ9K_Z3660_REG_ROW_PITCH), 1920 * 2 / 4,
        "a register changed while a copyin slept is written again");
#endif
    zz9k_blit_on = 0;
    EQ(blits(r, 2), ENXIO, "blitter patched off");
    zz9k_blit_on = 1;
}

static int
cwrite(str)
char *str;
{
    return zz9000consolewrite((unsigned char *)str, (unsigned)strlen(str));
}

#ifdef ZZ9K_MONSW
static void
nested_writer()
{
    cwrite("Q");                /* as from an interrupt inside the renderer */
}
#endif

#ifdef ZZ9K_MODULE
static int
other_hook()
{
    return 0;
}
#endif

static void
t_console_enable()
{
#ifdef ZZ9K_MODULE
    unsigned int mode;

    setmode(1280, 720);
    mode = *reg32(ZZ9K_Z3660_REG_MODE);
    khook = other_hook;         /* another driver has the console */
    EQ(iocv(ZZ9KIOC_CONSOLE, ZZ9K_CONSOLE_ENABLE, RW, &kroot), EBUSY, "the hook is taken");
    EQ(*reg32(ZZ9K_Z3660_REG_MODE), mode, "the display is left as it was");
    EQ(zz9k_console_on, 0, "the console stays off");
    CHECK(khook == other_hook, "the other hook is kept");
    khook = 0;
    EQ(iocv(ZZ9KIOC_CONSOLE, ZZ9K_CONSOLE_REDIRECT, RW, &kroot), 0, "REDIRECT");
    EQ(iocv(ZZ9KIOC_CONSOLE, ZZ9K_CONSOLE_ENABLE, RW, &kroot), 0, "ENABLE again");
    CHECK(khook == zz9k_cons_hook && zz9k_console_on && zz9k_console_redirect,
        "the console restarted, the hook kept");
    EQ(iocv(ZZ9KIOC_CONSOLE, ZZ9K_CONSOLE_DISABLE, RW, &kroot), 0, "DISABLE");
    CHECK(khook == 0, "the hook cleared");
    zz9k_boards[0].fb_size = 1000;      /* too small for the console's mode */
    EQ(iocv(ZZ9KIOC_CONSOLE, ZZ9K_CONSOLE_ENABLE, RW, &kroot), ENOMEM, "start fails");
    CHECK(khook == 0, "the hook is rolled back");
    zz9k_boards[0].fb_size = ZZ9K_Z3660_FB_MAX;
#endif
}

static void
t_csi()
{
    zz9k_blit_on = 0;
    EQ(iocv(ZZ9KIOC_CONSOLE, ZZ9K_CONSOLE_ENABLE, RW, &kroot), 0, "console on");
    cwrite("ab\033[?25lc");
    EQ(zz9k_console_col, 3, "ESC [ ? 25 l is swallowed whole");
    CHECK(cell_is(0, 2, 'c'), "and c drawn after it");
    cwrite("\r\033[99999999999999C");
    EQ(zz9k_console_col, zz9k_console_cols - 1, "a huge count clamps, no overflow");
    cwrite("\r\033[5C");
    EQ(zz9k_console_col, 5, "ESC [ 5 C");
    cwrite("\007\001\177");
    EQ(zz9k_console_col, 5, "BEL, SOH and DEL draw nothing");
    CHECK(cell_is(0, 6, ' '), "no ? drawn for them");
    cwrite("\033[3\033[2D");
    EQ(zz9k_console_col, 3, "a new ESC abandons the sequence before it");
    cwrite("\033[1\030x");
    EQ(zz9k_console_col, 4, "CAN abandons the sequence; the next byte draws");
    cwrite("\033[1 qy");
    EQ(zz9k_console_col, 5, "an intermediate byte: the sequence has no effect");
    cwrite("\033[2\rC");
    EQ(zz9k_console_col, 2, "a control inside a sequence acts (CR, then 2 right)");
    EQ(zz9k_console_esc, ZZ9K_ESC_NONE, "and the final byte ends it");
    cwrite("\033[2;3H");
    CHECK(zz9k_console_row == 1 && zz9k_console_col == 2, "ESC [ 2 ; 3 H");
    EQ(iocv(ZZ9KIOC_CONSOLE, ZZ9K_CONSOLE_DISABLE, RW, &kroot), 0, "console off");
    zz9k_blit_on = 1;
}

static struct {
    struct zz9k_font f;
    unsigned char data[16];
} kfont;

static void
t_font()
{
    unsigned short *p;
    int i;

    memset(&kfont, 0, sizeof kfont);
    kfont.f.f_magic = ZZ9K_FONT_MAGIC;
    kfont.f.f_height = 0;
    kfont.f.f_ctable['M'].ce_move = 7;
    builtinfont[0] = &kfont.f;
    zz9k_blit_on = 0;
    EQ(iocv(ZZ9KIOC_CONSOLE, ZZ9K_CONSOLE_ENABLE, RW, &kroot), 0, "console on");
    CHECK(zz9k_console_font == 0 && zz9k_console_font_w == ZZ9K_FALLBACK_FONT_W,
        "a font of height 0 is refused");
    kfont.f.f_height = 8;
    kfont.f.f_ctable['M'].ce_move = 16;
    EQ(iocv(ZZ9KIOC_CONSOLE, ZZ9K_CONSOLE_ENABLE, RW, &kroot), 0, "console again");
    CHECK(zz9k_console_font == 0, "a font wider than 16 is refused");
    kfont.f.f_ctable['M'].ce_move = 7;
    kfont.f.f_ctable['A'].ce_height = 8;
    kfont.f.f_ctable['A'].ce_width = 8;
    kfont.f.f_ctable['A'].ce_bpl = 1;
    kfont.f.f_ctable['A'].ce_data = (unsigned short)((char *)kfont.data - (char *)&kfont);
    for (i = 0; i < 8; ++i)
        kfont.data[i] = 0x81;
    kfont.f.f_ctable['W'] = kfont.f.f_ctable['A'];
    kfont.f.f_ctable['W'].ce_width = 40;        /* a bad width: drawn clipped, no overrun */
    EQ(iocv(ZZ9KIOC_CONSOLE, ZZ9K_CONSOLE_ENABLE, RW, &kroot), 0, "console with the font");
    CHECK(zz9k_console_font == &kfont.f && zz9k_console_font_w == 8, "an 8x8 font is used");
    cwrite("AW\n");             /* the cursor off row 0 */
    p = (unsigned short *)zz9k_console_bp->fb + (unsigned long)zz9k_con_top * zz9k_con_w;
    CHECK(p[0] == ZZ9K_FG && p[1] == ZZ9K_BG && p[7] == ZZ9K_FG, "the font's A drawn");
    CHECK(p[8] == ZZ9K_FG && p[15] == ZZ9K_FG && p[16] != ZZ9K_FG, "W stays in its cell");
    EQ(iocv(ZZ9KIOC_CONSOLE, ZZ9K_CONSOLE_DISABLE, RW, &kroot), 0, "console off");
    builtinfont[0] = 0;
    zz9k_blit_on = 1;
}

static void
t_console_pause()
{
    struct zz9k_conputs cp;

    EQ(iocv(ZZ9KIOC_CONSOLE, ZZ9K_CONSOLE_ENABLE, RW, &kroot), 0, "console on");
    EQ(cwrite("x"), 1, "the console takes writes");
    setmode(1024, 768);
    EQ(zz9k_console_paused, 1, "a client's SETMODE pauses it");
    EQ(cwrite("x"), 0, "writes go to the native screen meanwhile");
    cp.buf = (char *)8192;
    cp.len = 1;
    EQ(ioc(ZZ9KIOC_CONPUTS, 64, &cp, sizeof cp, RW, &kuser), EBUSY, "CONPUTS while paused");
    EQ(zz9000close(0, RW, 0, &kroot), 0, "the client's last close");
    EQ(zz9k_console_paused, 0, "resumed");
    EQ(zz9k_boards[0].width, zz9k_con_w, "in the console's mode");
    EQ(cwrite("x"), 1, "the console takes writes again");
    EQ(iocv(ZZ9KIOC_CONSOLE, ZZ9K_CONSOLE_DISABLE, RW, &kroot), 0, "console off");
}

static void
t_console_guard()
{
    static char big[300];
    int col;

    EQ(iocv(ZZ9KIOC_CONSOLE, ZZ9K_CONSOLE_ENABLE, RW, &kroot), 0, "console on");
#ifdef ZZ9K_MODULE
    CHECK(khook == zz9k_cons_hook, "the module's hook is set");
    EQ((*khook)(&kconsole2, (unsigned char *)"zz", 2), 0, "a console not on display is passed on");
#else
    CHECK(conputc == zz9k_kputc, "conputc goes through the guard");
#endif
    EQ(cwrite("ab"), 1, "a write is taken");
    EQ(zz9k_console_col, 2, "two columns");
    EQ(kspl, 0, "the level is back");
    zz9k_con_busy = 1;          /* the renderer is in use below us */
    EQ(cwrite("XYZ"), 1, "a nested write is taken");
    EQ(zz9k_console_col, 2, "and not drawn yet");
    EQ((zz9k_con_head - zz9k_con_tail) & (ZZ9K_CON_RING - 1), 3, "three bytes kept");
    zz9k_con_busy = 0;
    cwrite("c");
    EQ(zz9k_console_col, 6, "the owner drew its byte and then the three kept");
    EQ(zz9k_con_head, zz9k_con_tail, "the ring is empty");
#ifdef ZZ9K_MONSW
    kmonsw_hook = nested_writer;
    col = zz9k_console_col;
    cwrite("de");
    EQ(zz9k_console_col, col + 3, "a writer inside the renderer is drawn after it");
    EQ(zz9k_con_busy, 0, "the renderer is free");
#endif
    memset(big, 'x', sizeof big);
    zz9k_con_lost = 0;
    zz9k_con_busy = 1;
    zz9000consolewrite((unsigned char *)big, (unsigned)sizeof big);
    EQ(zz9k_con_lost, (long)sizeof big - (ZZ9K_CON_RING - 1), "what does not fit is counted");
    zz9k_con_busy = 0;
    zz9000consolewrite((unsigned char *)big, 0U);
    EQ(zz9k_con_head, zz9k_con_tail, "drained");
    {
        struct zz9k_conputs cp;

        memcpy(uspace + 8192, "\r12", 3);
        cp.buf = (char *)8192;
        cp.len = 3;
        zz9k_con_busy = 1;
        EQ(ioc(ZZ9KIOC_CONPUTS, 64, &cp, sizeof cp, RW, &kuser), 0, "CONPUTS");
        EQ((zz9k_con_head - zz9k_con_tail) & (ZZ9K_CON_RING - 1), 3, "CONPUTS goes through the guard");
        zz9k_con_busy = 0;
        cwrite("");
        EQ(zz9k_console_col, 2, "drawn by the next owner");
    }
    EQ(kspl, 0, "the level is back");
    EQ(iocv(ZZ9KIOC_CONSOLE, ZZ9K_CONSOLE_DISABLE, RW, &kroot), 0, "console off");
    EQ(cwrite("ab"), 0, "a write is passed on when off");
}

/* the console cell (row, col) on the screen shown matches the fallback glyph of c */
static int
cell_is(row, col, c)
int row, col;
int c;
{
    unsigned short *p;
    int px, py, on;

    for (py = 0; py < zz9k_console_font_h; ++py) {
        p = (unsigned short *)zz9k_console_bp->fb +
            (unsigned long)(zz9k_con_top + row * zz9k_console_font_h + py) * zz9k_con_w +
            col * zz9k_console_font_w;
        for (px = 0; px < zz9k_console_font_w; ++px) {
            on = px < ZZ9K_FALLBACK_FONT_W && (zz9k_glyph((unsigned char)c, py) & (0x10 >> px));
            if (p[px] != (on ? ZZ9K_FG : ZZ9K_BG))
                return 0;
        }
    }
    return 1;
}

static void
t_console_scroll()
{
    char line[16];
    int i, bad, rebases, lasttop, stride;

    zz9k_blit_on = 0;           /* registers draw nothing here: check the CPU paths */
    EQ(iocv(ZZ9KIOC_CONSOLE, ZZ9K_CONSOLE_ENABLE, RW, &kroot), 0, "console on");
    stride = zz9k_con_w * 2;
#ifdef ZZ9K_TARGET_Z3660
    EQ(zz9k_con_ringh, 2 * zz9k_con_h + zz9k_console_font_h, "a pan ring on the Z3660");
#else
    EQ(zz9k_con_ringh, zz9k_con_h, "no pan room in the ZZ9000's Zorro II window");
#endif
    for (i = 0; i < zz9k_console_rows; ++i)
        cwrite("\n");          /* the cursor on the last row: every line scrolls from here */
    bad = 0;
    rebases = 0;
    lasttop = zz9k_con_top;
    for (i = 0; i < 400; ++i) {
        sprintf(line, "L%03d\n", i);
        cwrite(line);
        if (zz9k_con_top < lasttop)
            ++rebases;
        lasttop = zz9k_con_top;
#ifdef ZZ9K_TARGET_Z3660
        if (*reg32(ZZ9K_Z3660_REG_PAN) != (unsigned int)(zz9k_con_top * stride))
            ++bad;
        if (*reg32(ZZ9K_Z3660_REG_X2) != (unsigned int)zz9k_con_w)
            ++bad;
#endif
        if (i < 2)
            continue;
        sprintf(line, "L%03d", i);
        if (!cell_is(zz9k_console_rows - 2, 0, 'L') || !cell_is(zz9k_console_rows - 2, 3, line[3]))
            ++bad;
        sprintf(line, "L%03d", i - 1);
        if (!cell_is(zz9k_console_rows - 3, 2, line[2]) || !cell_is(zz9k_console_rows - 3, 3, line[3]))
            ++bad;
        if (!cell_is(zz9k_console_rows - 1, 1, ' ') || !cell_is(zz9k_console_rows - 1, 7, ' '))
            ++bad;
    }
    EQ(bad, 0, "400 lines: the last two lines and a clean bottom row after every scroll");
    zz9k_blit_inuse = 1;        /* as if the write interrupted a blitter operation */
    lasttop = zz9k_con_top;
    cwrite("Mxy\n");
    EQ(zz9k_con_top, lasttop, "no pan while the blitter registers are in use");
    CHECK(cell_is(zz9k_console_rows - 2, 0, 'M') && cell_is(zz9k_console_rows - 3, 0, 'L'),
        "the copy scroll in place");
    zz9k_blit_inuse = 0;
#ifdef ZZ9K_TARGET_Z3660
    {
        struct zz9k_pan pan;

        pan.offset = 4096;
        *reg32(ZZ9K_Z3660_REG_X1) = 77;
        *reg32(ZZ9K_Z3660_REG_Y1) = 5;
        EQ(ioc(ZZ9KIOC_SETPAN, 64, &pan, sizeof pan, RW, &kuser), 0, "SETPAN");
        EQ(*reg32(ZZ9K_Z3660_REG_PAN), 4096, "PAN written");
        CHECK(*reg32(ZZ9K_Z3660_REG_X1) == 0 && *reg32(ZZ9K_Z3660_REG_Y1) == 0 &&
            *reg32(ZZ9K_Z3660_REG_X2) == (unsigned int)zz9k_boards[0].width,
            "X1, Y1 and X2 set first, so the firmware adds nothing");
    }
#endif
    zz9k_blit_on = 1;
    for (i = 0; i < 300; ++i)
        cwrite("blitter path\n");    /* the blitter paths run (they draw nothing here) */
    EQ(zz9k_blit_inuse, 0, "the in-use count returns to 0");
#ifdef ZZ9K_TARGET_Z3660
    CHECK(rebases >= 2, "the ring went round at least twice");
#endif
    EQ(iocv(ZZ9KIOC_CONSOLE, ZZ9K_CONSOLE_DISABLE, RW, &kroot), 0, "console off");
    zz9k_blit_on = 1;
}

static void
t_cpu_loops()
{
    static unsigned short buf[64];
    int i, bad;

    for (i = 0; i < 64; ++i)
        buf[i] = 0x1111;
    zz9k_fill16(buf + 1, 9L, 0xabcd);
    bad = buf[0] != 0x1111 || buf[10] != 0x1111;
    for (i = 1; i < 10; ++i)
        bad += buf[i] != 0xabcd;
    EQ(bad, 0, "fill16 from an odd pixel");
    for (i = 0; i < 64; ++i)
        buf[i] = (unsigned short)i;
    zz9k_copy16(buf + 20, buf + 31, 10L);        /* different alignment */
    zz9k_copy16(buf + 40, buf + 2, 7L);          /* same alignment */
    bad = 0;
    for (i = 0; i < 10; ++i)
        bad += buf[20 + i] != 31 + i;
    for (i = 0; i < 7; ++i)
        bad += buf[40 + i] != 2 + i;
    bad += buf[19] != 19 || buf[30] != 30 || buf[47] != 47;
    EQ(bad, 0, "copy16 with either alignment");
}

static void
t_fill()
{
    struct zz9k_fill f;
    unsigned char *fb = (unsigned char *)zz9k_boards[0].fb;
    unsigned long n = zz9k_boards[0].fb_size;
    int bad, i;

    memset(fb, 0x11, 3 * ZZ9K_FILL_MAX);
    f.offset = 3; f.bytes = 2 * ZZ9K_FILL_MAX; f.value = 0x1a5;
    EQ(ioc(ZZ9KIOC_FILL, 64, &f, sizeof f, RW, &kuser), 0, "FILL 2 MiB");
    EQ(krval, (long)ZZ9K_FILL_MAX, "one call fills 1 MiB");
    bad = fb[2] != 0x11 || fb[3 + ZZ9K_FILL_MAX] != 0x11;
    for (i = 3; i < 3 + (int)ZZ9K_FILL_MAX; ++i)
        bad += fb[i] != 0xa5;
    EQ(bad, 0, "exactly the bytes asked, the low byte of value, from an odd offset");
    f.offset = n - 5; f.bytes = 100;
    EQ(ioc(ZZ9KIOC_FILL, 64, &f, sizeof f, RW, &kuser), 0, "FILL at the end");
    EQ(krval, 5, "not past the end");
    f.offset = n;
    EQ(ioc(ZZ9KIOC_FILL, 64, &f, sizeof f, RW, &kuser), EINVAL, "FILL beyond the end");
    f.offset = 0; f.bytes = 0;
    EQ(ioc(ZZ9KIOC_FILL, 64, &f, sizeof f, RW, &kuser), 0, "FILL of nothing");
    EQ(krval, 0, "fills 0");
}

/* last: re-probes the board */
static void
t_fixed()
{
#ifdef ZZ9K_TARGET_Z3660
    struct zz9k_board *bp = &zz9k_boards[0];

    /* a ZZ9000 configured as Zorro III in the list as well: refused, the next row tries */
    kboards[1].pc = ZZ9K_PRODUCT_PC_Z3;
    kboards[1].base = 0x40000000L;
    kboards[1].size = 0x01000000L;
    bp->base = 0;
    EQ(zz9k_probe(0), 1, "a refused row no longer ends the probe");
    CHECK(bp->bt && bp->bt->autocon_pc == ZZ9K_Z3660_PRODUCT_PC, "the Z3660 found after it");
    kboards[1].pc = 0;

    kboards[0].pc = 0;          /* a machine whose AutoConfig list lacks the board */
    bp->base = 0;
    zz9k_fixed_on = 0;
    EQ(zz9k_probe(0), 0, "no fixed-base probe by default");
    zz9k_fixed_on = 1;
    klogn = 0;
    EQ(zz9k_probe(0), 0, "a failed probe is remembered");
    EQ(klogn, 0, "and says nothing the second time");
    zz9k_noboard[0] = 0;
    EQ(zz9k_probe(0), 1, "the fixed row when asked");
    CHECK(bp->bt && bp->bt->autocon_pc == 0, "attached by the fixed row");
    *(volatile unsigned short *)0xdff004UL = 0x0000;     /* OCS Agnus */
    bp->base = 0;
    EQ(zz9k_probe(0), 0, "and only on an AGA machine");
    *(volatile unsigned short *)0xdff004UL = 0x2300;
#endif
}

static void
t_layout()
{
    struct zz9k_autocon_dump d;

    EQ((char *)&d.entry[0] - (char *)&d, 4, "autocon entries at offset 4");
    EQ(sizeof d, 324, "autocon dump 324 bytes");
    EQ(sizeof(struct zz9k_blit), 20, "a BLITS record 20 bytes");
    EQ(sizeof(struct zz9k_fbinfo), 44, "fbinfo 44 bytes");
    EQ(sizeof(struct zz9k_info), 32, "info 32 bytes");
    memset(uspace + 64, 0x55, sizeof d);
    EQ(ioc(ZZ9KIOC_GETAUTOCON, 64, &d, sizeof d, FREAD, &kuser), 0, "GETAUTOCON");
    EQ(d.pad, 0, "the padding is written as 0");
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
    EQ(zz9k_fixed_on, 0, "fixed defaults to 0");
    strcpy(buf, "fixed=1");
    zz9k_params(buf);
    EQ(zz9k_fixed_on, 1, "fixed=1");
    strcpy(buf, "fixed=on");
    zz9k_params(buf);
    EQ(zz9k_fixed_on, 1, "a bad bool keeps fixed");
    zz9k_fixed_on = 0;
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
    t_blits();
    t_console_enable();
    t_console_guard();
    t_console_pause();
    t_csi();
    t_font();
    t_console_scroll();
    t_cpu_loops();
    t_fill();
    t_rect();
    t_layout();
    t_fixed();
    printf("%s: %d passed, %d failed\n", ZZ9K_HOST_CONFIG, npass, nfail);
    return nfail != 0;
}
