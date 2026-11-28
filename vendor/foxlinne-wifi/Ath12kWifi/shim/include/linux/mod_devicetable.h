#ifndef LX_MODDEV_H
#define LX_MODDEV_H
#include "lx_os.h"
#define MHI_NAME_SIZE 32
#define MHI_DEVICE_MODALIAS_FMT "mhi:%s"
typedef unsigned long kernel_ulong_t;
struct mhi_device_id { const char chan[MHI_NAME_SIZE]; kernel_ulong_t driver_data; };
#endif
