/* What the platform below the Linux-derived code must provide. */
#ifndef LX_PLAT_H
#define LX_PLAT_H
#include "lx_os.h"
#include "nw_abi.h"
extern const struct lx_platform *lx_plat;
void lx_root_init(void);
#endif
