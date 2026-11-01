/*
 * xztop.c - the Z3660's temperatures, voltages and CPU clock in a small X
 * window, in the manner of xload: one Athena StripChart per measurement,
 * from the RTG driver's ZZ9KIOC_SENSORS.  See xztop(1).
 *
 * Copyright (c) 2026 Jussi Alanärä.  SPDX-License-Identifier: MIT
 *
 * Every chart asks for its value at the same rate; the first to ask in a
 * new second reads the driver once and the others use that reading, so the
 * board is read at most once a second whatever the number of charts.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <X11/Shell.h>
#include <X11/Xaw/Paned.h>
#include <X11/Xaw/Label.h>
#include <X11/Xaw/StripChart.h>
#include "zz9000.h"

#define INFODEV "/dev/zz9000i"
#define MAINDEV "/dev/zz9000"

/*
 * The charts, top to bottom.  A chart's value is the measurement / 1000:
 * tens of degrees for a temperature (0.01 C units), volts for a voltage (mV),
 * so the StripChart's scale lines fall every 10 C or every volt.
 */
struct row {
    int id;
    char *name;         /* the widget name, for resources */
    char *label;
    int temp;           /* 1: a temperature, 0: a voltage */
    int minscale;       /* the chart's lowest top line, in chart units */
    Widget lw;
    char text[32];
};

static struct row rows[] = {
    { ZZ9K_SENS_CPU_TEMP, "cpu", "CPU", 1, 6 },
    { ZZ9K_SENS_FPGA_TEMP, "fpga", "FPGA", 1, 6 },
    { ZZ9K_SENS_BOARD_TEMP, "board", "Board", 1, 6 },
    { ZZ9K_SENS_V5, "v5", "5V", 0, 6 },
    { ZZ9K_SENS_V3V3, "v3v3", "3.3V", 0, 4 },
    { ZZ9K_SENS_VCORE, "vcore", "Core", 0, 2 }
};
#define NROWS ((int)(sizeof rows / sizeof rows[0]))

static int fd = -1;
static struct zz9k_sensors cur;
static time_t curtime;

struct appres {
    int update;
    String device;
};
static struct appres res;

static XtResource resources[] = {
    { "update", "Interval", XtRInt, sizeof(int),
      XtOffsetOf(struct appres, update), XtRImmediate, (XtPointer)2 },
    { "device", "Device", XtRString, sizeof(String),
      XtOffsetOf(struct appres, device), XtRString, (XtPointer)0 }
};

static XrmOptionDescRec options[] = {
    { "-update", "*update", XrmoptionSepArg, NULL },
    { "-device", "*device", XrmoptionSepArg, NULL }
};

static String fallback[] = {
    "XZtop*Label.borderWidth: 0",
    "XZtop*Label.justify: left",
    "XZtop*StripChart.width: 140",
    "XZtop*StripChart.height: 32",
    "XZtop*showGrip: false",
    NULL
};

/* one reading per second at most, shared by every chart */
static void
refresh(void)
{
    time_t now;

    now = time((time_t *)0);
    if (now == curtime)
        return;
    curtime = now;
    memset((char *)&cur, 0, sizeof cur);
    cur.size = sizeof cur;
    if (ioctl(fd, ZZ9KIOC_SENSORS, &cur) < 0)
        cur.valid = 0;
}

static void
settext(struct row *r)
{
    char buf[32];
    long v;

    v = cur.value[r->id];
    if (!(cur.valid & (1UL << r->id)))
        sprintf(buf, "%-6s  -", r->label);
    else if (r->temp)
        sprintf(buf, "%-6s%3ld.%ld C", r->label, v / 100, (v % 100) / 10);
    else
        sprintf(buf, "%-6s%2ld.%02ld V", r->label, v / 1000, (v % 1000) / 10);
    if (strcmp(buf, r->text) != 0) {
        strcpy(r->text, buf);
        XtVaSetValues(r->lw, XtNlabel, r->text, NULL);
    }
}

/* the StripChart's getValue callback: client_data is the row */
static void
getvalue(Widget w, XtPointer client, XtPointer call)
{
    struct row *r;

    r = (struct row *)client;
    refresh();
    settext(r);
    if (cur.valid & (1UL << r->id))
        *(double *)call = (double)cur.value[r->id] / 1000.0;
    else
        *(double *)call = 0.0;
}

int
main(int argc, char **argv)
{
    XtAppContext app;
    Widget top, pane, w;
    char title[64];
    int i, shown;

    top = XtVaAppInitialize(&app, "XZtop", options, XtNumber(options),
        &argc, argv, fallback, NULL);
    if (argc != 1) {
        fprintf(stderr, "usage: xztop [-update seconds] [-device file] [X toolkit options]\n");
        return 2;
    }
    XtGetApplicationResources(top, (XtPointer)&res, resources, XtNumber(resources),
        NULL, 0);
    if (res.update < 1)
        res.update = 1;
    if (res.update > 60)
        res.update = 60;

    fd = open(res.device ? res.device : INFODEV, O_RDONLY);
    if (fd < 0 && !res.device)
        fd = open(MAINDEV, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "xztop: %s: %s\n", res.device ? res.device : INFODEV,
            strerror(errno));
        return 1;
    }
    refresh();
    if (cur.count == 0) {
        fprintf(stderr, "xztop: no sensors on this board\n");
        return 3;
    }

    pane = XtVaCreateManagedWidget("pane", panedWidgetClass, top, NULL);
    if (cur.valid & (1UL << ZZ9K_SENS_CPU_MHZ)) {
        sprintf(title, "Z3660  %ld MHz", cur.value[ZZ9K_SENS_CPU_MHZ]);
        XtVaCreateManagedWidget("clock", labelWidgetClass, pane, XtNlabel, title, NULL);
    }
    for (i = 0, shown = 0; i < NROWS; ++i) {
        if (!(cur.valid & (1UL << rows[i].id)))
            continue;               /* the firmware does not offer it */
        rows[i].lw = XtVaCreateManagedWidget(rows[i].name, labelWidgetClass, pane,
            XtNlabel, "", NULL);
        settext(&rows[i]);
        w = XtVaCreateManagedWidget(rows[i].name, stripChartWidgetClass, pane,
            XtNupdate, res.update, XtNminScale, rows[i].minscale, NULL);
        XtAddCallback(w, XtNgetValue, getvalue, (XtPointer)&rows[i]);
        ++shown;
    }
    if (!shown) {
        fprintf(stderr, "xztop: no temperature or voltage on this board\n");
        return 3;
    }
    XtRealizeWidget(top);
    XtAppMainLoop(app);
    return 0;
}
