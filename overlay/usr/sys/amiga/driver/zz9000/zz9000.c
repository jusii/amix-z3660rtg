/*
 * zz9000.c - MNT ZZ9000 framebuffer/mode driver for Amiga UNIX.
 *
 * Copyright (c) 2026 Ignacio Soriano.  The Z3660 target, the module form and
 * later changes Copyright (c) 2026 Jussi Alanärä.  SPDX-License-Identifier: MIT
 *
 * Board identity, window layout and register file are data: one row of
 * zz9k_boardtab[] per supported board (see zz9000.h).  The default rows
 * are the ZZ9000 Zorro II product 3 (attached) and the Zorro III product 4
 * (identified and refused: its window lies outside the kernel's
 * identity-mapped low 1 GB).  A target build (-DZZ9K_TARGET_...) adds rows.
 * The DMA paths from the AmigaOS and NetBSD drivers are intentionally not
 * used here.
 */

#include "sys/types.h"
#include "sys/immu.h"
#include "sys/param.h"
#include "sys/uio.h"
#include "sys/sysmacros.h"
#include "sys/inline.h"
#include "sys/errno.h"
#include "sys/systm.h"
#include "sys/file.h"
#ifdef ZZ9K_MODULE
/* the ABI kit's stock tree (module/Makefile), not a kernel tree beside this directory */
#include "amiga/console/screen.h"
#include "amiga/console/console.h"
#else
#include "../../console/screen.h"
#include "../../console/console.h"
#endif
#include "zz9000.h"

/*
 * -DZZ9K_MODULE builds the loadable module form (amix-dlm C3-6 section 3): the same driver
 * behind the module core's wrapper, its console through the core's cons_hook_set instead of
 * the kernel's conputc, and only DDI v1 names imported.  Without it the source is the static
 * driver, unchanged.
 */
#ifdef ZZ9K_MODULE
#include "sys/conf.h"
#include "sys/moddefs.h"
#include "sys/amixddi.h"
#ifdef ZZ9K_FB_NC
#error "ZZ9K_FB_NC: hat_cm_fb_add is not in DDI v1, build the module without it"
#endif
#ifndef ZZ9K_MONSW
#error "the module depends on the monsw module (depend=monsw): build it with -DZZ9K_MONSW"
#endif
#endif

#define ZZ9K_MAXBOARDS 1
#define ZZ9K_UNIT(dev)    (getminor(dev) & ~ZZ9K_MINOR_INFO)
#define ZZ9K_ISINFO(dev)  (getminor(dev) & ZZ9K_MINOR_INFO)

#define ZZ9K_REG(base, off) \
    (*((volatile unsigned short *)((char *)(base) + (off))))

struct cred;

struct zz9k_configdev {
    struct zz9k_configdev *ln_succ;
    struct zz9k_configdev *ln_pred;
    unsigned char ln_type;
    char ln_pri;
    unsigned short ln_name[2];
    unsigned char cd_flags;
    unsigned char cd_pad;
    unsigned char er_type;
    unsigned char er_product;
    unsigned char er_flags;
    unsigned char er_reserved03;
    unsigned short er_manufacturer;
    unsigned short er_serialnumber[2];
    unsigned short er_initdiagvec;
    unsigned char er_reserved0c;
    unsigned char er_reserved0d;
    unsigned char er_reserved0e;
    unsigned char er_reserved0f;
    unsigned long cd_boardaddr;
    unsigned long cd_boardsize;
    unsigned short cd_slotaddr;
    unsigned short cd_slotsize;
    unsigned long cd_driver;
    struct zz9k_configdev *cd_nextcd;
    unsigned long cd_unused[4];
};

struct zz9k_bootinfo {
    struct zz9k_configdev autocon[ZZ9K_MAX_AUTOCON];
};

struct zz9k_charent {
    unsigned char ce_height;
    unsigned char ce_width;
    unsigned char ce_basel;
    unsigned char ce_move;
    unsigned char ce_bpl;
    unsigned char ce_pad;
    unsigned short ce_data;
};

struct zz9k_font {
    unsigned short f_magic;
    unsigned short f_flags;
    unsigned short f_count;
    unsigned short f_pad0;
    unsigned long f_length;
    unsigned char f_height;
    unsigned char f_width;
    unsigned char f_baseline;
    unsigned char f_pad1;
    struct zz9k_charent f_ctable[256];
};

extern struct zz9k_bootinfo bootinfo;
#ifdef ZZ9K_NO_BUILTINFONT
/* a kernel without the console's builtinfont[] (the 68040/68060 relinks):
 * the console uses the built-in 5x7 glyphs */
static struct zz9k_font *builtinfont[1] = { (struct zz9k_font *)0 };
#else
extern struct zz9k_font *builtinfont[];
#endif

struct zz9k_board {
    struct zz9k_boardtab *bt;
    long base;                    /* the register page, as the kernel reaches it */
    long phys;                    /* the window's physical base */
    char *fb;                     /* the framebuffer, as the kernel reaches it */
    unsigned long fb_kmap;        /* framebuffer bytes the kernel reaches (fb_size, or the mapped part) */
    short mapped;                 /* the window is mapped (zz9k_map), not identity-mapped */
    unsigned long board_size;
    unsigned long fb_size;
    unsigned short hw_version;
    unsigned short fw_version;
    unsigned short mode;
    unsigned short width;
    unsigned short height;
    unsigned short color_mode;
    unsigned short reg_hi;        /* SETREG HI-lane latch on a 32-bit row */
    short monsw_handle;           /* monsw registration, -1 = none */
    short svga_switch;            /* last SVGAIOCSetMonitorSwitch: 1 Amiga, 2 SVGA */
    unsigned long blitreg[ZZ9K_NREG];  /* blitter registers as last written */
    unsigned long blitvalid;      /* bit r: blitreg[r] is what the card holds */
};

/*
 * The blitter register cache.  Every band of a banded operation used to re-send the whole
 * register set -- 9 writes for a fill, 11 for a copy -- and the card's firmware handles each
 * register write one by one (~14 us apiece, ~0.14 ms per band, measured on metal).  The
 * firmware keeps every blitter register in a global between writes (rtg.c rect_x1 ..,
 * blitter_dst_offset, blitter_colormode, rect_rgb; written only by their own register
 * handlers), so a value already on the card need not be sent again: a band then costs Y1,
 * Y2 and the trigger for a fill, Y1, Y2, Y3 and the trigger for a copy.
 *
 * The cache is emptied whenever something else may have written those registers: when a
 * blitting ioctl starts, after each copyin inside BLITS (a copyin can sleep on a page
 * fault, and another process can run meanwhile), at SETREG, at a mode set and at attach.
 * The console, which can run from an interrupt between two records, empties it after using
 * the registers itself (zz9k_blit_inuse below).
 */
#define ZZ9K_BLIT_FORGET(bp)   ((bp)->blitvalid = 0)

/*
 * Nonzero while a blitter operation, a pan or a mode set is writing the blitter registers.
 * The console can be written from an interrupt taken between two of those writes; it then
 * stays off the blitter registers (CPU drawing, no pan) so the operation below it is not
 * corrupted, and when it does use them it empties the cache after.
 */
static int zz9k_blit_inuse;

static struct zz9k_board zz9k_boards[ZZ9K_MAXBOARDS];
static char zz9k_noboard[ZZ9K_MAXBOARDS];      /* a probe found nothing */

static unsigned short zz9k_mode_id();
static unsigned short zz9k_bpp();
static int zz9k_probe();
static int zz9k_present();
static int zz9k_attach();
static void zz9k_fill_autocon();
static unsigned long zz9k_rd();
static void zz9k_wr();
static void zz9k_wr32();
static int zz9k_aga();
static char *zz9k_devphys();
static void zz9k_monsw_attach();

#ifdef ZZ9K_FB_NC
/*
 * The framebuffer window's pages go into the driver-line kernel's cache-class
 * selector as noncacheable-not-serialised (NC, 0x60) instead of the default for an
 * unmanaged device page, NCS (0x40): stores then drain through the 68060 store buffer
 * instead of serialising the pipeline one by one.  Only the framebuffer (base+fb_phys,
 * fb_size bytes), never the register page, which keeps NCS as a control register needs.
 * The selector's table has two slots and is immutable once installed: register once, at
 * attach -- before the device can be opened or mapped, i.e. before any first fault on it --
 * and never remove.  As Antti's va2000 does (va2000.c va2000_map_regs).  Driver-line
 * kernels only: the r13 lineage has no hat_cm_fb table at all.
 */
extern int hat_cm_fb_add();
#endif

/*
 * RTG monitor switch.  The machine's switch line belongs to the kernel's
 * card-independent monsw driver (sys/monsw.h, the port's driver-line kernels).  This driver
 * only (a) registers with it, offering the Z3660 firmware's `monitor_switch` (register
 * 0x318) as the seed -- used only when /etc/monsw is absent; (b) answers the RTG contract's
 * SVGAIOCGet/SetMonitorSwitch (Klaus Burckert's numbering, as in Antti's va2000.h), which the
 * X servers' rtg/ layer issues on its own device at screen init and at exit; (c) calls
 * monsw_lastclose() on its last close, so a server that dies leaves the monitor on the native
 * video.  Built with -DZZ9K_MONSW, i.e. only for kernels that link monsw; without it the
 * ioctls are answered but switch nothing (the r13 lineage keeps startx-rtg's fallback).
 */
#ifdef ZZ9K_MONSW
#include <sys/monsw.h>
#define ZZ9K_MONSW_SET(rtg)   monsw_set(rtg)
#else
#define ZZ9K_MONSW_SET(rtg)
#endif
static void zz9k_blit_fill();
static void zz9k_blit_copy();
static void zz9k_cpu_fillrect();
static void zz9k_cpu_copyrect();
static int zz9k_can_blit();
static int zz9k_rect_ok();
static void zz9k_blit_run();
static void zz9k_fillrect();
static void zz9k_copyrect();
static void zz9k_bwr();
static void zz9k_bwr32();
static void zz9k_fill_fbinfo();
static void zz9k_set_mode_hw();
static void zz9k_set_switch_hw();
static int zz9k_console_enable();
static void zz9k_console_disable();
static int zz9k_console_test();
static void zz9k_console_puts();
static void zz9k_console_fill_rect();
static void zz9k_conputc();
static unsigned short zz9k_console_color();

#ifdef ZZ9K_MODULE
extern struct screen *displayedscreen;    /* DDI data; no stock header declares it */
static int zz9k_cons_hook();
#else
extern void (*conputc)();

static void (*zz9k_old_conputc)();
static void zz9k_kputc();
#endif
static int zz9k_console_on;
static int zz9k_console_redirect;
/*
 * A client's SETMODE while the console shows on that board pauses it: the client owns the
 * screen and its stride, and console output goes to the native screen meanwhile, without
 * switching the monitor back.  The last close resumes it (mode, clear, monitor).
 */
static int zz9k_console_paused;
static int zz9k_console_row;
static int zz9k_console_col;
static int zz9k_console_cols;
static int zz9k_console_rows;
static int zz9k_console_font_w;
static int zz9k_console_font_h;
static int zz9k_console_cursor_on;
static int zz9k_console_esc;
static int zz9k_console_priv;      /* the sequence has a private marker or intermediate */
static int zz9k_console_argc;
static int zz9k_console_arg;
static int zz9k_console_args[4];
static unsigned short zz9k_console_fg;
static unsigned short zz9k_console_bg;
static struct zz9k_board *zz9k_console_bp;
static struct zz9k_font *zz9k_console_font;

#define ZZ9K_CON_WIDTH  1920
#define ZZ9K_CON_HEIGHT 800
#define ZZ9K_CON_COLOR  ZZ9K_COLOR_16BIT565
#define ZZ9K_FALLBACK_FONT_W 6
#define ZZ9K_FALLBACK_FONT_H 8
#define ZZ9K_FG         0xffff
#define ZZ9K_NOMODE     0xffff          /* zz9k_mode_id: not in the row's table */
#define ZZ9K_CURSOR_XOR 0xffff          /* the cursor inverts its cell */
/* COLORMODE's high byte, the copy's plane mask: all planes (FILLRECT ignores it) */
#define ZZ9K_COLORMODE_MASK  (0xff << 8)
#define ZZ9K_FILL_TRIGGER    0xff       /* the value written to FILLRECT */
#define ZZ9K_COPY_TRIGGER    1          /* the value written to COPYRECT */
#define ZZ9K_BG         0x0000
#define ZZ9K_FONT_MAGIC 0x2a46
#define ZZ9K_ESC_NONE   0
#define ZZ9K_ESC_ESC    1
#define ZZ9K_ESC_CSI    2

#define ZZ9K_VPOSR      0xdff004L
#define ZZ9K_VSECT1     0x40000000UL

static int zz9k_con_w;
static int zz9k_con_h;

/*
 * ZZ9000 register file: 16-bit registers, offsets from zz9000.h.
 */
static unsigned short zz9k_regoff_zz9000[ZZ9K_NREG] = {
    ZZ9K_REG_HW_VERSION,
    ZZ9K_REG_MODE,
    ZZ9K_REG_PAN_HI,
    ZZ9K_REG_BLITTER_X1,
    ZZ9K_REG_BLITTER_Y1,
    ZZ9K_REG_BLITTER_X2,
    ZZ9K_REG_BLITTER_Y2,
    ZZ9K_REG_BLITTER_ROW_PITCH,
    ZZ9K_REG_BLITTER_X3,
    ZZ9K_REG_BLITTER_Y3,
    ZZ9K_REG_BLITTER_RGB_HI,
    ZZ9K_REG_BLITTER_OP_FILLRECT,
    ZZ9K_REG_BLITTER_OP_COPYRECT,
    ZZ9K_REG_BLITTER_SRC_HI,
    ZZ9K_REG_BLITTER_DST_HI,
    ZZ9K_REG_BLITTER_COLORMODE,
    ZZ9K_REG_BLITTER_SPLIT_POS,
    ZZ9K_REG_FW_VERSION,
    ZZ9K_REG_CX_CAPTURE,
    ZZ9K_NOREG
};

static struct zz9k_modetab zz9k_modes_zz9000[] = {
    { 1280, 720, ZZ9K_MODE_1280x720 },
    { 800, 600, ZZ9K_MODE_800x600 },
    { 640, 480, ZZ9K_MODE_640x480 },
    { 1024, 768, ZZ9K_MODE_1024x768 },
    { 1280, 1024, ZZ9K_MODE_1280x1024 },
    { 1920, 1080, ZZ9K_MODE_1920x1080 },
    { 720, 576, ZZ9K_MODE_720x576 },
    { 720, 480, ZZ9K_MODE_720x480 },
    { 640, 512, ZZ9K_MODE_640x512 },
    { 1600, 1200, ZZ9K_MODE_1600x1200 },
    { 2560, 1440, ZZ9K_MODE_2560x1440 },
    { 640, 400, ZZ9K_MODE_640x400 },
    { 1920, 800, ZZ9K_MODE_1920x800 },
    { 0, 0, 0 }
};

