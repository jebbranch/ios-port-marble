#include "lx_net.h"
#ifndef LX_NET_NAMESPACE_H
#define LX_NET_NAMESPACE_H
struct pernet_operations { struct list_head list; int (*init)(struct net *); void (*exit)(struct net *); bool registered; };
static inline bool net_eq(const struct net *a, const struct net *b) { return a == b; }
int register_pernet_subsys(struct pernet_operations *operations);
void unregister_pernet_subsys(struct pernet_operations *operations);
#define register_pernet_device(ops) register_pernet_subsys(ops)
#define unregister_pernet_device(ops) unregister_pernet_subsys(ops)
#endif
