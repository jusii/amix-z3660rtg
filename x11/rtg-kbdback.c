/*
 * rtg-kbdback.c -- give the keyboard back to the console after an RTG X server that died
 * without cleaning up.
 *
 * The RTG X server selects its own screen-manager screen for keyboard input (SIOCACTIVATE)
 * and, with the console-keyboard patch (rt10), hands the selection back (SIOCBACK) when it ends.  A server killed with
 * SIGKILL runs none of that: the kernel closes its screen, its CloseScreen finds no
 * successor in the screen's (empty) group and leaves no active screen, and from then on
 * every key typed on the console is dropped (console/screen.c).
 *
 * This opens a screen of its own, selects it (so it is the active one), sends it to the
 * back -- HideScreen then selects the first usable other screen, the console -- and closes
 * it.  Run when no screen is active it restores the console; run while another screen is
 * active it takes the selection away from that screen for a moment and gives it to the
 * first usable screen, so startx-rtg calls it only after its own server is gone.
 *
 * usage: rtg-kbdback        exit 0 done, 1 could not open a screen, 2 the screen could not
 *                           be sent back (the keyboard may still be lost)
 * The 1.0 kit's binary (x11/KIT.sha256) was built before exit status 2.
 * Cross-built (x11/README.md); needs /usr/amiga/lib/libscreen.so on the box.
 */
#include <stdio.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <amiga/screen.h>

int
main()
{
    int fd, rc;

    rc = 0;
    fd = OpenScreen("rtg-kbdback", DEFAULT_SCRTYPE, 0);
    if (fd < 0) {
        fprintf(stderr, "rtg-kbdback: OpenScreen failed: %s\n", ScreenError());
        return 1;
    }
    if (ioctl(fd, SIOCACTIVATE, 0))
        perror("rtg-kbdback: SIOCACTIVATE");
    if (ioctl(fd, SIOCBACK, 0)) {
        perror("rtg-kbdback: SIOCBACK");
        rc = 2;
    }
    CloseScreen(fd);
    return rc;
}