/* the ZZ9000's video and blitter registers, for GETREG/SETREG (zz9000.h) */
static struct zz9k_regwin zz9k_regwin_zz9000[] = {
    { ZZ9K_REG_HW_VERSION, ZZ9K_REG_HW_VERSION + 2, 0 },
    { ZZ9K_REG_MODE, ZZ9K_REG_MODE + 2, 1 },
    { ZZ9K_REG_PAN_HI, ZZ9K_REG_PAN_LO + 2, 1 },
    { ZZ9K_REG_BLITTER_X1, ZZ9K_REG_BLITTER_OP_COPYRECT + 2, 1 },
    { ZZ9K_REG_BLITTER_SRC_HI, ZZ9K_REG_BLITTER_SRC_PITCH + 2, 1 },
    { ZZ9K_REG_VBLANK_STATUS, ZZ9K_REG_VBLANK_STATUS + 2, 0 },
    { ZZ9K_REG_BLITTER_SPLIT_POS, ZZ9K_REG_BLITTER_SPLIT_POS + 2, 1 },
    { ZZ9K_REG_FW_VERSION, ZZ9K_REG_FW_VERSION + 2, 0 },
    { 0, 0, 0 }
};

#ifdef ZZ9K_TARGET_Z3660
/*
 * Z3660 register file: 32-bit words from 0x100 (zz9000.h).  No HW_VERSION,
 * no capture register.
 */
static unsigned short zz9k_regoff_z3660[ZZ9K_NREG] = {
    ZZ9K_NOREG,
    ZZ9K_Z3660_REG_MODE,
    ZZ9K_Z3660_REG_PAN,
    ZZ9K_Z3660_REG_X1,
    ZZ9K_Z3660_REG_Y1,
    ZZ9K_Z3660_REG_X2,
    ZZ9K_Z3660_REG_Y2,
    ZZ9K_Z3660_REG_ROW_PITCH,
    ZZ9K_Z3660_REG_X3,
    ZZ9K_Z3660_REG_Y3,
    ZZ9K_Z3660_REG_RGB,
    ZZ9K_Z3660_REG_FILLRECT,
    ZZ9K_Z3660_REG_COPYRECT,
    ZZ9K_Z3660_REG_BLIT_SRC,
    ZZ9K_Z3660_REG_BLIT_DST,
    ZZ9K_Z3660_REG_COLORMODE,
    ZZ9K_Z3660_REG_SPLIT_POS,
    ZZ9K_Z3660_REG_FW_VERSION,
    ZZ9K_NOREG,
    ZZ9K_Z3660_REG_MONITOR_SWITCH
};

static struct zz9k_modetab zz9k_modes_z3660[] = {
    { 1280, 720, ZZ9K_MODE_1280x720 },
    { 800, 600, ZZ9K_MODE_800x600 },
    { 640, 480, ZZ9K_MODE_640x480 },
    { 1024, 768, ZZ9K_MODE_1024x768 },
    { 1280, 1024, ZZ9K_MODE_1280x1024 },
    { 1920, 1080, ZZ9K_MODE_1920x1080 },
    { 720, 576, ZZ9K_MODE_720x576 },
    { 720, 480, ZZ9K_MODE_720x480 },
    { 640, 512, ZZ9K_MODE_640x512 },
    { 1600, 1200, ZZ9K_MODE_1600x1200 },
    { 2560, 1440, ZZ9K_MODE_2560x1440 },
    { 640, 400, ZZ9K_Z3660_MODE_640x400 },
    { 1280, 800, ZZ9K_Z3660_MODE_1280x800 },
    { 1920, 1200, ZZ9K_Z3660_MODE_1920x1200 },
    { 1600, 900, ZZ9K_Z3660_MODE_1600x900 },
    { 1680, 1050, ZZ9K_Z3660_MODE_1680x1050 },
    { 1366, 768, ZZ9K_Z3660_MODE_1366x768 },
    { 0, 0, 0 }
};

/*
 * The Z3660's video and blitter words, for GETREG/SETREG.  Left out: ZZ_CONFIG 0x104 (the
 * interrupt enables the ethernet driver owns), FILLTEMPLATE 0x13C (its template comes from
 * an address), 0x150-0x178 (operations on addresses), 0x180-0x184 (DMA), 0x190-0x1AC (the
 * ethernet mailbox, FW_VERSION among it: GETINFO reports that), and everything from 0x1B0
 * (USB, decoder, audio, the firmware's own settings).
 */
static struct zz9k_regwin zz9k_regwin_z3660[] = {
    { ZZ9K_Z3660_REG_MODE, ZZ9K_Z3660_REG_MODE + 4, 1 },
    { ZZ9K_Z3660_REG_SPRITE_X, ZZ9K_Z3660_REG_COPYRECT + 4, 1 },
    { ZZ9K_Z3660_REG_BLIT_SRC, ZZ9K_Z3660_REG_SRC_PITCH + 4, 1 },
    { ZZ9K_Z3660_REG_VBLANK, ZZ9K_Z3660_REG_VBLANK + 4, 0 },
    { ZZ9K_Z3660_REG_SPLIT_POS, ZZ9K_Z3660_REG_SPLIT_POS + 4, 1 },
    { ZZ9K_Z3660_REG_FW_BETA, ZZ9K_Z3660_REG_FW_ALFA + 4, 0 },
    { ZZ9K_Z3660_REG_MONITOR_SWITCH, ZZ9K_Z3660_REG_MONITOR_SWITCH + 4, 0 },
    { 0, 0, 0 }
};

/* The Z3660's measurements for ZZ9KIOC_SENSORS, in the order of the ZZ9K_SENS_* ids. */
static struct zz9k_sensreg zz9k_sens_z3660[ZZ9K_SENS_N] = {
    { ZZ9K_Z3660_REG_TEMPERATURE, 10, 2000 },   /* C x 10 -> x 100 */
    { ZZ9K_Z3660_REG_LTC_TEMP, 1, 20000 },
    { ZZ9K_Z3660_REG_LTC_060_TEMP, 1, 20000 },
    { ZZ9K_Z3660_REG_VOLTAGE_INT, 10, 20000 },  /* V x 100 -> mV */
    { ZZ9K_Z3660_REG_VOLTAGE_AUX, 10, 20000 },
    { ZZ9K_Z3660_REG_LTC_V1, 10, 20000 },
    { ZZ9K_Z3660_REG_LTC_V2, 10, 20000 },
    { ZZ9K_Z3660_REG_LTC_VCC, 10, 20000 },
    { ZZ9K_Z3660_REG_CPU_FREQ, 1, 1000 }
};
#endif /* ZZ9K_TARGET_Z3660 */

/*
 * The card's blitter.  The Z3660 firmware runs FILLRECT and COPYRECT
 * synchronously and acknowledges the triggering register write only when the
 * operation is done (rtg.c REG_ZZ_FILLRECT/COPYRECT), so no completion wait is
 * needed.  -DZZ9K_NO_BLITTER builds the Z3660 rows without it (the CPU loops,
 * as before); -DZZ9K_BENCH_BLIT gives it to the ZZ9000 rows, a bench aid for
 * amiberry's emulated blitter, not a statement about the real ZZ9000.
 */
#ifdef ZZ9K_NO_BLITTER
#define ZZ9K_BT_BLIT_Z3660     0
#else
#define ZZ9K_BT_BLIT_Z3660     ZZ9K_BT_BLITTER
#endif
#ifdef ZZ9K_BENCH_BLIT
#define ZZ9K_BT_BLIT_ZZ9000    ZZ9K_BT_BLITTER
#else
#define ZZ9K_BT_BLIT_ZZ9000    0
#endif

/*
 * Kernel-patchable: zz9k_blit_on 0 turns the blitter off without a relink;
 * zz9k_blit_maxarea splits an operation into bands of at most that many pixels,
 * since the CPU sits in the stalled register write for the whole operation and
 * takes no interrupt meanwhile.  The default, 8192 px, is about 0.16 ms at the
 * firmware's measured ~50,000 px/ms: under one character time at 38400 baud,
 * where a full-screen operation (~20 ms) lost 42-44 % of serial input.  0 means
 * the default; a value of a screen's area or more never splits.
 */
#define ZZ9K_BLIT_MAXAREA_DEFAULT  8192L
int zz9k_blit_on = 1;
long zz9k_blit_maxarea = ZZ9K_BLIT_MAXAREA_DEFAULT;

/*
 * Kernel-patchable (the module's `fixed` parameter): zz9k_fixed_on 1 lets the probe try the
 * fixed-base rows, whose board has no AutoConfig entry.  Off by default: such a probe reads
 * a fixed address on any machine with the right chipset, and on one without the board that
 * can bus-error or hit another card's memory.  A real Z3660 is found through its AutoConfig
 * entry.  The bench build (-DZZ9K_BENCH_Z3660) turns it on for amiberry's Z3660 stand-in,
 * which has no entry.
 */
#ifdef ZZ9K_BENCH_Z3660
int zz9k_fixed_on = 1;
#else
int zz9k_fixed_on = 0;
#endif

static struct zz9k_boardtab zz9k_boardtab[] = {
    { "Zorro II board", "",
      ZZ9K_PRODUCT_PC_Z2, 0, 0,
      ZZ9K_FB_OFFSET, ZZ9K_FB_OFFSET, 0, ZZ9K_Z2_RESERVED_TAIL, ZZ9K_REG_SIZE,
      2, zz9k_regoff_zz9000, zz9k_modes_zz9000,
      ZZ9K_CON_WIDTH, ZZ9K_CON_HEIGHT,
      ZZ9K_FW_VER_MIN, 0, 2, ZZ9K_PRODUCT_Z2, ZZ9K_BT_BLIT_ZZ9000,
      zz9k_regwin_zz9000, 0 },
#ifndef ZZ9K_BENCH_Z3
    { "Zorro III product", "configure card as Zorro II product 3 for AMIX",
      ZZ9K_PRODUCT_PC_Z3, 0, 0,
      ZZ9K_FB_OFFSET, ZZ9K_FB_OFFSET, 0, ZZ9K_Z2_RESERVED_TAIL, ZZ9K_REG_SIZE,
      2, zz9k_regoff_zz9000, zz9k_modes_zz9000,
      ZZ9K_CON_WIDTH, ZZ9K_CON_HEIGHT,
      ZZ9K_FW_VER_MIN, 0, 3, ZZ9K_PRODUCT_Z3, ZZ9K_BT_REJECT,
      zz9k_regwin_zz9000, 0 }
#else
    /*
     * Bench aid, not a supported configuration: amiberry can place its
     * ZZ9000 Zorro III model inside the identity map (z3mapping=uae puts
     * it at 0x10000000), which rehearses a 32-bit window before the Z3660
     * stand-in exists.  Replaces the product 4 rejection above; same
     * register file and layout as the Zorro II row, framebuffer capped so
     * the mapping stays small.  The probe still refuses a product 4 that
     * lies outside the identity map.
     */
    { "Zorro III board (bench)", "",
      ZZ9K_PRODUCT_PC_Z3, 0, 0,
      ZZ9K_FB_OFFSET, ZZ9K_FB_OFFSET, ZZ9K_Z3660_FB_MAX, 0, ZZ9K_REG_SIZE,
      2, zz9k_regoff_zz9000, zz9k_modes_zz9000,
      ZZ9K_CON_WIDTH, ZZ9K_CON_HEIGHT,
      ZZ9K_FW_VER_MIN, 0, 3, ZZ9K_PRODUCT_Z3, ZZ9K_BT_BLIT_ZZ9000,
      zz9k_regwin_zz9000, 0 }
#endif /* ZZ9K_BENCH_Z3 */
#ifdef ZZ9K_TARGET_Z3660
    /*
     * The combo board as the AutoConfig table lists it.  On real hardware
     * the hard-mapped board (autoconfig_rtg NO) still enters the board
     * list -- it advertises itself for SCSI boot -- so the presence of an
     * entry says nothing about where the window is; only its base does.
     * Inside the identity map (0x10000000) it is the RTG and this row
     * attaches it; above it (autoconfig_rtg YES: 0x40000000, or
     * 0x50000000 when the RAM board configures first) the generic check
     * refuses it with the advice below and the next row has its turn.
     * Measured on an A4000 + Z3660, 2026-09-18: rejecting on the entry
     * alone refused a board the kernel can use.
     */
  , { "Z3660 RTG", "set autoconfig_rtg NO for AMIX",
      ZZ9K_Z3660_PRODUCT_PC, 0, 0,
      0, ZZ9K_Z3660_FB_PHYS, ZZ9K_Z3660_FB_MAX, 0,
      ZZ9K_Z3660_REG_SIZE,
      4, zz9k_regoff_z3660, zz9k_modes_z3660,
      ZZ9K_Z3660_CON_W, ZZ9K_Z3660_CON_H,
      ZZ9K_Z3660_FW_MIN, ZZ9K_Z3660_FW_MAJOR, 3, 1,
      ZZ9K_BT_NOCAPTURE | ZZ9K_BT_BLIT_Z3660, zz9k_regwin_z3660,
      zz9k_sens_z3660 }
    /*
     * The same board reached by its fixed base, for a machine whose
     * AutoConfig table does not list it.  Device space is the framebuffer
     * alone (physically at +0x200000): the register page also carries the
     * ethernet driver's doorbells, so it is never mapped to userland;
     * registers are reached through GETREG/SETREG only.
     */
  , { "Z3660 RTG (fixed base)", "",
      0, ZZ9K_Z3660_BASE, ZZ9K_Z3660_WINDOW,
      0, ZZ9K_Z3660_FB_PHYS, ZZ9K_Z3660_FB_MAX, 0,
      ZZ9K_Z3660_REG_SIZE,
      4, zz9k_regoff_z3660, zz9k_modes_z3660,
      ZZ9K_Z3660_CON_W, ZZ9K_Z3660_CON_H,
      ZZ9K_Z3660_FW_MIN, ZZ9K_Z3660_FW_MAJOR, 3, 1,
      ZZ9K_BT_AGAGATE | ZZ9K_BT_NOCAPTURE | ZZ9K_BT_BLIT_Z3660,
      zz9k_regwin_z3660, zz9k_sens_z3660 }
#endif /* ZZ9K_TARGET_Z3660 */
};

#define ZZ9K_NBOARDTAB (sizeof zz9k_boardtab / sizeof zz9k_boardtab[0])

static unsigned short
zz9k_mode_id(bp, width, height)
struct zz9k_board *bp;
unsigned short width;
unsigned short height;
{
    struct zz9k_modetab *m;

    for (m = bp->bt->modes; m->width; ++m)
        if (m->width == width && m->height == height)
            return m->id;
    return ZZ9K_NOMODE;
}

static unsigned short
zz9k_bpp(color_mode)
unsigned short color_mode;
{
    switch (color_mode) {
    case ZZ9K_COLOR_8BIT:
        return 1;
    case ZZ9K_COLOR_15BIT:
    case ZZ9K_COLOR_16BIT565:
        return 2;
    case ZZ9K_COLOR_32BIT:
        return 4;
    default:
        return 0;
    }
}

static void
zz9k_fill_fbinfo(bp, fbi)
struct zz9k_board *bp;
struct zz9k_fbinfo *fbi;
{
    fbi->fb_offset = bp->bt->fb_offset;
    fbi->fb_size = bp->fb_size;
    fbi->width = bp->width;
    fbi->height = bp->height;
    fbi->color_mode = bp->color_mode;
    fbi->bytes_per_pixel = zz9k_bpp(bp->color_mode);
    fbi->stride = bp->width * fbi->bytes_per_pixel;
    fbi->red_mask = 0;
    fbi->green_mask = 0;
    fbi->blue_mask = 0;
    fbi->alpha_mask = 0;
    fbi->red_shift = 0;
    fbi->green_shift = 0;
    fbi->blue_shift = 0;
    fbi->alpha_shift = 0;

