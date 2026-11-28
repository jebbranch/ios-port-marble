/* The MHI host registers its bus from a module initcall; with no module loader the platform calls this once. */
#include "lx_os.h"
extern const struct bus_type mhi_bus_type;
int mhi_init_hook(void) { return bus_register(&mhi_bus_type); }
