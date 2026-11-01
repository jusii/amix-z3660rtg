/* host stub of the kit's <sys/moddefs.h>: the row the driver fills, no wrapper */
struct mod_drv_data {
    int drv_bdevsw[1];
    int drv_bmajor, drv_bcount;
    int (*drv_cdevsw[6])();
    int drv_cmajor, drv_ccount;
};
#define MOD_DRV_WRAPPER(p, load, unload, halt, desc)