    switch (bp->color_mode) {
    case ZZ9K_COLOR_8BIT:
        fbi->depth = 8;
        break;
    case ZZ9K_COLOR_15BIT:
        fbi->depth = 15;
        fbi->red_mask = 0x7c00;
        fbi->green_mask = 0x03e0;
        fbi->blue_mask = 0x001f;
        fbi->alpha_mask = 0x8000;
        fbi->red_shift = 10;
        fbi->green_shift = 5;
        fbi->blue_shift = 0;
        fbi->alpha_shift = 15;
        break;
    case ZZ9K_COLOR_16BIT565:
        fbi->depth = 16;
        fbi->red_mask = 0xf800;
        fbi->green_mask = 0x07e0;
        fbi->blue_mask = 0x001f;
        fbi->red_shift = 11;
        fbi->green_shift = 5;
        fbi->blue_shift = 0;
        break;
    case ZZ9K_COLOR_32BIT:
        fbi->depth = 32;
        fbi->red_mask = 0x00ff0000;
        fbi->green_mask = 0x0000ff00;
        fbi->blue_mask = 0x000000ff;
        fbi->alpha_mask = 0xff000000;
        fbi->red_shift = 16;
        fbi->green_shift = 8;
        fbi->blue_shift = 0;
        fbi->alpha_shift = 24;
        break;
    default:
        fbi->depth = 0;
        break;
    }
}

/*
 * bp->bt and bp->base are set by the caller.  A register the row does not
 * have reads as all ones, so an absent HW_VERSION never counts as present.
 */
static int
zz9k_present(bp)
struct zz9k_board *bp;
{
    unsigned long hw, fw, ones;

    if (!bp->base)
        return 0;

    ones = (bp->bt->regw == 4) ? 0xffffffffUL : 0xffffUL;
    hw = zz9k_rd(bp, ZZ9K_R_HW_VERSION);
    fw = zz9k_rd(bp, ZZ9K_R_FW_VERSION);
    if ((hw == 0 || hw == ones) && (fw == 0 || fw == ones))
        return 0;
    if (bp->bt->fw_major && (fw >> 8) != bp->bt->fw_major)
        return 0;
    return 1;
}


/*
 * Attach the row's board at physical base.  kregs and kfb are the kernel's addresses of the
 * register page and the framebuffer when the window is mapped (kfblen bytes of it), or 0
 * when it lies in the identity map and the physical address is the kernel's.
 */
static int
zz9k_attach_at(bp, bt, base, size, kregs, kfb, kfblen)
struct zz9k_board *bp;
struct zz9k_boardtab *bt;
long base;
long size;
char *kregs;
char *kfb;
unsigned long kfblen;
{
    bp->bt = bt;
    bp->base = kregs ? (long)kregs : base;
    if (!zz9k_present(bp)) {
        bp->base = 0;
        bp->bt = (struct zz9k_boardtab *)0;
        if (bt->autocon_pc)
            printf("zz9000: autocon base 0x%x did not look like ZZ9000 regs\n",
                (unsigned int)base);
        else
            printf("zz9000: no %s at 0x%x\n", bt->name, (unsigned int)base);
        return 0;
    }
    bp->phys = base;
    bp->mapped = kregs != 0;

    if (bt->fb_max) {
        bp->fb_size = bt->fb_max;
        bp->board_size = bt->fb_offset + bp->fb_size;
    } else {
        bp->board_size = (unsigned long)size;
        if (bp->board_size > bt->fb_offset + bt->reserved_tail)
            bp->fb_size = bp->board_size - bt->fb_offset - bt->reserved_tail;
        else
            bp->fb_size = 0;
    }
    bp->fb = (char *)base + bt->fb_phys;
    bp->fb_kmap = bp->fb_size;
    if (kregs) {
        bp->fb = kfb;
        if (bp->fb_kmap > kfblen)
            bp->fb_kmap = kfblen;
    }
#ifdef ZZ9K_FB_NC
    if (bp->fb_size) {
        if (hat_cm_fb_add(phystopfn((unsigned long)base + bt->fb_phys),
                          phystopfn((unsigned long)base + bt->fb_phys + bp->fb_size)))
            printf("zz9000: framebuffer 0x%x+0x%x registered noncacheable-not-serialised\n",
                (unsigned int)((unsigned long)base + bt->fb_phys), (unsigned int)bp->fb_size);
        else
            printf("zz9000: framebuffer cache class NOT registered (stays serialised)\n");
    }
#endif
    bp->hw_version = (unsigned short)zz9k_rd(bp, ZZ9K_R_HW_VERSION);
    bp->fw_version = (unsigned short)zz9k_rd(bp, ZZ9K_R_FW_VERSION);
    bp->width = 800;
    bp->height = 600;
    bp->color_mode = ZZ9K_COLOR_16BIT565;
    bp->mode = ZZ9K_MODE_800x600;
    zz9k_monsw_attach(bp);

    printf("zz9000: %s at 0x%x size 0x%x hw 0x%x fw 0x%x\n", bt->name,
        (unsigned int)bp->phys, (unsigned int)bp->board_size,
        bp->hw_version, bp->fw_version);
    if (bp->mapped)
        printf("zz9000: mapped: the register page and 0x%x bytes of the framebuffer\n",
            (unsigned int)bp->fb_kmap);
    if (bt->fw_min && bp->fw_version < bt->fw_min)
        printf("zz9000: warning firmware 0x%x is older than 0x%x\n",
            bp->fw_version, bt->fw_min);
    return 1;
}

static int
zz9k_attach(bp, bt, base, size)
struct zz9k_board *bp;
struct zz9k_boardtab *bt;
long base;
long size;
{
    return zz9k_attach_at(bp, bt, base, size, (char *)0, (char *)0, 0UL);
}

/*
 * A window outside the identity-mapped low 1 GB -- the Z3660 under autoconfig_rtg YES
 * lies at 0x40000000, or 0x50000000 when the 256 MB RAM board configures first -- is
 * reached through kernel mappings of the two parts the display needs: the register page
 * and the start of the framebuffer, at most zz9k_kvfb MiB of it (the console and the
 * kernel's own drawing stay inside that part; user mmap still reaches the whole
 * framebuffer by its physical pages).  Only rows whose device space is the framebuffer
 * alone (fb_offset 0) are attached this way, so nothing else on the board is mapped.
 *
 * The mapping depends on the line: on the 68040/68060 line dev_kvmap, which sets the
 * cache class (noncacheable-serialised for the registers, noncacheable for the
 * framebuffer); on the 68030 line sptalloc, whose pages are not data-cached there, from
 * a shared 4 MiB pool.  A build with neither (-DZZ9K_DEV_KVMAP, -DZZ9K_SPT_MAP) refuses
 * such a window, as before.  No sleeping: a failed mapping is a failed probe.
 */
#if defined(ZZ9K_DEV_KVMAP) || defined(ZZ9K_SPT_MAP)
#define ZZ9K_CAN_MAP 1
#ifdef ZZ9K_DEV_KVMAP
extern char *dev_kvmap();
extern void dev_kvunmap();
#define ZZ9K_KVFB_DEFAULT  8        /* MiB: a 1920x1080 console's pan ring */
#else
extern char *sptalloc();
extern void sptfree();
#define ZZ9K_KVFB_DEFAULT  2        /* MiB, of the 4 MiB pool other drivers share */
#endif
#define ZZ9K_KVFB_MIN      1
#define ZZ9K_KVFB_LIMIT    46
#define ZZ9K_CM_NCS        0x40     /* noncacheable, serialised: registers */
#define ZZ9K_CM_NC         0x60     /* noncacheable: the framebuffer */
#define ZZ9K_MIB           0x100000UL
int zz9k_kvfb = ZZ9K_KVFB_DEFAULT;  /* kernel-patchable; the module's kvfb parameter */

static char *
zz9k_map(phys, len, cm)
unsigned long phys, len;
int cm;
{
#ifdef ZZ9K_DEV_KVMAP
    return dev_kvmap(phys, len, cm, 1);         /* flags 1: do not sleep */
#else
    return sptalloc((int)((len + NBPP - 1) / NBPP), PG_V, phystopfn(phys), 0);
#endif
}

static void
zz9k_unmap(kva, len)
char *kva;
unsigned long len;
{
    if (!kva)
        return;
#ifdef ZZ9K_DEV_KVMAP
    dev_kvunmap(kva, len);
#else
    sptfree(kva, (int)((len + NBPP - 1) / NBPP), 0);
#endif
}

/* the board's mapped parts: released at unload or when the attach fails */
static char *zz9k_kregs;
static unsigned long zz9k_kregslen;
static char *zz9k_kfb;
static unsigned long zz9k_kfblen;

static void
zz9k_unmap_all()
{
    zz9k_unmap(zz9k_kfb, zz9k_kfblen);
    zz9k_unmap(zz9k_kregs, zz9k_kregslen);
    zz9k_kfb = zz9k_kregs = (char *)0;
    zz9k_kfblen = zz9k_kregslen = 0;
}

static int
zz9k_attach_mapped(bp, bt, base, size)
struct zz9k_board *bp;
struct zz9k_boardtab *bt;
long base;
long size;
{
    unsigned long fbsize, want;
    int mib;

    if (bt->fb_offset != 0 || bt->reg_size == 0)
        return 0;
    fbsize = bt->fb_max ? bt->fb_max : (unsigned long)size - bt->fb_phys - bt->reserved_tail;
    zz9k_kregs = zz9k_map((unsigned long)base, bt->reg_size, ZZ9K_CM_NCS);
    zz9k_kregslen = bt->reg_size;
    if (!zz9k_kregs) {
        printf("zz9000: %s at 0x%x: cannot map its register page\n", bt->name,
            (unsigned int)base);
        return 0;
    }
    /* as much of the framebuffer as asked, halving when the kernel has less room */
    mib = zz9k_kvfb;
    if (mib < ZZ9K_KVFB_MIN || mib > ZZ9K_KVFB_LIMIT)
        mib = ZZ9K_KVFB_DEFAULT;
    for (; mib >= ZZ9K_KVFB_MIN; mib >>= 1) {
        want = (unsigned long)mib * ZZ9K_MIB;
        if (want > fbsize)
            want = fbsize;
        zz9k_kfb = zz9k_map((unsigned long)base + bt->fb_phys, want, ZZ9K_CM_NC);
        if (zz9k_kfb) {
            zz9k_kfblen = want;
            break;
        }
    }
    if (!zz9k_kfb) {
        printf("zz9000: %s at 0x%x: cannot map its framebuffer\n", bt->name,
            (unsigned int)base);
        zz9k_unmap_all();
        return 0;
    }
    if (zz9k_attach_at(bp, bt, base, size, zz9k_kregs, zz9k_kfb, zz9k_kfblen))
        return 1;
    zz9k_unmap_all();
    return 0;
}
#endif /* ZZ9K_DEV_KVMAP || ZZ9K_SPT_MAP */

static void
zz9k_fill_autocon(dump)
struct zz9k_autocon_dump *dump;
{
    int i;
    struct zz9k_configdev *cd;

    dump->count = ZZ9K_MAX_AUTOCON;
    dump->pad = 0;
    for (i = 0; i < ZZ9K_MAX_AUTOCON; ++i) {
        cd = &bootinfo.autocon[i];
        dump->entry[i].manufacturer = cd->er_manufacturer;
        dump->entry[i].product = cd->er_product;
        dump->entry[i].type = cd->er_type;
        dump->entry[i].flags = cd->er_flags;
        dump->entry[i].board_addr = cd->cd_boardaddr;
        dump->entry[i].board_size = cd->cd_boardsize;
        dump->entry[i].slot_addr = cd->cd_slotaddr;
        dump->entry[i].slot_size = cd->cd_slotsize;
    }
}
/*
 * Agnus/Alice id: bits 8-14 of VPOSR are >= 0x22 on AGA.  A fixed-base row
 * is probed only on a chipset that can carry its board; the read itself is
 * always safe.
 */
static int
zz9k_aga()
{
    unsigned short v;

    v = *((volatile unsigned short *)ZZ9K_VPOSR);
    return ((v >> 8) & 0x7f) >= 0x22;
}

static int
zz9k_probe(unit)
int unit;
{
    long base, size;
    int i;
    struct zz9k_board *bp;
    struct zz9k_boardtab *bt;

    if (unit >= ZZ9K_MAXBOARDS)
        return 0;

    bp = &zz9k_boards[unit];
    if (bp->base)
        return 1;
    if (zz9k_noboard[unit])             /* boards do not appear later: no second walk */
        return 0;

    for (i = 0; i < ZZ9K_NBOARDTAB; ++i) {
        bt = &zz9k_boardtab[i];
        if (bt->autocon_pc) {
            if (!autocon(bt->autocon_pc, unit, &base, &size))
                continue;
            if (bt->flags & ZZ9K_BT_REJECT) {
                printf("zz9000: %s at 0x%x size 0x%x unsupported\n",
                    bt->name, (unsigned int)base, (unsigned int)size);
                printf("zz9000: %s\n", bt->advice);
                continue;               /* another row may still find a board */
            }
            printf("zz9000: autocon match pc 0x%x base 0x%x size 0x%x\n",
                (unsigned int)bt->autocon_pc, (unsigned int)base,
                (unsigned int)size);
        } else {
            if (!zz9k_fixed_on)
                continue;
            if ((bt->flags & ZZ9K_BT_AGAGATE) && !zz9k_aga())
                continue;
            base = (long)bt->fixed_base;
            size = (long)bt->fixed_size;
        }
        /*
         * Kernel code dereferences the window directly: it must lie
         * inside the identity-mapped low 1 GB.  A window that does not,
         * or a board that does not answer, leaves the next row its turn:
         * the same board may be reachable by another route.
         */
        if ((unsigned long)base >= ZZ9K_VSECT1 ||
            ZZ9K_VSECT1 - (unsigned long)base < (unsigned long)size) {
#ifdef ZZ9K_CAN_MAP
            if (bt->fb_offset == 0) {
                if (zz9k_attach_mapped(bp, bt, base, size))
                    return 1;
                continue;
            }
#endif
            printf("zz9000: %s at 0x%x size 0x%x is outside the identity map\n",
                bt->name, (unsigned int)base, (unsigned int)size);
            if (bt->advice[0])
                printf("zz9000: %s\n", bt->advice);
            continue;
        }
        if (zz9k_attach(bp, bt, base, size))
            return 1;
        /* attach() said what it saw; a later row may still find it */
    }

    printf("zz9000: no supported board found\n");
    zz9k_noboard[unit] = 1;
    return 0;
}

/*
 * Register access through the row: logical id -> offset and width.  An
 * absent register reads as all ones and ignores writes.
 */
static unsigned long
zz9k_rd(bp, r)
struct zz9k_board *bp;
int r;
{
    unsigned short off;

    off = bp->bt->regoff[r];
    if (off == ZZ9K_NOREG)
        return (bp->bt->regw == 4) ? 0xffffffffUL : 0xffffUL;
    if (bp->bt->regw == 4)
        return *((volatile unsigned long *)((char *)bp->base + off));
    return (unsigned long)ZZ9K_REG(bp->base, off);
}

static void
zz9k_wr(bp, r, value)
struct zz9k_board *bp;
int r;
unsigned long value;
{
    unsigned short off;

    off = bp->bt->regoff[r];
    if (off == ZZ9K_NOREG)
        return;
    if (bp->bt->regw == 4)
        *((volatile unsigned long *)((char *)bp->base + off)) = value;
    else
        ZZ9K_REG(bp->base, off) = (unsigned short)value;
}

