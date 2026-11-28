/* Userspace kern-control socket ABI from the selected XNU bsd/sys headers.
 * iPhoneOS SDK omits these declarations; no kernel headers enter this build. */
#ifndef NW_DARWIN_CONTROL_H
#define NW_DARWIN_CONTROL_H
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#define NW_SYSPROTO_CONTROL 2
#define NW_AF_SYS_CONTROL 2
struct nw_ctl_info { uint32_t id; char name[96]; };
struct nw_sockaddr_ctl {
  uint8_t length, family;
  uint16_t sysaddr;
  uint32_t id, unit, reserved[5];
};
#define NW_CTLIOCGINFO _IOWR('N', 3, struct nw_ctl_info)
_Static_assert(sizeof(struct nw_ctl_info) == 100, "kern-control info ABI");
_Static_assert(sizeof(struct nw_sockaddr_ctl) == 32, "kern-control address ABI");
#endif
