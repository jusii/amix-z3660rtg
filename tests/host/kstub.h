/*
 * kstub.h -- the kernel the driver sees in the host tests.
 *
 * Copyright (c) 2026 Jussi Alanärä.  SPDX-License-Identifier: MIT
 *
 * Read before zz9000.c, after the host's own headers.  "User space" is the
 * array uspace[]: an ioctl's arg and the pointers inside its records are
 * offsets into it, so copyin/copyout can check them and a bad one gives
 * EFAULT as on the box.  The board windows are real host memory at their
 * Amiga addresses (zz9k_host.c maps them), so the driver's own address
 * checks run unchanged.
 */

#include <stdarg.h>

#define FREAD   0x01
#define FWRITE  0x02

typedef enum { UIO_READ, UIO_WRITE } uio_rw_t;
struct uio {
    long uio_offset;
    long uio_resid;
    char *uio_ubase;            /* the host buffer behind it */
};
struct cred {
    int cr_uid;
};

#define getminor(d)     ((int)((d) & 0xff))
#define phystopfn(p)    ((int)((unsigned long)(p) >> 12))

#define USPACE  (1 << 20)
extern char uspace[USPACE];
extern struct cred kroot, kuser;

static void (*kcopyin_hook)();     /* runs at each copyin: another process, as if it slept */

static int
copyin(u, k, n)
char *u, *k;
unsigned long n;
{
    unsigned long a = (unsigned long)u;

    if (kcopyin_hook)
        (*kcopyin_hook)();

    if (a < 16 || a > USPACE || n > USPACE - a)
        return -1;
    memcpy(k, uspace + a, n);
    return 0;
}

#define bzero(p, n)     memset((p), 0, (n))

static int
copyout(k, u, n)
char *k, *u;
unsigned long n;
{
    unsigned long a = (unsigned long)u;

    if (a < 16 || a > USPACE || n > USPACE - a)
        return -1;
    memcpy(uspace + a, k, n);
    return 0;
}

static int
uiomove(k, n, rw, uio)
char *k;
long n;
uio_rw_t rw;
struct uio *uio;
{
    if (rw == UIO_READ)
        memcpy(uio->uio_ubase, k, n);
    else
        memcpy(k, uio->uio_ubase, n);
    uio->uio_ubase += n;
    uio->uio_offset += n;
    uio->uio_resid -= n;
    return 0;
}

static int
drv_priv(cr)
struct cred *cr;
{
    return (cr && cr->cr_uid == 0) ? 0 : EPERM;
}

/* spl: the level, and a hook that runs once at the next raise (a nested writer) */
static int kspl;
static void (*kspl_hook)();

static int
spl7()
{
    int s = kspl;
    void (*f)() = kspl_hook;

    kspl = 4;
    if (f) {
        kspl_hook = 0;
        (*f)();
    }
    return s;
}

static int
splx(s)
int s;
{
    int o = kspl;

    kspl = s;
    return o;
}

/* the console hook */
static int (*khook)();

static int
cons_hook_set(f)
int (*f)();
{
    if (khook)
        return EBUSY;
    khook = f;
    return 0;
}

static void
cons_hook_clear()
{
    khook = 0;
}

/* monsw */
static int kmonsw_state = -1, kmonsw_regs, kmonsw_lastcloses;
static void (*kmonsw_hook)();

static void
monsw_set(rtg)
int rtg;
{
    void (*f)() = kmonsw_hook;

    kmonsw_state = rtg;
    if (f) {
        kmonsw_hook = 0;
        (*f)();
    }
}

static int
monsw_register(name, seed)
char *name;
int seed;
{
    return kmonsw_regs++;
}

static void
monsw_unregister(h)
int h;
{
    --kmonsw_regs;
}

static void
monsw_lastclose(h)
int h;
{
    ++kmonsw_lastcloses;
}

/* autocon: a table the test sets */
struct kboard {
    unsigned long pc;
    long base;
    long size;
};
static struct kboard kboards[4];

static int
autocon(pc, unit, basep, sizep)
unsigned long pc;
int unit;
long *basep, *sizep;
{
    int i;

    for (i = 0; i < 4; ++i)
        if (kboards[i].pc && kboards[i].pc == pc && unit == 0) {
            *basep = kboards[i].base;
            *sizep = kboards[i].size;
            return 1;
        }
    return 0;
}

/* printf: kept in a buffer, shown with -v */
static char klog[65536];
static int klogn, kverbose;

static int
kprintf(const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(klog + klogn, sizeof klog - klogn, fmt, ap);
    va_end(ap);
    if (kverbose)
        fputs(klog + klogn, stdout);
    if (n > 0 && klogn + n < (int)sizeof klog - 256)
        klogn += n;
    else
        klogn = 0;
    return n;
}

/*
 * The two ways a module maps a window above the identity map.  "Physical" 0x40000000 up
 * is the host memory at KWIN (zz9k_host.c maps it), so a mapped board is the same fake
 * board.  kmap_limit refuses larger framebuffer maps, as a kernel short of room would.
 */
#define KWIN        0x10000000UL
#define KPHYS       0x40000000UL
#ifndef NBPP
#define NBPP        2048
#endif
#ifndef PG_V
#define PG_V        0x00000001
#endif
static unsigned long kmap_limit = 0xffffffffUL;
static int kmap_calls, kunmap_calls, kmap_regsfail, kmap_lastcm, kmap_lastflags;
static unsigned long kmapped;           /* bytes mapped now */

static char *
kmap_phys(phys, len)
unsigned long phys, len;
{
    if (phys < KPHYS || len > kmap_limit)
        return (char *)0;
    if (len <= 0x2000 && kmap_regsfail)
        return (char *)0;
    kmapped += len;
    return (char *)(KWIN + (phys - KPHYS));
}

static char *
dev_kvmap(phys, len, cm, flags)
unsigned long phys, len;
int cm, flags;
{
    ++kmap_calls;
    kmap_lastcm = cm;
    kmap_lastflags = flags;
    return kmap_phys(phys, len);
}

static void
dev_kvunmap(kva, len)
char *kva;
unsigned long len;
{
    ++kunmap_calls;
    kmapped -= len;
}

static char *
sptalloc(npages, mode, pfn, flag)
int npages, mode, pfn, flag;
{
    ++kmap_calls;
    return kmap_phys((unsigned long)pfn << 12, (unsigned long)npages * NBPP);
}

static void
sptfree(kva, npages, flag)
char *kva;
int npages, flag;
{
    ++kunmap_calls;
    kmapped -= (unsigned long)npages * NBPP;
}