/* A 32-bit value: one write on a 32-bit row, HI then LO on a 16-bit row. */
static void
zz9k_wr32(bp, r, value)
struct zz9k_board *bp;
int r;
unsigned long value;
{
    unsigned short off;

    off = bp->bt->regoff[r];
    if (off == ZZ9K_NOREG)
        return;
    if (bp->bt->regw == 4) {
        *((volatile unsigned long *)((char *)bp->base + off)) = value;
        return;
    }
    ZZ9K_REG(bp->base, off) = (unsigned short)(value >> 16);
    ZZ9K_REG(bp->base, off + 2) = (unsigned short)(value & 0xffff);
}

/* A blitter register write through the cache: skipped when the card holds it. */
static void
zz9k_bwr(bp, r, value)
struct zz9k_board *bp;
int r;
unsigned long value;
{
    if ((bp->blitvalid & ((unsigned long)1 << r)) && bp->blitreg[r] == value)
        return;
    bp->blitreg[r] = value;
    bp->blitvalid |= (unsigned long)1 << r;
    zz9k_wr(bp, r, value);
}

static void
zz9k_bwr32(bp, r, value)
struct zz9k_board *bp;
int r;
unsigned long value;
{
    if ((bp->blitvalid & ((unsigned long)1 << r)) && bp->blitreg[r] == value)
        return;
    bp->blitreg[r] = value;
    bp->blitvalid |= (unsigned long)1 << r;
    zz9k_wr32(bp, r, value);
}

/*
 * Device space as seen through read/write/mmap: [0, fb_offset) is the
 * register block, [fb_offset, board_size) the framebuffer.  On a row whose
 * framebuffer is not contiguous with its registers the two are joined here.
 */
static char *
zz9k_devphys(bp, off)
struct zz9k_board *bp;
unsigned long off;
{
    if (off < bp->bt->fb_offset)
        return (char *)bp->base + off;
    return bp->fb + (off - bp->bt->fb_offset);
}

/* The physical address behind device offset off (d_mmap). */
static unsigned long
zz9k_devpa(bp, off)
struct zz9k_board *bp;
unsigned long off;
{
    if (off < bp->bt->fb_offset)
        return (unsigned long)bp->phys + off;
    return (unsigned long)bp->phys + bp->bt->fb_phys + (off - bp->bt->fb_offset);
}

static void
zz9k_blit_fill(bp, x, y, w, h, color)
struct zz9k_board *bp;
int x, y, w, h;
unsigned short color;
{
    if (!bp || !bp->base || w <= 0 || h <= 0)
        return;

    zz9k_bwr32(bp, ZZ9K_R_DST, 0);
    /*
     * The Z3660 firmware byte-reverses the RGB register (rtg.c REG_ZZ_RGB:
     * rect_rgb = swap32 of the value written) and a 16-bit fill takes the low
     * half of that (gfx.c fill_rect_solid), i.e. the high half as written: a
     * colour in the low half fills with 0 (measured on the bench, 2026-09-29).
     */
    if (bp->bt->regw == 4)
        zz9k_bwr32(bp, ZZ9K_R_RGB, (unsigned long)color << 16);
    else
        zz9k_bwr32(bp, ZZ9K_R_RGB, (unsigned long)color);
    zz9k_bwr(bp, ZZ9K_R_ROW_PITCH,
        (unsigned long)((bp->width * zz9k_bpp(bp->color_mode)) >> 2));
    /* the copy's value: FILLRECT ignores COLORMODE's mask byte, so fills and copies
     * interleaved in one batch do not rewrite it */
    zz9k_bwr(bp, ZZ9K_R_COLORMODE,
        (unsigned long)(bp->color_mode | ZZ9K_COLORMODE_MASK));
    zz9k_bwr(bp, ZZ9K_R_X1, (unsigned long)x);
    zz9k_bwr(bp, ZZ9K_R_Y1, (unsigned long)y);
    zz9k_bwr(bp, ZZ9K_R_X2, (unsigned long)w);
    zz9k_bwr(bp, ZZ9K_R_Y2, (unsigned long)h);
    zz9k_wr(bp, ZZ9K_R_FILLRECT, ZZ9K_FILL_TRIGGER);
}

static void
zz9k_blit_copy(bp, sx, sy, dx, dy, w, h)
struct zz9k_board *bp;
int sx, sy, dx, dy, w, h;
{
    if (!bp || !bp->base || w <= 0 || h <= 0)
        return;

    zz9k_bwr(bp, ZZ9K_R_Y1, (unsigned long)dy);
    zz9k_bwr(bp, ZZ9K_R_Y2, (unsigned long)h);
    zz9k_bwr(bp, ZZ9K_R_Y3, (unsigned long)sy);
    zz9k_bwr(bp, ZZ9K_R_X1, (unsigned long)dx);
    zz9k_bwr(bp, ZZ9K_R_X2, (unsigned long)w);
    zz9k_bwr(bp, ZZ9K_R_X3, (unsigned long)sx);
    zz9k_bwr(bp, ZZ9K_R_ROW_PITCH,
        (unsigned long)((bp->width * zz9k_bpp(bp->color_mode)) >> 2));
    zz9k_bwr(bp, ZZ9K_R_COLORMODE,
        (unsigned long)(bp->color_mode | ZZ9K_COLORMODE_MASK));
    zz9k_bwr32(bp, ZZ9K_R_SRC, 0);
    zz9k_bwr32(bp, ZZ9K_R_DST, 0);
    zz9k_wr(bp, ZZ9K_R_COPYRECT, ZZ9K_COPY_TRIGGER);
}

/*
 * CPU fill and copy of 16-bit pixels with 32-bit stores where the alignment allows: every
 * access to the board is a bus cycle, so half as many of them is close to twice as fast.
 */
static void
zz9k_fill16(p, n, c)
unsigned short *p;
long n;
unsigned short c;
{
    unsigned long *q, v;

    if (n > 0 && ((unsigned long)p & 2)) {
        *p++ = c;
        --n;
    }
    q = (unsigned long *)p;
    v = ((unsigned long)c << 16) | c;
    for (; n >= 2; n -= 2)
        *q++ = v;
    if (n > 0)
        *(unsigned short *)q = c;
}

/* n bytes of c, 32 bits at a time between the unaligned ends (ZZ9KIOC_FILL) */
static void
zz9k_fill8(p, n, c)
unsigned char *p;
unsigned long n;
unsigned char c;
{
    unsigned long *q, v;

    for (; n > 0 && ((unsigned long)p & 3); --n)
        *p++ = c;
    v = c | ((unsigned long)c << 8);
    v |= v << 16;
    for (q = (unsigned long *)p; n >= 4; n -= 4)
        *q++ = v;
    for (p = (unsigned char *)q; n > 0; --n)
        *p++ = c;
}

/* forward: dst below src, or the two apart */
static void
zz9k_copy16(d, s, n)
unsigned short *d, *s;
long n;
{
    unsigned long *qd, *qs;

    if (((unsigned long)d ^ (unsigned long)s) & 2) {
        while (n-- > 0)
            *d++ = *s++;
        return;
    }
    if (n > 0 && ((unsigned long)d & 2)) {
        *d++ = *s++;
        --n;
    }
    qd = (unsigned long *)d;
    qs = (unsigned long *)s;
    for (; n >= 2; n -= 2)
        *qd++ = *qs++;
    if (n > 0)
        *(unsigned short *)qd = *(unsigned short *)qs;
}

static void
zz9k_cpu_fillrect(bp, x, y, w, h, color)
struct zz9k_board *bp;
int x, y, w, h;
unsigned short color;
{
    unsigned short *fb;
    int yy;

    if (!bp || !bp->base || w <= 0 || h <= 0)
        return;

    fb = (unsigned short *)bp->fb;
    for (yy = y; yy < y + h; ++yy)
        zz9k_fill16(fb + ((unsigned long)yy * bp->width) + x, (long)w, color);
}

static void
zz9k_cpu_copyrect(bp, sx, sy, dx, dy, w, h)
struct zz9k_board *bp;
int sx, sy, dx, dy, w, h;
{
    unsigned short *fb;
    unsigned short *src;
    unsigned short *dst;
    int row, col;
    int row_start, row_end, row_step;

    if (!bp || !bp->base || w <= 0 || h <= 0)
        return;

    fb = (unsigned short *)bp->fb;

    if (dy > sy) {
        row_start = h - 1;
        row_end = -1;
        row_step = -1;
    } else {
        row_start = 0;
        row_end = h;
        row_step = 1;
    }

    for (row = row_start; row != row_end; row += row_step) {
        src = fb + ((unsigned long)(sy + row) * bp->width) + sx;
        dst = fb + ((unsigned long)(dy + row) * bp->width) + dx;
        if (dy == sy && dx > sx) {
            for (col = w - 1; col >= 0; --col)  /* along one row, rightwards */
                dst[col] = src[col];
        } else
            zz9k_copy16(dst, src, (long)w);
    }
}

/*
 * The blitter serves this board now: its row has it, it is not patched off,
 * the mode is the 16-bit one the fill colour and the CPU fallbacks assume,
 * and a row is a whole number of the 32-bit words ROW_PITCH counts in.
 */
static int
zz9k_can_blit(bp)
struct zz9k_board *bp;
{
    return (bp->bt->flags & ZZ9K_BT_BLITTER) && zz9k_blit_on &&
        bp->color_mode == ZZ9K_COLOR_16BIT565 &&
        ((bp->width * zz9k_bpp(bp->color_mode)) & 3) == 0;
}

/* The kernel reaches the first rows rows of the screen (a mapped window maps only a part). */
static int
zz9k_kreach(bp, rows)
struct zz9k_board *bp;
unsigned long rows;
{
    return rows * bp->width * zz9k_bpp(bp->color_mode) <= bp->fb_kmap;
}

/* A non-empty rectangle wholly on the screen. */
static int
zz9k_rect_ok(bp, x, y, w, h)
struct zz9k_board *bp;
unsigned long x, y, w, h;
{
    return w != 0 && h != 0 && x < bp->width && y < bp->height &&
        w <= bp->width - x && h <= bp->height - y;
}

/* Rows per band for a w x h operation: at most zz9k_blit_maxarea pixels, at least one row. */
static int
zz9k_band(w, h)
int w, h;
{
    long area;
    int band;

    area = zz9k_blit_maxarea > 0 ? zz9k_blit_maxarea : ZZ9K_BLIT_MAXAREA_DEFAULT;
    if (w <= 0 || (long)w * h <= area)
        return h;
    band = (int)(area / w);
    return band < 1 ? 1 : band;
}

/*
 * One checked fill (sx < 0) or copy on the blitter, in bands (zz9k_band).  A
 * copy moving down runs its bands bottom-up, so no band overwrites a source
 * row a later band still reads; inside a band the firmware orders rows and
 * columns itself (gfx.c copy_rect_nomask).
 */
static void
zz9k_blit_run(bp, sx, sy, dx, dy, w, h, color)
struct zz9k_board *bp;
int sx, sy, dx, dy, w, h;
unsigned short color;
{
    int band, top, n;

    ++zz9k_blit_inuse;
    band = zz9k_band(w, h);
    if (sx >= 0 && dy > sy) {
        for (top = h; top > 0; top -= n) {
            n = (top < band) ? top : band;
            zz9k_blit_copy(bp, sx, sy + top - n, dx, dy + top - n, w, n);
        }
    } else {
        for (top = 0; top < h; top += n) {
            n = (h - top < band) ? h - top : band;
            if (sx < 0)
                zz9k_blit_fill(bp, dx, dy + top, w, n, color);
            else
                zz9k_blit_copy(bp, sx, sy + top, dx, dy + top, w, n);
        }
    }
    --zz9k_blit_inuse;
}

/*
 * PAN as a framebuffer byte offset.  The firmware adds X1 pixels and Y1 rows of X2 pixels
 * to the value written and takes X2 as the displayed row length (rtg.c REG_ZZ_PAN, as the
 * ZZ9000's), so those are set first: 0, 0 and the mode's width, with COLORMODE for the
 * pixel size.  Through the blitter cache, which then holds them for the next blit.
 */
static void
zz9k_set_pan(bp, off)
struct zz9k_board *bp;
unsigned long off;
{
    ++zz9k_blit_inuse;
    ZZ9K_BLIT_FORGET(bp);
    zz9k_bwr(bp, ZZ9K_R_X1, 0);
    zz9k_bwr(bp, ZZ9K_R_Y1, 0);
    zz9k_bwr(bp, ZZ9K_R_X2, (unsigned long)bp->width);
    zz9k_bwr(bp, ZZ9K_R_COLORMODE, (unsigned long)(bp->color_mode | ZZ9K_COLORMODE_MASK));
    zz9k_wr32(bp, ZZ9K_R_PAN, off);
    --zz9k_blit_inuse;
}

static void
zz9k_set_mode_hw(bp, mode, color_mode, scale_mode)
struct zz9k_board *bp;
unsigned short mode;
unsigned short color_mode;
unsigned short scale_mode;
{
    ++zz9k_blit_inuse;
    zz9k_wr(bp, ZZ9K_R_CAPTURE, 0);
    zz9k_wr(bp, ZZ9K_R_X1, 0);
    zz9k_wr(bp, ZZ9K_R_Y1, 0);
    zz9k_wr(bp, ZZ9K_R_X2, 0);
    zz9k_wr(bp, ZZ9K_R_Y2, 0);
    zz9k_wr(bp, ZZ9K_R_COLORMODE, (unsigned long)color_mode);
    zz9k_wr32(bp, ZZ9K_R_SRC, 0);
    zz9k_wr32(bp, ZZ9K_R_DST, 0);
    zz9k_wr(bp, ZZ9K_R_SPLIT_POS, 0);
    zz9k_wr32(bp, ZZ9K_R_PAN, 0);
    zz9k_wr(bp, ZZ9K_R_MODE,
        (unsigned long)(mode | (color_mode << 8) | (scale_mode << 12)));
    bp->mode = mode;
    bp->color_mode = color_mode;
    ZZ9K_BLIT_FORGET(bp);
    --zz9k_blit_inuse;
}

static void
zz9k_set_switch_hw(bp, enabled)
struct zz9k_board *bp;
unsigned short enabled;
{
    if (bp->bt->flags & ZZ9K_BT_NOCAPTURE)
        return;
    if (enabled == ZZ9K_SWITCH_CAPTURE) {
        zz9k_wr32(bp, ZZ9K_R_PAN, ZZ9K_CAPTURE_PAN);
        zz9k_wr(bp, ZZ9K_R_CAPTURE, 1);
    } else {
        zz9k_wr(bp, ZZ9K_R_CAPTURE, 0);
        zz9k_wr32(bp, ZZ9K_R_PAN, 0);
    }
}

/*
 * Attach: register with monsw, offering the firmware's monitor_switch as the seed when the
 * row has the register and the value is valid (monsw takes a seed only while /etc/monsw has
 * not set a value, and only from the first card that offers one).
 */
static void
zz9k_monsw_attach(bp)
struct zz9k_board *bp;
{
    bp->monsw_handle = -1;
    ZZ9K_BLIT_FORGET(bp);
    bp->svga_switch = 1;
#ifdef ZZ9K_MONSW
    {
        unsigned long v;
        int seed = -1;

        if (bp->bt->regoff[ZZ9K_R_MONSW] != ZZ9K_NOREG) {
            v = zz9k_rd(bp, ZZ9K_R_MONSW);
            if (v & ~(unsigned long)MONSW_VALID)
                printf("zz9000: monitor_switch 0x%x is not a 1/2/16/32 bit sum; no seed\n",
                    (unsigned int)v);
            else
                seed = (int)v;
        }
        bp->monsw_handle = monsw_register(bp->bt->name, seed);
    }
#endif
}

