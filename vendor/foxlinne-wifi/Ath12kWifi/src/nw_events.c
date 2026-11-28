/* Board event delivery is independent of the wireless protocol implementation. */
#include "nw_events.h"
struct nw_wifi_events nw_events;
void nw_wifi_set_events(const struct nw_wifi_events *events) { nw_events = *events; }
