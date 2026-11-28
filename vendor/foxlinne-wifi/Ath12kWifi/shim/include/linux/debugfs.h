#include "lx_os.h"
/* Debugfs is not selected on XNU; this matches the disabled Linux interface. */
#ifdef CONFIG_DEBUG_FS
#error A native debugfs provider is not implemented.
#endif
#define debugfs_change_name(...) (-ENODEV)
#define debugfs_create_dir(...) ((struct dentry *)ERR_PTR(-ENODEV))
#define debugfs_remove_recursive(entry) ((void)(entry))
#define debugfs_remove(entry) ((void)(entry))