static unsigned char
zz9k_glyph(c, row)
unsigned char c;
int row;
{
    static unsigned char space[7] = {0,0,0,0,0,0,0};
    static unsigned char quest[7] = {0x0e,0x11,0x01,0x02,0x04,0x00,0x04};
    static unsigned char dash[7] = {0,0,0,0x1f,0,0,0};
    static unsigned char dot[7] = {0,0,0,0,0,0x0c,0x0c};
    static unsigned char colon[7] = {0,0x0c,0x0c,0,0x0c,0x0c,0};
    static unsigned char slash[7] = {0x01,0x02,0x04,0x08,0x10,0,0};
    static unsigned char digits[10][7] = {
        {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e},
        {0x04,0x0c,0x04,0x04,0x04,0x04,0x0e},
        {0x0e,0x11,0x01,0x02,0x04,0x08,0x1f},
        {0x1f,0x02,0x04,0x02,0x01,0x11,0x0e},
        {0x02,0x06,0x0a,0x12,0x1f,0x02,0x02},
        {0x1f,0x10,0x1e,0x01,0x01,0x11,0x0e},
        {0x06,0x08,0x10,0x1e,0x11,0x11,0x0e},
        {0x1f,0x01,0x02,0x04,0x08,0x08,0x08},
        {0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e},
        {0x0e,0x11,0x11,0x0f,0x01,0x02,0x0c}
    };
    static unsigned char letters[26][7] = {
        {0x0e,0x11,0x11,0x1f,0x11,0x11,0x11},
        {0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e},
        {0x0e,0x11,0x10,0x10,0x10,0x11,0x0e},
        {0x1e,0x11,0x11,0x11,0x11,0x11,0x1e},
        {0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f},
        {0x1f,0x10,0x10,0x1e,0x10,0x10,0x10},
        {0x0e,0x11,0x10,0x17,0x11,0x11,0x0f},
        {0x11,0x11,0x11,0x1f,0x11,0x11,0x11},
        {0x0e,0x04,0x04,0x04,0x04,0x04,0x0e},
        {0x07,0x02,0x02,0x02,0x12,0x12,0x0c},
        {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
        {0x10,0x10,0x10,0x10,0x10,0x10,0x1f},
        {0x11,0x1b,0x15,0x15,0x11,0x11,0x11},
        {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
        {0x0e,0x11,0x11,0x11,0x11,0x11,0x0e},
        {0x1e,0x11,0x11,0x1e,0x10,0x10,0x10},
        {0x0e,0x11,0x11,0x11,0x15,0x12,0x0d},
        {0x1e,0x11,0x11,0x1e,0x14,0x12,0x11},
        {0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e},
        {0x1f,0x04,0x04,0x04,0x04,0x04,0x04},
        {0x11,0x11,0x11,0x11,0x11,0x11,0x0e},
        {0x11,0x11,0x11,0x11,0x11,0x0a,0x04},
        {0x11,0x11,0x11,0x15,0x15,0x15,0x0a},
        {0x11,0x11,0x0a,0x04,0x0a,0x11,0x11},
        {0x11,0x11,0x0a,0x04,0x04,0x04,0x04},
        {0x1f,0x01,0x02,0x04,0x08,0x10,0x1f}
    };

    if (row <= 0 || row >= ZZ9K_FALLBACK_FONT_H)
        return 0;
    --row;
    if (c >= 'a' && c <= 'z')
        c -= 'a' - 'A';
    if (c >= 'A' && c <= 'Z')
        return letters[c - 'A'][row];
    if (c >= '0' && c <= '9')
        return digits[c - '0'][row];
    if (c == ' ') return space[row];
    if (c == '-') return dash[row];
    if (c == '.') return dot[row];
    if (c == ':') return colon[row];
    if (c == '/' || c == '\\') return slash[row];
    return quest[row];
}

static void
zz9k_console_clear_line(y)
int y;
{
    zz9k_console_fill_rect(0, y, zz9k_con_w, zz9k_console_font_h,
        zz9k_console_bg);
}

static void
zz9k_console_clear_eol()
{
    int y, px;

    px = zz9k_console_col * zz9k_console_font_w;
    y = zz9k_console_row * zz9k_console_font_h;
    zz9k_console_fill_rect(px, y, zz9k_con_w - px,
        zz9k_console_font_h, zz9k_console_bg);
}

static void
zz9k_console_clear()
{
    zz9k_console_fill_rect(0, 0, zz9k_con_w, zz9k_con_h,
        zz9k_console_bg);
}

/*
 * The console's screen is con_h rows from framebuffer row zz9k_con_top.  With room for
 * 2 * con_h + font_h rows the console pans instead of copying: a scroll moves the top
 * down one text row (a PAN write) and clears the rows that come into view, and when the
 * ring runs out it copies the screen back to row 0 -- out of sight, once every con_h /
 * font_h lines.  Without the room (the ZZ9000's Zorro II window) it copies every time.
 */
static int zz9k_con_top;
static int zz9k_con_ringh;

#define ZZ9K_CON_PIX(x, y) ((unsigned short *)zz9k_console_bp->fb + \
    ((unsigned long)(zz9k_con_top + (y)) * zz9k_con_w + (x)))

/* the console may use the blitter registers now */
static int
zz9k_con_blit_ok()
{
    return !zz9k_blit_inuse && zz9k_can_blit(zz9k_console_bp);
}

static void
zz9k_console_fill_rect(x, y, w, h, color)
int x, y, w, h;
unsigned short color;
{
    int py;

    if (!zz9k_console_bp || !zz9k_console_bp->base)
        return;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x >= zz9k_con_w || y >= zz9k_con_h || w <= 0 || h <= 0)
        return;
    if (x + w > zz9k_con_w)
        w = zz9k_con_w - x;
    if (y + h > zz9k_con_h)
        h = zz9k_con_h - y;

    if (zz9k_con_blit_ok()) {
        ZZ9K_BLIT_FORGET(zz9k_console_bp);
        zz9k_blit_run(zz9k_console_bp, -1, 0, x, zz9k_con_top + y, w, h, color);
        ZZ9K_BLIT_FORGET(zz9k_console_bp);
        return;
    }
    for (py = 0; py < h; ++py)
        zz9k_fill16(ZZ9K_CON_PIX(x, y + py), (long)w, color);
}

/* n whole framebuffer rows from row src up to row dst (dst < src) */
static void
zz9k_con_rows_up(src, dst, n)
int src, dst, n;
{
    unsigned short *fb;
    int i;

    if (n <= 0)
        return;
    if (zz9k_con_blit_ok()) {
        ZZ9K_BLIT_FORGET(zz9k_console_bp);
        zz9k_blit_run(zz9k_console_bp, 0, src, 0, dst, zz9k_con_w, n, 0);
        ZZ9K_BLIT_FORGET(zz9k_console_bp);
        return;
    }
    fb = (unsigned short *)zz9k_console_bp->fb;
    for (i = 0; i < n; ++i)
        zz9k_copy16(fb + (unsigned long)(dst + i) * zz9k_con_w,
            fb + (unsigned long)(src + i) * zz9k_con_w, (long)zz9k_con_w);
}

static void
zz9k_console_scroll()
{
    int fh, keep;

    fh = zz9k_console_font_h;
    keep = zz9k_con_h - fh;
    if (zz9k_con_ringh > zz9k_con_h && !zz9k_blit_inuse) {
        if (zz9k_con_top + zz9k_con_h + fh > zz9k_con_ringh) {
            /* the window is past con_h: rows [0, con_h) are out of sight */
            zz9k_con_rows_up(zz9k_con_top + fh, 0, keep);
            zz9k_con_top = 0;
        } else
            zz9k_con_top += fh;
        zz9k_console_clear_line(keep);          /* below the window still shown */
        zz9k_set_pan(zz9k_console_bp, (unsigned long)zz9k_con_top * zz9k_con_w * 2);
        return;
    }
    /* no pan room, or an interrupted blitter operation owns the registers */
    zz9k_con_rows_up(zz9k_con_top + fh, zz9k_con_top, keep);
    zz9k_console_clear_line(keep);
}

static void
zz9k_console_draw(c)
unsigned char c;
{
    unsigned short *p;
    unsigned char bits;
    unsigned char *data;
    unsigned char *rowdata;
    struct zz9k_charent *ce;
    unsigned short rowbits;
    int px, py, x, y;
    int on;

    x = zz9k_console_col * zz9k_console_font_w;
    y = zz9k_console_row * zz9k_console_font_h;
    ce = (struct zz9k_charent *)0;
    data = (unsigned char *)0;
    if (zz9k_console_font &&
        zz9k_console_font->f_magic == ZZ9K_FONT_MAGIC) {
        ce = &zz9k_console_font->f_ctable[c];
        if (ce->ce_data == 0 && c != ' ')
            ce = &zz9k_console_font->f_ctable['?'];
        if (ce->ce_data)
            data = (unsigned char *)zz9k_console_font + ce->ce_data;
    }

    bits = 0;
    for (py = 0; py < zz9k_console_font_h; ++py) {
        if (!data)
            bits = zz9k_glyph(c, py);   /* the 5x7 glyphs only without a font */
        p = ZZ9K_CON_PIX(x, y + py);
        for (px = 0; px < zz9k_console_font_w; ++px) {
            on = 0;
            if (data) {
                if (py >= ce->ce_height || px >= ce->ce_width)
                    ;
                else if (ce->ce_bpl == 2) {
                    rowdata = (unsigned char *)zz9k_console_font +
                        ce->ce_data + (py * ce->ce_bpl);
                    rowbits = ((unsigned short)rowdata[0] << 8) |
                        rowdata[1];
                    if (ce->ce_width <= 16)
                        on = rowbits & (1 << (ce->ce_width - 1 - px));
                } else if (px < (ce->ce_bpl << 3)) {
                    bits = data[(py * ce->ce_bpl) + (px >> 3)];
                    on = bits & (0x80 >> (px & 7));
                }
            } else if (py < ZZ9K_FALLBACK_FONT_H &&
                px < ZZ9K_FALLBACK_FONT_W) {
                on = bits & (0x10 >> px);
            }
            *p++ = on ? zz9k_console_fg : zz9k_console_bg;
        }
    }
}

static void
zz9k_console_cursor(on)
int on;
{
    unsigned short *p;
    int px, py, x, y;

    if (!zz9k_console_bp)
        return;
    if (on == zz9k_console_cursor_on)
        return;

    x = zz9k_console_col * zz9k_console_font_w;
    y = zz9k_console_row * zz9k_console_font_h;
    for (py = 0; py < zz9k_console_font_h; ++py) {
        p = ZZ9K_CON_PIX(x, y + py);
        for (px = 0; px < zz9k_console_font_w; ++px) {
            *p = *p ^ ZZ9K_CURSOR_XOR;
            ++p;
        }
    }
    zz9k_console_cursor_on = on;
}

static unsigned short
zz9k_console_color(n)
int n;
{
    static unsigned short colors[8] = {
        0x0000, 0xf800, 0x07e0, 0xffe0,
        0x001f, 0xf81f, 0x07ff, 0xffff
    };

    if (n < 0 || n > 7)
        return ZZ9K_FG;
    return colors[n];
}

static void
zz9k_console_reset_attr()
{
    zz9k_console_fg = ZZ9K_FG;
    zz9k_console_bg = ZZ9K_BG;
}

static void
zz9k_console_newline()
{
    zz9k_console_col = 0;
    if (++zz9k_console_row >= zz9k_console_rows) {
        zz9k_console_scroll();
        zz9k_console_row = zz9k_console_rows - 1;
    }
}

static void
zz9k_console_move(row, col)
int row;
int col;
{
    if (row < 0)
        row = 0;
    if (col < 0)
        col = 0;
    if (row >= zz9k_console_rows)
        row = zz9k_console_rows - 1;
    if (col >= zz9k_console_cols)
        col = zz9k_console_cols - 1;
    zz9k_console_row = row;
    zz9k_console_col = col;
}

static int
zz9k_console_argn(n, def)
int n;
int def;
{
    if (n < 0 || n > zz9k_console_argc)
        return def;
    if (zz9k_console_args[n] == 0)
        return def;
    return zz9k_console_args[n];
}

static void
zz9k_console_sgr()
{
    int i, a;
    unsigned short tmp;

    if (zz9k_console_argc == 0 && zz9k_console_args[0] == 0) {
        zz9k_console_reset_attr();
        return;
    }
    for (i = 0; i <= zz9k_console_argc; ++i) {
        a = zz9k_console_args[i];
        if (a == 0)
            zz9k_console_reset_attr();
        else if (a == 1)
            zz9k_console_fg = ZZ9K_FG;          /* bold: white */
        else if (a == 7) {
            tmp = zz9k_console_fg;
            zz9k_console_fg = zz9k_console_bg;
            zz9k_console_bg = tmp;
        } else if (a >= 30 && a <= 37)
            zz9k_console_fg = zz9k_console_color(a - 30);
        else if (a >= 40 && a <= 47)
            zz9k_console_bg = zz9k_console_color(a - 40);
    }
}

/* end the escape sequence: back to text */
static void
zz9k_console_esc_reset()
{
    zz9k_console_esc = ZZ9K_ESC_NONE;
    zz9k_console_priv = 0;
    zz9k_console_arg = 0;
    zz9k_console_argc = 0;
    zz9k_console_args[0] = 0;
    zz9k_console_args[1] = 0;
    zz9k_console_args[2] = 0;
    zz9k_console_args[3] = 0;
}

/*
 * One byte of a control sequence (ESC [ ...), as ECMA-48 lays it out: parameter bytes 0x30-
 * 0x3F, then intermediate bytes 0x20-0x2F, then one final byte 0x40-0x7E.  Digits and ';'
 * are the parameters this renderer knows; a private marker ('<' '=' '>' '?', or ':') or an
 * intermediate makes the sequence one it does not know, and its final byte then ends it
 * with no effect (ESC [ ? 25 l, hide the cursor, used to print "25l").  A number stops
 * growing at four digits.  Any other byte abandons the sequence.
 */
