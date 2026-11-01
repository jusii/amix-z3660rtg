# /dev/zz9000 ioctl ABI: the 1.1 changes

The ioctl numbers, structures and meanings of `zz9000.h` stay as they were. The ZZ9000 build
answers the same calls as before. These are the 1.1 additions and tightenings, for anyone who
calls the driver: the X server (`Xzz9000`, the x11r6.3 package), the tools here, and scripts.

## Who may call what

| Call | Needs |
|---|---|
| `GETINFO`, `GETFBINFO`, `GETAUTOCON`, `SVGAIOCGetMonitorSwitch` | any open descriptor |
| `SETMODE`, `SETPAN`, `FILL`, `FILLRECT`, `COPYRECT`, `CONPUTS`, `BLITS`, `SVGAIOCSetMonitorSwitch` | a descriptor open for writing, else `EBADF` |
| `SETREG`, `CONSOLE`, `SETSWITCH` | open for writing (`EBADF`) and privilege (`EPERM`) |
| `GETREG`, `PROBE` | privilege (`EPERM`) |
| `read`/`write` of the register block (device offsets below `fb_offset`; ZZ9000 rows only) | privilege (`EPERM`) |

The node is `0660 root:sys` (`mkdev.sh`). The X server runs as root and opens the node
read-write, so it is not affected.

## GETREG and SETREG reach only the RTG registers

Each board row lists the registers GETREG/SETREG may reach. Anything else gives `EINVAL`:

- **Z3660:** readable and writable are MODE `0x100`, `0x108`-`0x13B` (sprite position, the
  blitter coordinates, PAN, ROW_PITCH, RGB, the FILLRECT/COPYRECT triggers) and
  `0x140`-`0x14F` (SRC, DST, COLORMODE, SRC_PITCH), plus SPLIT_POS `0x188`. Read-only are VBLANK
  `0x17C`, FW_BETA/FW_ALFA `0x280`-`0x287` and the monitor switch `0x318`. Always refused:
  ZZ_CONFIG `0x104` and `0x190`-`0x1AF`, the ethernet mailbox, which another driver owns.
  FW_VERSION `0x1A0` lies in that range; `GETINFO` reports it.
- **ZZ9000:** readable and writable are MODE, PAN HI/LO, the blitter block `0x10`-`0x25` and
  `0x28`-`0x33`, and SPLIT_POS. Read-only are HW_VERSION, VBLANK and FW_VERSION. CONFIG
  `0x04`, the ethernet and USB block and the capture block are refused.

A SETREG also empties the driver's blitter-register cache.

## BLITS: a partial count, and an error means nothing ran

`ZZ9KIOC_BLITS` returns `r >= 0`. Records `[0, r)` have run and `[r, n)` have not. Before
1.1, a successful call always returned `n`, and a bad record after the first gave `EINVAL`
after the records before it had run. The caller could not tell how many had run.

A call now ends early, with `r < n`, when:

- **its pixel budget is spent.** `ZZ9K_BLITS_PIXELS` is 4 Mpx (about 80 ms of blitter),
  counted over whole records. The first record always runs, so every call makes progress.
- **record `r` is bad:** off the screen, or neither FILL nor COPY. The next call, which starts
  at that record, then gets `EINVAL`.
- **record `r` cannot be read.**

An error return means no record ran:

| errno | Meaning | What the caller does |
|---|---|---|
| `ENXIO` | no blitter for this board or mode (or `blit=0`) | draw `[r, n)` on the CPU, stop using BLITS |
| `EINVAL` | record 0 of this call is bad, or `n > ZZ9K_BLITS_MAX` | draw or drop that one record, carry on from the next |
| `EFAULT` | the header or record 0 cannot be read | a caller bug |
| `EBADF` | the descriptor is read-only | open it read-write |

### What the X server's blitFlush should do

rt07's `blitFlush` treats any error as a whole-batch refusal: it redraws every queued record
on the CPU and turns the blitter off for the life of the server. A copy that already ran is
then applied twice. With 1.1 it should loop:

```c
for (i = 0; i < n; ) {
    bs.recs = blitQ + i;
    bs.n = n - i;
    r = ioctl(fd, ZZ9KIOC_BLITS, &bs);
    if (r > 0) {
        i += r;                 /* [i, i + r) ran; send the rest */
    } else if (r < 0 && errno == EINVAL) {
        cpuDraw(&blitQ[i]);     /* this one record, on the CPU */
        i++;
    } else {                    /* ENXIO (or anything else): none of [i, n) ran */
        cpuDrawAll(blitQ + i, n - i);
        blitOff();
        break;
    }
}
```

With the server's defaults (`-blitmax 8192`, a 64-record queue) one batch is at most
512 Kpx. That is far below the budget, so a 1.0 server sees `r < n` only for a bad record,
which a correct server never queues. A server run with `-blitmax 0` can queue more than the
budget. Under 1.1 it needs the loop above.

The kernel now bands every operation itself (the `maxarea` parameter, default 8192 px; see
the next section). The server's `-blitmax` banding is no longer needed for interrupt latency.

## The band default

The static driver's `zz9k_blit_maxarea` and the module's `maxarea` parameter now default to
8192 px. `0` also means the default. A value of a screen's area or more (up to the
parameter's limit, 3686400) never splits.

## FILL: at most 1 MiB per call, and the count

`ZZ9KIOC_FILL` once filled up to the whole framebuffer (46 MB on the Z3660) byte by byte, in
one kernel loop. It now fills at most `ZZ9K_FILL_MAX` (1 MiB) per call, with 32-bit stores,
and returns the number of bytes it filled (before 1.1 it returned 0). A caller with more to
fill calls again from `offset + r`.

The X server clears the screen once at start with one FILL and ignores the result. It now
clears the first 1 MiB. Its first root-window paint covers the rest, so nothing visible
changes. To clear all of it, it would loop on the count.

## The console steps aside for a client's mode

With the console redirected to the board, a `SETMODE` from a client (the X server, `zz9k_mode`)
pauses the console. The client owns the screen and its stride. Console output goes to the
native screen meanwhile, and the monitor switch is left alone. `CONPUTS` gives `EBUSY` while
the console is paused. The last close of `/dev/zz9000` resumes the console: its mode, a
cleared screen, the monitor on the RTG. Before 1.1 the console kept drawing at its own stride
into the client's framebuffer and switched the monitor on every write.
