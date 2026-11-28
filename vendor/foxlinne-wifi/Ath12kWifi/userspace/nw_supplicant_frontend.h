/* Settings requests enter the original supplicant control interface. */
#ifndef NW_SUPPLICANT_FRONTEND_H
#define NW_SUPPLICANT_FRONTEND_H
#include "nw_control.h"
int nw_supplicant_request(void *context, const struct nw_control_view *event);
void nw_supplicant_auth_failed(void *context, const char *reason);
#endif