static void
zz9k_console_csi(c)
unsigned char c;
{
    int n;
    int *a;

    if (c >= '0' && c <= '9') {
        a = &zz9k_console_args[zz9k_console_arg];
        if (*a < 1000)
            *a = *a * 10 + c - '0';
        return;
    }
    if (c == ';') {
        if (zz9k_console_arg < 3)
            ++zz9k_console_arg;
        if (zz9k_console_arg > zz9k_console_argc)
            zz9k_console_argc = zz9k_console_arg;
        return;
    }
    if ((c >= 0x3a && c <= 0x3f) || (c >= 0x20 && c <= 0x2f)) {
        zz9k_console_priv = 1;
        return;
    }
    if (c < 0x40 || c > 0x7e || zz9k_console_priv) {
        zz9k_console_esc_reset();
        return;
    }
    if (zz9k_console_arg > zz9k_console_argc)
        zz9k_console_argc = zz9k_console_arg;
    n = zz9k_console_argn(0, 1);

    switch (c) {
    case 'A':
        zz9k_console_move(zz9k_console_row - n, zz9k_console_col);
        break;
    case 'B':
        zz9k_console_move(zz9k_console_row + n, zz9k_console_col);
        break;
    case 'C':
        zz9k_console_move(zz9k_console_row, zz9k_console_col + n);
        break;
    case 'D':
        zz9k_console_move(zz9k_console_row, zz9k_console_col - n);
        break;
    case 'H':
    case 'f':
        zz9k_console_move(zz9k_console_argn(0, 1) - 1,
            zz9k_console_argn(1, 1) - 1);
        break;
    case 'J':
        if (zz9k_console_args[0] == 2) {
            zz9k_console_clear();
            zz9k_console_move(0, 0);
        } else if (zz9k_console_args[0] == 1) {
            zz9k_console_fill_rect(0, 0, zz9k_con_w,
                zz9k_console_row * zz9k_console_font_h, zz9k_console_bg);
            zz9k_console_fill_rect(0,
                zz9k_console_row * zz9k_console_font_h,
                (zz9k_console_col + 1) * zz9k_console_font_w,
                zz9k_console_font_h, zz9k_console_bg);
        } else {
            zz9k_console_clear_eol();
            zz9k_console_fill_rect(0,
                (zz9k_console_row + 1) * zz9k_console_font_h,
                zz9k_con_w,
                zz9k_con_h -
                ((zz9k_console_row + 1) * zz9k_console_font_h),
                zz9k_console_bg);
        }
        break;
    case 'K':
        zz9k_console_clear_eol();
        break;
    case 'm':
        zz9k_console_sgr();
        break;
    }
    zz9k_console_esc_reset();
}

/* A C0 control (or DEL): the four the console acts on; the rest draw nothing. */
static void
zz9k_console_ctl(c)
unsigned char c;
{
    switch (c) {
    case '\r':
        zz9k_console_col = 0;
        break;
    case '\n':
        zz9k_console_newline();
        break;
    case '\t':
        zz9k_console_col = (zz9k_console_col + 8) & ~7;
        if (zz9k_console_col >= zz9k_console_cols)
            zz9k_console_newline();
        break;
    case '\b':
        if (zz9k_console_col)
            --zz9k_console_col;
        break;
    }
}

static void
zz9k_conputc(c)
unsigned char c;
{
    if (!zz9k_console_on || zz9k_console_paused || !zz9k_console_bp)
        return;

    zz9k_console_cursor(0);

    if (zz9k_console_esc == ZZ9K_ESC_CSI) {
        if (c == 0x1b)
            zz9k_console_esc = ZZ9K_ESC_ESC;    /* a new escape abandons this one */
        else if (c == 0x18 || c == 0x1a)        /* CAN, SUB */
            zz9k_console_esc_reset();
        else if (c < 0x20)
            zz9k_console_ctl(c);                /* a control inside a sequence acts */
        else
            zz9k_console_csi(c);
        zz9k_console_cursor(1);
        return;
    }
    if (zz9k_console_esc == ZZ9K_ESC_ESC) {
        zz9k_console_esc_reset();
        if (c == '[')
            zz9k_console_esc = ZZ9K_ESC_CSI;
        else if (c == 0x1b)
            zz9k_console_esc = ZZ9K_ESC_ESC;
        zz9k_console_cursor(1);
        return;
    }
    if (c == 0x1b) {
        zz9k_console_esc = ZZ9K_ESC_ESC;
        zz9k_console_cursor(1);
        return;
    }
    if (c < 0x20 || c == 0x7f) {
        zz9k_console_ctl(c);
        zz9k_console_cursor(1);
        return;
    }
    zz9k_console_draw(c);
    if (++zz9k_console_col >= zz9k_console_cols) {
        zz9k_console_newline();
    }
    zz9k_console_cursor(1);
}

static int
zz9k_console_start(bp)
struct zz9k_board *bp;
{
    unsigned short id;

    if (!bp || !bp->base)
        return ENXIO;
    id = zz9k_mode_id(bp, bp->bt->con_width, bp->bt->con_height);
    if (id == ZZ9K_NOMODE)
        return EINVAL;
    zz9k_con_w = bp->bt->con_width;
    zz9k_con_h = bp->bt->con_height;
    if ((unsigned long)zz9k_con_w * zz9k_con_h * 2 > bp->fb_kmap)
        return ENOMEM;

    zz9k_set_mode_hw(bp, id, ZZ9K_CON_COLOR, 0);
    bp->width = zz9k_con_w;
    bp->height = zz9k_con_h;
    zz9k_set_switch_hw(bp, ZZ9K_SWITCH_RTG);
    ZZ9K_MONSW_SET(1);

    zz9k_console_bp = bp;
    zz9k_console_font = builtinfont[0];
    /* a font the renderer can draw: rows of at most 16 pixels, 1 to 32 rows */
    if (!zz9k_console_font ||
        zz9k_console_font->f_magic != ZZ9K_FONT_MAGIC ||
        zz9k_console_font->f_height == 0 || zz9k_console_font->f_height > 32 ||
        zz9k_console_font->f_ctable['M'].ce_move > 15) {
        zz9k_console_font = (struct zz9k_font *)0;
        zz9k_console_font_w = ZZ9K_FALLBACK_FONT_W;
        zz9k_console_font_h = ZZ9K_FALLBACK_FONT_H;
    } else {
        zz9k_console_font_w =
            zz9k_console_font->f_ctable['M'].ce_move + 1;
        zz9k_console_font_h = zz9k_console_font->f_height;
    }
    zz9k_console_cols = zz9k_con_w / zz9k_console_font_w;
    zz9k_console_rows = zz9k_con_h / zz9k_console_font_h;
    zz9k_con_top = 0;
    zz9k_con_ringh = zz9k_con_h;
    if ((unsigned long)(2 * zz9k_con_h + zz9k_console_font_h) * zz9k_con_w * 2 <= bp->fb_kmap)
        zz9k_con_ringh = 2 * zz9k_con_h + zz9k_console_font_h;
    zz9k_console_col = 0;
    zz9k_console_row = 0;
    zz9k_console_cursor_on = 0;
    zz9k_console_esc_reset();
    zz9k_console_reset_attr();
    zz9k_console_paused = 0;
    zz9k_console_clear();

    zz9k_console_on = 1;
    zz9k_console_cursor(1);
    return 0;
}

/*
 * The renderer's guard.  The console is written at the writer's spl, which may be an
 * interrupt's (a printf from a handler), so a write can arrive in the middle of another:
 * inside an escape sequence, a scroll, or with the cursor drawn.  Every way into the
 * renderer takes the busy flag at spl7 -- which raises to level 4 only in these kernels, so
 * levels 5 and 6 still come through -- and a writer that finds it taken, i.e. one that
 * interrupted the renderer, puts its bytes in a small ring instead.  The owner draws them
 * before it lets go.  Bytes that do not fit are dropped and counted (zz9k_con_lost).
 */
#define ZZ9K_CON_RING   256     /* a power of two */

static int zz9k_con_busy;
static unsigned char zz9k_con_ring[ZZ9K_CON_RING];
static int zz9k_con_head;
static int zz9k_con_tail;
long zz9k_con_lost = 0;         /* bytes the ring dropped */

/* Called at spl7 with the renderer busy: keep p[0..n) for its owner. */
static void
zz9k_con_defer(p, n)
unsigned char *p;
unsigned n;
{
    int next;

    for (; n; --n, ++p) {
        next = (zz9k_con_head + 1) & (ZZ9K_CON_RING - 1);
        if (next == zz9k_con_tail) {
            ++zz9k_con_lost;
            continue;
        }
        zz9k_con_ring[zz9k_con_head] = *p;
        zz9k_con_head = next;
    }
}

/* Raise to spl7 (the old level in *sp) and take the renderer: 1 if it is ours now. */
static int
zz9k_con_take(sp)
int *sp;
{
    *sp = spl7();
    if (zz9k_con_busy)
        return 0;
    zz9k_con_busy = 1;
    return 1;
}

/* Called at spl7 by the owner: draw what others left, let go, back to level s. */
static void
zz9k_con_release(s)
int s;
{
    unsigned char c;

    for (;;) {
        while (zz9k_con_tail != zz9k_con_head) {
            c = zz9k_con_ring[zz9k_con_tail];
            zz9k_con_tail = (zz9k_con_tail + 1) & (ZZ9K_CON_RING - 1);
            zz9k_conputc(c);
        }
        zz9k_con_busy = 0;
        if (zz9k_con_tail == zz9k_con_head)
            break;
        zz9k_con_busy = 1;          /* a byte came in after the last look */
    }
    splx(s);
}

/* p[0..n) through the renderer, at spl7; kept for the owner if it is busy. */
static void
zz9k_con_write(p, n)
unsigned char *p;
unsigned n;
{
    int s;

    if (!zz9k_con_take(&s)) {
        zz9k_con_defer(p, n);
        splx(s);
        return;
    }
    while (n--)
        zz9k_conputc(*p++);
    zz9k_con_release(s);
}

/*
 * A long job on the console's state (start, stop) from process context: it holds the
 * flag, not the level, so the clock keeps ticking through a full-screen clear; interrupt
 * writers meanwhile go to the ring.  0 = EBUSY (never seen from process context).
 */
static int
zz9k_con_hold()
{
    int s;

    if (!zz9k_con_take(&s)) {
        splx(s);
        return 0;
    }
    splx(s);
    return 1;
}

static void
zz9k_con_unhold()
{
    zz9k_con_release(spl7());
}

/*
 * ZZ9KIOC_CONSOLE ENABLE and REDIRECT (the same): take the kernel's console output first,
 * then switch the monitor and clear the screen, so a refusal (the core's one hook already
 * taken: EBUSY) leaves the display as it was.  Until zz9k_console_redirect is set the hook
 * passes every write on.  A second ENABLE restarts the console and keeps the hook.
 */
static int
zz9k_console_enable(bp)
struct zz9k_board *bp;
{
    int error;
#ifdef ZZ9K_MODULE
    int hooked;

    hooked = 0;
    if (!zz9k_console_redirect) {
        error = cons_hook_set(zz9k_cons_hook);
        if (error)
            return error;
        hooked = 1;
    }
#endif
    if (!zz9k_con_hold())
        error = EBUSY;
    else {
        error = zz9k_console_start(bp);
        zz9k_con_unhold();
    }
    if (error) {
#ifdef ZZ9K_MODULE
        if (hooked)
            cons_hook_clear();
#endif
        return error;
    }
#ifndef ZZ9K_MODULE
    if (!zz9k_old_conputc)
        zz9k_old_conputc = conputc;
    conputc = zz9k_kputc;
#endif
    zz9k_console_redirect = 1;
    return 0;
}

static void
zz9k_console_disable()
{
    struct zz9k_board *bp;

    if (!zz9k_con_hold())
        return;
    bp = zz9k_console_bp;
    zz9k_console_cursor(0);
#ifdef ZZ9K_MODULE
    if (zz9k_console_redirect)          /* the hook is ours only once a set succeeded */
        cons_hook_clear();
#else
    if (zz9k_old_conputc)
        conputc = zz9k_old_conputc;
#endif
    zz9k_console_redirect = 0;
    zz9k_console_on = 0;
    zz9k_con_unhold();                  /* what interrupted us is dropped: the console is off */
    if (bp)
        zz9k_set_switch_hw(bp, ZZ9K_SWITCH_CAPTURE);
    ZZ9K_MONSW_SET(0);
}

int
zz9000consolewrite(p, n)
unsigned char *p;
unsigned n;
{
    int s;

    if (!zz9k_console_redirect || zz9k_console_paused)
        return 0;
    if (!zz9k_con_take(&s)) {
        zz9k_con_defer(p, n);
        splx(s);
        return 1;
    }
    if (zz9k_console_bp) {
        zz9k_set_switch_hw(zz9k_console_bp, ZZ9K_SWITCH_RTG);
        ZZ9K_MONSW_SET(1);
    }
    while (n--)
        zz9k_conputc(*p++);
    zz9k_con_release(s);
    return 1;
}

#ifndef ZZ9K_MODULE
/* the static kernel's conputc while the console is redirected: one byte, guarded */
static void
zz9k_kputc(c)
int c;
{
    unsigned char b;

    b = (unsigned char)c;
    (void)zz9000consolewrite(&b, 1);
}
#endif

#ifdef ZZ9K_MODULE
/*
 * The module's console hook (cons_hook_set): the core calls it with every write the stock
 * scrcon_put would draw, from a virtual terminal or from printf.  Its first argument is the
 * struct console being written, not a screen, so the c1.c hook's test (integration/
 * c1-console.txt) reads the console's screen: only the console on display is taken.
 */
static int
zz9k_cons_hook(cp, p, n)
struct console *cp;
unsigned char *p;
unsigned n;
{
    if (cp->screen != displayedscreen)
        return 0;
    return zz9000consolewrite(p, n);
}
#endif

static void
zz9k_console_puts(s)
char *s;
{
    char *e;

    for (e = s; *e; ++e)
        ;
    zz9k_con_write((unsigned char *)s, (unsigned)(e - s));
}

static int
zz9k_console_test(bp)
struct zz9k_board *bp;
{
    int row, tens, ones, error;
    unsigned char digit;

    if (!zz9k_console_on || zz9k_console_paused) {
        if (!zz9k_con_hold())
            return EBUSY;
        error = zz9k_console_start(bp);
        zz9k_con_unhold();
        if (error)
            return ENXIO;
    }
    zz9k_set_switch_hw(zz9k_console_bp, ZZ9K_SWITCH_RTG);
    ZZ9K_MONSW_SET(1);

    zz9k_console_puts("\033[2J");
    zz9k_console_puts("ZZ9000 direct console renderer test\n");
    zz9k_console_puts("ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789\n");
    zz9k_console_puts("THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG\n");
    zz9k_console_puts("\033[31mRED\033[0m \033[32mGREEN\033[0m ");
    zz9k_console_puts("\033[33mYELLOW\033[0m \033[34mBLUE\033[0m ");
    zz9k_console_puts("\033[35mMAGENTA\033[0m \033[36mCYAN\033[0m\n");
    zz9k_console_puts("cursor movement test: start\033[2DOK\n");
    zz9k_console_puts("clear to eol test: this tail should disappear\033[19D\033[Kdone\n");
    zz9k_console_puts("\nscroll/check rows:\n");
    for (row = 0; row < zz9k_console_rows + 4; ++row) {
        zz9k_console_puts("row ");
        /* no division by a constant: gcc -O would emit a 64-bit mulsl,
         * which the 68060 does not implement in supervisor mode */
        for (tens = 0, ones = row; ones >= 10; ones -= 10)
            if (++tens >= 10)
                tens = 0;
        digit = '0' + tens;
        zz9k_con_write(&digit, 1);
        digit = '0' + ones;
        zz9k_con_write(&digit, 1);
        zz9k_console_puts(" -- no garbage should remain after this line\n");
    }
    return 0;
}

#ifndef ZZ9K_MODULE
void
zz9000init()
{
    (void)zz9k_probe(0);
}
#endif

int
zz9000open(devp, mode, type, cr)
dev_t *devp;
int mode;
int type;
struct cred *cr;
{
    int unit;

    unit = ZZ9K_UNIT(*devp);
    if (unit >= ZZ9K_MAXBOARDS)
        return ENXIO;
    return 0;
}

int
zz9000close(dev, mode, type, cr)
dev_t dev;
int mode;
int type;
struct cred *cr;
{
    int unit;

