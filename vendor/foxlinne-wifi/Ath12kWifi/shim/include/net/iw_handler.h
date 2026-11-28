#ifndef LX_IW_HANDLER_H
#define LX_IW_HANDLER_H
#include <linux/wireless.h>
#ifdef CONFIG_WEXT_CORE
#error The native control backend does not expose Wireless Extensions.
#endif
static inline void wireless_nlevent_flush(void) { }
#endif