    unit = ZZ9K_UNIT(dev);
    if (ZZ9K_ISINFO(dev))               /* the information node holds nothing */
        return 0;
    /*
     * SVR4 calls close on the last close only, and the kernel closes a dying process's
     * descriptors, so a crashed or killed X server lands here too: back to native,
     * unless the kernel console is redirected to this board.
     */
#ifdef ZZ9K_MONSW
    if (unit < ZZ9K_MAXBOARDS && zz9k_boards[unit].monsw_handle >= 0 &&
        !(zz9k_console_redirect && zz9k_console_bp == &zz9k_boards[unit])) {
        monsw_lastclose(zz9k_boards[unit].monsw_handle);
        zz9k_boards[unit].svga_switch = SVGAMONITORSWITCH_Amiga;   /* what Get answers now */
    }
#endif
    /* the client that paused the console is gone: the console takes the screen back */
    if (unit < ZZ9K_MAXBOARDS && zz9k_console_redirect && zz9k_console_paused &&
        zz9k_console_bp == &zz9k_boards[unit] && zz9k_con_hold()) {
        (void)zz9k_console_start(zz9k_console_bp);
        zz9k_con_unhold();
    }
    return 0;
}

static int
zz9000rw(dev, direction, uiop, cr)
dev_t dev;
uio_rw_t direction;
struct uio *uiop;
struct cred *cr;
{
    int unit, error;
    unsigned long count, end, pos, n, limit;
    struct zz9k_board *bp;

    if (ZZ9K_ISINFO(dev))
        return ENODEV;
    unit = ZZ9K_UNIT(dev);
    if (!zz9k_probe(unit))
        return ENXIO;
    bp = &zz9k_boards[unit];

    count = uiop->uio_resid;
    if (count == 0)
        return 0;
    /* the part the kernel reaches: the whole board, or what a mapped window maps */
    limit = bp->bt->fb_offset + bp->fb_kmap;
    if (limit > bp->board_size)
        limit = bp->board_size;
    if (uiop->uio_offset < 0 ||
        (unsigned long)uiop->uio_offset >= limit)
        return ENXIO;

    end = (unsigned long)uiop->uio_offset + count;
    if (end > limit)
        count = limit - (unsigned long)uiop->uio_offset;
    if (direction == UIO_WRITE && count == 0)
        return ENXIO;
    /* the register block, as GETREG/SETREG, is privileged; the framebuffer is not */
    if ((unsigned long)uiop->uio_offset < bp->bt->fb_offset && drv_priv(cr))
        return EPERM;

    /* never carry one uiomove across the register/framebuffer seam */
    while (count) {
        pos = (unsigned long)uiop->uio_offset;
        n = count;
        if (pos < bp->bt->fb_offset && n > bp->bt->fb_offset - pos)
            n = bp->bt->fb_offset - pos;
        error = uiomove(zz9k_devphys(bp, pos), n, direction, uiop);
        if (error)
            return error;
        count -= n;
    }
    return 0;
}

int
zz9000read(dev, uiop, cr)
dev_t dev;
struct uio *uiop;
struct cred *cr;
{
    return zz9000rw(dev, UIO_READ, uiop, cr);
}

int
zz9000write(dev, uiop, cr)
dev_t dev;
struct uio *uiop;
struct cred *cr;
{
    return zz9000rw(dev, UIO_WRITE, uiop, cr);
}

int
zz9000mmap(dev, offset, maxprot)
dev_t dev;
off_t offset;
int maxprot;
{
    int unit;
    struct zz9k_board *bp;

    if (ZZ9K_ISINFO(dev))
        return -1;
    unit = ZZ9K_UNIT(dev);
    if (!zz9k_probe(unit))
        return -1;
    bp = &zz9k_boards[unit];
    if (offset < 0 || (unsigned long)offset >= bp->board_size)
        return -1;

    return phystopfn(zz9k_devpa(bp, (unsigned long)offset));
}

/*
 * FILLRECT/COPYRECT, checked by the caller: the blitter when it serves the
 * board, else the CPU loops.  Called with the CPU loops' own argument lists,
 * so the ioctl switch's cases -- and its jump table, which the relink gate's
 * scan060 reads as code -- keep their size.
 */
static void
zz9k_fillrect(bp, x, y, w, h, color)
struct zz9k_board *bp;
int x, y, w, h;
unsigned short color;
{
    ZZ9K_BLIT_FORGET(bp);
    if (zz9k_can_blit(bp))
        zz9k_blit_run(bp, -1, 0, x, y, w, h, color);
    else
        zz9k_cpu_fillrect(bp, x, y, w, h, color);
}

static void
zz9k_copyrect(bp, sx, sy, dx, dy, w, h)
struct zz9k_board *bp;
int sx, sy, dx, dy, w, h;
{
    ZZ9K_BLIT_FORGET(bp);
    if (zz9k_can_blit(bp))
        zz9k_blit_run(bp, sx, sy, dx, dy, w, h, 0);
    else
        zz9k_cpu_copyrect(bp, sx, sy, dx, dy, w, h);
}

/*
 * ZZ9KIOC_BLITS (zz9000.h): records are copied in a few at a time, each one checked just
 * before it runs.  The return is the number run; an error means none ran.  The pixel
 * budget bounds the call: interrupts are taken between bands, but the calling process
 * holds the CPU for the whole call.
 */
static int
zz9k_blits(bp, arg, rvalp)
struct zz9k_board *bp;
int arg;
int *rvalp;
{
    struct zz9k_blits bs;
    struct zz9k_blit rec[8];
    struct zz9k_blit *b;
    unsigned long done, i, k;
    long area, used;

    if (copyin((caddr_t)arg, (caddr_t)&bs, sizeof bs))
        return EFAULT;
    if (!bp->base || !zz9k_can_blit(bp))
        return ENXIO;
    if (bs.n > ZZ9K_BLITS_MAX)
        return EINVAL;
    ZZ9K_BLIT_FORGET(bp);
    used = 0;
    for (done = 0; done < bs.n; done += k) {
        k = bs.n - done;
        if (k > 8)
            k = 8;
        if (copyin((caddr_t)(bs.recs + done), (caddr_t)rec,
                   (unsigned)(k * sizeof rec[0]))) {
            if (done == 0)
                return EFAULT;
            *rvalp = (int)done;
            return 0;
        }
        ZZ9K_BLIT_FORGET(bp);           /* the copyin may have slept */
        for (i = 0; i < k; i++) {
            b = &rec[i];
            if (!zz9k_rect_ok(bp, (unsigned long)b->x, (unsigned long)b->y,
                              (unsigned long)b->width, (unsigned long)b->height) ||
                (b->op == ZZ9K_BLIT_COPY &&
                 !zz9k_rect_ok(bp, (unsigned long)b->src_x, (unsigned long)b->src_y,
                               (unsigned long)b->width, (unsigned long)b->height)) ||
                (b->op != ZZ9K_BLIT_FILL && b->op != ZZ9K_BLIT_COPY)) {
                if (done + i == 0)
                    return EINVAL;
                *rvalp = (int)(done + i);
                return 0;
            }
            area = (long)b->width * b->height;
            if (done + i > 0 && area > ZZ9K_BLITS_PIXELS - used) {
                *rvalp = (int)(done + i);
                return 0;
            }
            used += area;
            if (b->op == ZZ9K_BLIT_FILL)
                zz9k_blit_run(bp, -1, 0, b->x, b->y, b->width, b->height,
                    (unsigned short)b->color);
            else
                zz9k_blit_run(bp, b->src_x, b->src_y, b->x, b->y,
                    b->width, b->height, 0);
        }
    }
    *rvalp = (int)done;
    return 0;
}

/* ZZ9KIOC_GETMODES (zz9000.h): the row's mode table. */
static int
zz9k_getmodes(bp, arg)
struct zz9k_board *bp;
int arg;
{
    struct zz9k_modes ms;
    struct zz9k_modetab *m;
    int i;

    bzero((caddr_t)&ms, sizeof ms);
    for (i = 0, m = bp->bt->modes; m->width && i < ZZ9K_MODES_MAX; ++i, ++m) {
        ms.mode[i].width = m->width;
        ms.mode[i].height = m->height;
        ms.mode[i].id = m->id;
    }
    ms.count = (unsigned short)i;
    if (copyout((caddr_t)&ms, (caddr_t)arg, sizeof ms))
        return EFAULT;
    return 0;
}

/*
 * ZZ9KIOC_SENSORS (zz9000.h): reads only.  The firmware answers each register itself, in
 * a few microseconds; a register it does not have reads as all ones.
 */
static int
zz9k_sensors(bp, arg)
struct zz9k_board *bp;
int arg;
{
    struct zz9k_sensors ss;
    struct zz9k_sensreg *t;
    unsigned long raw;
    int i;

    if (copyin((caddr_t)arg, (caddr_t)&ss.size, sizeof ss.size))
        return EFAULT;
    if (ss.size < sizeof ss)
        return EINVAL;
    bzero((caddr_t)&ss, sizeof ss);
    ss.size = sizeof ss;
    if (bp->bt->sens && bp->bt->regw == 4) {
        for (i = 0, t = bp->bt->sens; i < ZZ9K_SENS_N; ++i, ++t) {
            if (t->off == ZZ9K_NOREG)
                continue;
            raw = *((volatile unsigned long *)((char *)bp->base + t->off));
            if (raw > t->max || (i == ZZ9K_SENS_CPU_MHZ && raw == 0))
                continue;
            ss.value[i] = (long)(raw * t->mul);
            ss.valid |= (unsigned long)1 << i;
        }
        if (ss.valid)
            ss.count = ZZ9K_SENS_N;
    }
    if (copyout((caddr_t)&ss, (caddr_t)arg, sizeof ss))
        return EFAULT;
    return 0;
}

/*
 * A GETREG (set 0) or SETREG (set 1) of the 16-bit lane at off: inside the page, even, and
 * in one of the row's windows (zz9000.h, struct zz9k_regwin).
 */
static int
zz9k_reg_allowed(bp, off, set)
struct zz9k_board *bp;
unsigned long off;
int set;
{
    struct zz9k_regwin *w;

    if (off >= bp->bt->reg_size || (off & 1) || !bp->bt->regwin)
        return 0;
    for (w = bp->bt->regwin; w->hi; ++w)
        if (off >= w->lo && off + 2 <= w->hi)
            return !set || w->set;
    return 0;
}

/*
 * Who may issue what.  A command that changes the board, the screen or the console needs a
 * descriptor open for writing (EBADF otherwise).  Raw register access, the console switch,
 * the probe and the capture switch need privilege as well (drv_priv, EPERM): the register
 * page also carries other drivers' registers, and the console takes the kernel's output.
 * The rest -- GETINFO, GETFBINFO, GETAUTOCON, the monitor-switch Get -- is open to any
 * descriptor.  A table rather than a switch, so the ioctl's own switch keeps its shape.
 */
#define ZZ9K_NEED_WRITE  1
#define ZZ9K_NEED_PRIV   2

static struct zz9k_ioneed {
    int cmd;
    int need;
} zz9k_ioneed[] = {
    { ZZ9KIOC_SETMODE,   ZZ9K_NEED_WRITE },
    { ZZ9KIOC_SETPAN,    ZZ9K_NEED_WRITE },
    { ZZ9KIOC_SETSWITCH, ZZ9K_NEED_WRITE | ZZ9K_NEED_PRIV },
    { ZZ9KIOC_FILL,      ZZ9K_NEED_WRITE },
    { ZZ9KIOC_GETREG,    ZZ9K_NEED_PRIV },
    { ZZ9KIOC_SETREG,    ZZ9K_NEED_WRITE | ZZ9K_NEED_PRIV },
    { ZZ9KIOC_PROBE,     ZZ9K_NEED_PRIV },
    { ZZ9KIOC_CONSOLE,   ZZ9K_NEED_WRITE | ZZ9K_NEED_PRIV },
    { ZZ9KIOC_FILLRECT,  ZZ9K_NEED_WRITE },
    { ZZ9KIOC_COPYRECT,  ZZ9K_NEED_WRITE },
    { ZZ9KIOC_CONPUTS,   ZZ9K_NEED_WRITE },
    { ZZ9KIOC_BLITS,     ZZ9K_NEED_WRITE },
    { SVGAIOCSetMonitorSwitch, ZZ9K_NEED_WRITE },
    { 0, 0 }
};

static int
zz9k_ioc_allowed(cmd, mode, cr)
int cmd;
int mode;
struct cred *cr;
{
    struct zz9k_ioneed *t;

    for (t = zz9k_ioneed; t->cmd; ++t) {
        if (t->cmd != cmd)
            continue;
        if ((t->need & ZZ9K_NEED_WRITE) && !(mode & FWRITE))
            return EBADF;
        if ((t->need & ZZ9K_NEED_PRIV) && drv_priv(cr))
            return EPERM;
        return 0;
    }
    return 0;
}

int
zz9000ioctl(dev, cmd, arg, mode, cr, rvalp)
dev_t dev;
int cmd;
int arg;
int mode;
struct cred *cr;
int *rvalp;
{
    int unit;
    int error;
    int sw;
    unsigned long limit, lane;
    unsigned short id, bpp;
    struct zz9k_board *bp;
    unsigned long n;
    /* one command's argument at a time: about 330 bytes of stack instead of 650 */
    union {
        struct zz9k_info info;
        struct zz9k_fbinfo fbi;
        struct zz9k_mode sm;
        struct zz9k_pan pan;
        struct zz9k_fill zf;
        struct zz9k_fillrect fr;
        struct zz9k_copyrect crct;
        struct zz9k_reg zr;
        struct zz9k_autocon_dump dump;
        struct {
            struct zz9k_conputs cp;
            char cbuf[128];
        } c;
    } u;

    unit = ZZ9K_UNIT(dev);
    if (unit >= ZZ9K_MAXBOARDS)
        return ENXIO;
    bp = &zz9k_boards[unit];
    if (ZZ9K_ISINFO(dev) && cmd != ZZ9KIOC_GETINFO && cmd != ZZ9KIOC_GETFBINFO &&
        cmd != ZZ9KIOC_GETMODES && cmd != ZZ9KIOC_SENSORS && cmd != SVGAIOCGetMonitorSwitch)
        return EPERM;
    error = zz9k_ioc_allowed(cmd, mode, cr);
    if (error)
        return error;

    /*
     * The RTG contract's monitor switch (SVGAIOC, 0xe3xx), handled outside the switch on
     * purpose: extra cases reshape the 0x5axx switch's jump table, and one table offset once
     * decoded as a 68060-unimplemented movep in the relink gate's scan060.
     */
    if (cmd == SVGAIOCGetMonitorSwitch) {
        sw = bp->svga_switch;
        if (copyout((caddr_t)&sw, (caddr_t)arg, sizeof sw))
            return EFAULT;
        return 0;
    }
    if (cmd == SVGAIOCSetMonitorSwitch) {
        if (copyin((caddr_t)arg, (caddr_t)&sw, sizeof sw))
            return EFAULT;
        if (sw != SVGAMONITORSWITCH_Amiga && sw != SVGAMONITORSWITCH_SVGA)
            return EINVAL;
        bp->svga_switch = (short)sw;
        ZZ9K_MONSW_SET(sw == SVGAMONITORSWITCH_SVGA);
        return 0;
    }
    if (cmd == ZZ9KIOC_BLITS)           /* outside the switch for the same reason */
        return zz9k_blits(bp, arg, rvalp);

    switch (cmd) {
    case ZZ9KIOC_GETAUTOCON:
        zz9k_fill_autocon(&u.dump);
        if (copyout((caddr_t)&u.dump, (caddr_t)arg, sizeof u.dump))
            return EFAULT;
        return 0;

    case ZZ9KIOC_PROBE:
        if (!zz9k_probe(unit))
            return ENXIO;
        return 0;
    }

    if (!zz9k_probe(unit))
        return ENXIO;
    if (cmd == ZZ9KIOC_GETMODES)        /* outside the switch, as BLITS */
        return zz9k_getmodes(bp, arg);
    if (cmd == ZZ9KIOC_SENSORS)
        return zz9k_sensors(bp, arg);

    switch (cmd) {
    case ZZ9KIOC_GETINFO:
        u.info.base = (unsigned long)bp->phys;
        u.info.board_size = bp->board_size;
        u.info.fb_offset = bp->bt->fb_offset;
        u.info.fb_size = bp->fb_size;
        u.info.hw_version = bp->hw_version;
        u.info.fw_version = bp->fw_version;
        u.info.product = bp->bt->product;
        u.info.zorro = bp->bt->zorro;
        u.info.width = bp->width;
        u.info.height = bp->height;
        u.info.color_mode = bp->color_mode;
        u.info.mode = bp->mode;
        if (copyout((caddr_t)&u.info, (caddr_t)arg, sizeof u.info))
            return EFAULT;
        break;

    case ZZ9KIOC_GETFBINFO:
        zz9k_fill_fbinfo(bp, &u.fbi);
        if (copyout((caddr_t)&u.fbi, (caddr_t)arg, sizeof u.fbi))
            return EFAULT;
        break;

    case ZZ9KIOC_SETMODE:
        if (copyin((caddr_t)arg, (caddr_t)&u.sm, sizeof u.sm))
            return EFAULT;
        id = zz9k_mode_id(bp, u.sm.width, u.sm.height);
        bpp = zz9k_bpp(u.sm.color_mode);
        if (id == ZZ9K_NOMODE || bpp == 0 || u.sm.scale_mode > ZZ9K_SCALE_MAX)
            return EINVAL;
        if ((unsigned long)u.sm.width * u.sm.height * bpp > bp->fb_size)
            return ENOMEM;
        if (zz9k_console_on && zz9k_console_bp == bp && zz9k_con_hold()) {
            zz9k_console_cursor(0);
            zz9k_console_paused = 1;
            zz9k_con_unhold();
        }
        zz9k_set_mode_hw(bp, id, u.sm.color_mode, u.sm.scale_mode);
        bp->width = u.sm.width;
        bp->height = u.sm.height;
        break;

    case ZZ9KIOC_SETPAN:
        if (copyin((caddr_t)arg, (caddr_t)&u.pan, sizeof u.pan))
            return EFAULT;
        if (u.pan.offset >= bp->fb_size)
            return EINVAL;
        zz9k_set_pan(bp, u.pan.offset);
        break;

    case ZZ9KIOC_SETSWITCH:
        if (arg != ZZ9K_SWITCH_RTG && arg != ZZ9K_SWITCH_CAPTURE)
            return EINVAL;
        zz9k_set_switch_hw(bp, (unsigned short)arg);
        break;

    case ZZ9KIOC_FILL:
        if (copyin((caddr_t)arg, (caddr_t)&u.zf, sizeof u.zf))
            return EFAULT;
        if (u.zf.bytes == 0) {
            *rvalp = 0;
            break;
        }
        if (u.zf.offset >= bp->fb_kmap)
            return EINVAL;
        limit = bp->fb_kmap - u.zf.offset;
        if (limit > u.zf.bytes)
            limit = u.zf.bytes;
        if (limit > ZZ9K_FILL_MAX)
            limit = ZZ9K_FILL_MAX;
        zz9k_fill8((unsigned char *)bp->fb + u.zf.offset, limit, (unsigned char)u.zf.value);
        *rvalp = (int)limit;
        break;

    case ZZ9KIOC_FILLRECT:
        if (copyin((caddr_t)arg, (caddr_t)&u.fr, sizeof u.fr))
            return EFAULT;
        if (u.fr.width == 0 || u.fr.height == 0)
            break;
        if (u.fr.x >= bp->width || u.fr.y >= bp->height)
            return EINVAL;
        if ((unsigned long)u.fr.x + u.fr.width > bp->width)
            return EINVAL;
        if ((unsigned long)u.fr.y + u.fr.height > bp->height)
            return EINVAL;
        if (bp->color_mode != ZZ9K_COLOR_16BIT565)
            return EINVAL;
        if (!zz9k_can_blit(bp) && !zz9k_kreach(bp, (unsigned long)u.fr.y + u.fr.height))
            return ENXIO;
        zz9k_fillrect(bp, u.fr.x, u.fr.y, u.fr.width, u.fr.height,
            (unsigned short)u.fr.color);
        break;

    case ZZ9KIOC_COPYRECT:
        if (copyin((caddr_t)arg, (caddr_t)&u.crct, sizeof u.crct))
            return EFAULT;
        if (u.crct.width == 0 || u.crct.height == 0)
            break;
        if (u.crct.src_x >= bp->width || u.crct.src_y >= bp->height ||
            u.crct.dst_x >= bp->width || u.crct.dst_y >= bp->height)
            return EINVAL;
        if ((unsigned long)u.crct.src_x + u.crct.width > bp->width)
            return EINVAL;
        if ((unsigned long)u.crct.src_y + u.crct.height > bp->height)
            return EINVAL;
        if ((unsigned long)u.crct.dst_x + u.crct.width > bp->width)
            return EINVAL;
        if ((unsigned long)u.crct.dst_y + u.crct.height > bp->height)
            return EINVAL;
        if (bp->color_mode != ZZ9K_COLOR_16BIT565)
            return EINVAL;
        if (!zz9k_can_blit(bp) &&
            (!zz9k_kreach(bp, (unsigned long)u.crct.src_y + u.crct.height) ||
             !zz9k_kreach(bp, (unsigned long)u.crct.dst_y + u.crct.height)))
            return ENXIO;
        zz9k_copyrect(bp, u.crct.src_x, u.crct.src_y, u.crct.dst_x,
            u.crct.dst_y, u.crct.width, u.crct.height);
        break;

    /*
     * Raw 16-bit register peek/poke.  On a 32-bit row the offset selects
     * a 16-bit lane of the aligned register.  Pokes follow the ZZ9000's
     * HI/LO convention: a poke of the HI lane (offset & 2 == 0) is
     * latched, a poke of the LO lane writes the whole 32-bit register at
     * once -- a read-modify-write would not compose on the Z3660, whose
     * MODE and blitter registers read as all ones.  Only the row's own
     * registers are reached (zz9k_reg_allowed); SETREG empties the blitter
     * cache, since a poke may change a register the cache holds.
     */
    case ZZ9KIOC_GETREG:
        if (copyin((caddr_t)arg, (caddr_t)&u.zr, sizeof u.zr))
            return EFAULT;
        if (!zz9k_reg_allowed(bp, u.zr.offset, 0))
            return EINVAL;
        if (bp->bt->regw == 4) {
            lane = *((volatile unsigned long *)((char *)bp->base +
                (u.zr.offset & ~3)));
            u.zr.value = (unsigned short)((u.zr.offset & 2) ? lane : lane >> 16);
        } else
            u.zr.value = ZZ9K_REG(bp->base, u.zr.offset);
        if (copyout((caddr_t)&u.zr, (caddr_t)arg, sizeof u.zr))
            return EFAULT;
        break;

    case ZZ9KIOC_SETREG:
        if (copyin((caddr_t)arg, (caddr_t)&u.zr, sizeof u.zr))
            return EFAULT;
        if (!zz9k_reg_allowed(bp, u.zr.offset, 1))
            return EINVAL;
        ZZ9K_BLIT_FORGET(bp);           /* the poke may change what the cache holds */
        if (bp->bt->regw == 4) {
            if (u.zr.offset & 2) {
                lane = ((unsigned long)bp->reg_hi << 16) | u.zr.value;
                *((volatile unsigned long *)((char *)bp->base +
                    (u.zr.offset & ~3))) = lane;
                bp->reg_hi = 0;
            } else
                bp->reg_hi = u.zr.value;
        } else
            ZZ9K_REG(bp->base, u.zr.offset) = u.zr.value;
        break;

    /*
     * Text from userland into the framebuffer console: the same renderer
     * the kernel's own output uses, so a program's output can be shown on
     * the RTG screen without the console driver hook.
     */
    case ZZ9KIOC_CONPUTS:
        if (copyin((caddr_t)arg, (caddr_t)&u.c.cp, sizeof u.c.cp))
            return EFAULT;
        if (!zz9k_console_on || zz9k_console_bp != bp)
            return ENXIO;
        if (zz9k_console_paused)
            return EBUSY;
        while (u.c.cp.len) {
            n = u.c.cp.len > sizeof u.c.cbuf ? sizeof u.c.cbuf : u.c.cp.len;
            if (copyin((caddr_t)u.c.cp.buf, (caddr_t)u.c.cbuf, n))
                return EFAULT;
            zz9k_con_write((unsigned char *)u.c.cbuf, (unsigned)n);
            u.c.cp.buf += n;
            u.c.cp.len -= n;
        }
        break;

    case ZZ9KIOC_CONSOLE:
        if (arg == ZZ9K_CONSOLE_ENABLE) {
            error = zz9k_console_enable(bp);
            if (error)
                return error;
            printf("zz9000 console enabled\n");
            break;
        }
        if (arg == ZZ9K_CONSOLE_DISABLE) {
            zz9k_console_disable();
            printf("zz9000 console disabled\n");
            break;
        }
        if (arg == ZZ9K_CONSOLE_REDIRECT) {
            error = zz9k_console_enable(bp);
            if (error)
                return error;
            printf("zz9000 console redirect enabled\n");
            break;
        }
        if (arg == ZZ9K_CONSOLE_TEST) {
            error = zz9k_console_test(bp);
            if (error)
                return error;
            break;
        }
        return EINVAL;

    default:
        return EINVAL;
    }

    return 0;
}

#ifdef ZZ9K_MODULE
/*
 * The module form (amix-dlm C3-6 section 3).  /dev/zz9000's row on major 49; the core loads
 * the module at the row's first open (class=demand) and fills the row's empty entries with
 * nodev, as the static row has them.  One object serves both targets: the probe walks the
 * ZZ9000 and the Z3660 rows of zz9k_boardtab[] at run time.  module/ in this repository
 * builds it with the ABI kit; module/z3660rtg.keys holds its .moddata keys.
 */
static int zz9000_load();
static int zz9000_unload();
static void zz9000_halt();

struct mod_drv_data zz9000_drvdata[] = {
    { { 0 }, 0, 0,
      { zz9000open, zz9000close, zz9000read, zz9000write, zz9000ioctl, zz9000mmap },
      49, 1 }
};

MOD_DRV_WRAPPER(zz9000, zz9000_load, zz9000_unload, zz9000_halt,
    "Z3660/ZZ9000 RTG framebuffer");

/*
 * The module's parameters replace the static kernel's patchable knobs (z3660rtg.keys):
 * blit (zz9k_blit_on), fixed (zz9k_fixed_on), maxarea (zz9k_blit_maxarea, pixels;
 * 0 = the default, 8192) and kvfb (zz9k_kvfb: MiB of the framebuffer mapped for the
 * kernel when the board lies above the identity map).
 * Above the largest mode's area (2560x1440) a cap never splits, so that is maxarea's limit.
 */
#define ZZ9K_MAXAREA_LIMIT  3686400L

/* The token p[0..n) is the word w. */
static int
zz9k_word(p, n, w)
char *p;
int n;
char *w;
{
    return strlen(w) == n && strncmp(p, w, n) == 0;
}

/* p[0..n) as a decimal in 0..ZZ9K_MAXAREA_LIMIT, else -1. */
static long
zz9k_decimal(p, n)
char *p;
int n;
{
    long v;

    if (n <= 0)
        return -1;
    for (v = 0; n--; ++p) {
        if (*p < '0' || *p > '9')
            return -1;
        v = v * 10 + (*p - '0');
        if (v > ZZ9K_MAXAREA_LIMIT)
            return -1;
    }
    return v;
}

/*
 * params: the manifest's "name=value ..." records, or 0 (C3-6 section 1.1), read here and
 * not kept.  An absent name keeps the .moddata default, which is the knob's initialiser; a
 * bad value is reported and the default kept.
 */
static void
zz9k_params(params)
char *params;
{
    char *p, *name, *val;
    int nlen, vlen;
    long v;

    for (p = params; p && *p; ) {
        while (*p == ' ')
            ++p;
        name = p;
        while (*p && *p != ' ' && *p != '=')
            ++p;
        nlen = p - name;
        val = p;
        vlen = 0;
        if (*p == '=') {
            val = ++p;
            while (*p && *p != ' ')
                ++p;
            vlen = p - val;
        }
        if (nlen == 0)
            continue;
        if (zz9k_word(name, nlen, "blit")) {
            if (zz9k_word(val, vlen, "1"))      /* bool: 0 or 1 (C3-1 section 3) */
                zz9k_blit_on = 1;
            else if (zz9k_word(val, vlen, "0"))
                zz9k_blit_on = 0;
            else
                printf("zz9000: param blit: bad value, kept %d\n", zz9k_blit_on);
        } else if (zz9k_word(name, nlen, "fixed")) {
            if (zz9k_word(val, vlen, "1"))
                zz9k_fixed_on = 1;
            else if (zz9k_word(val, vlen, "0"))
                zz9k_fixed_on = 0;
            else
                printf("zz9000: param fixed: bad value, kept %d\n", zz9k_fixed_on);
#ifdef ZZ9K_CAN_MAP
        } else if (zz9k_word(name, nlen, "kvfb")) {
            v = zz9k_decimal(val, vlen);         /* 0: the line's default */
            if (v < 0 || v > ZZ9K_KVFB_LIMIT)
                printf("zz9000: param kvfb: bad value, kept %d\n", zz9k_kvfb);
            else
                zz9k_kvfb = v ? (int)v : ZZ9K_KVFB_DEFAULT;
#endif
        } else if (zz9k_word(name, nlen, "maxarea")) {
            v = zz9k_decimal(val, vlen);
            if (v < 0)
                printf("zz9000: param maxarea: bad value, kept %d\n",
                    (int)zz9k_blit_maxarea);
            else
                zz9k_blit_maxarea = v;
        } else
            printf("zz9000: unknown param ignored\n");
    }
}

/* The console stays the ioctl's (ZZ9KIOC_CONSOLE), as in the static driver. */
static int
zz9000_load(params)
char *params;
{
    zz9k_params(params);
    return zz9k_probe(0) ? 0 : ENXIO;
}

/*
 * Called only while /dev/zz9000 is closed.  Undoes the console hook (and with it the switch
 * back to native, as ZZ9KIOC_CONSOLE DISABLE does) and the monsw registration, which keeps a
 * pointer to this module's board name.
 */
static int
zz9000_unload()
{
    struct zz9k_board *bp;

    if (zz9k_console_redirect)
        zz9k_console_disable();
    bp = &zz9k_boards[0];
    if (bp->base && bp->monsw_handle >= 0) {
        monsw_unregister(bp->monsw_handle);
        bp->monsw_handle = -1;
    }
#ifdef ZZ9K_CAN_MAP
    bp->base = 0;
    zz9k_unmap_all();                   /* a window mapped above the identity map */
#endif
    return 0;
}

/* Nothing to stop: the board neither interrupts nor writes memory. */
static void
zz9000_halt()
{
}
#endif /* ZZ9K_MODULE */
