/* With CONTROL_PORT_RX, wpa_supplicant needs only this address handle.
 * EAPOL send/receive goes through driver_sheng, with key-operation ordering. */
#include "utils/includes.h"
#include "utils/common.h"
#include "l2_packet.h"
struct l2_packet_data { u8 address[ETH_ALEN]; };
struct l2_packet_data *l2_packet_init(const char *name, const u8 *address,
  unsigned short protocol, void (*receive)(void *, const u8 *, const u8 *, size_t),
  void *context, int with_header)
{
  (void)name; (void)context; (void)with_header;
  if (!address || receive || protocol != ETH_P_EAPOL) return NULL;
  struct l2_packet_data *handle = os_zalloc(sizeof(*handle));
  if (handle) os_memcpy(handle->address, address, ETH_ALEN);
  return handle;
}
struct l2_packet_data *l2_packet_init_bridge(const char *bridge, const char *name,
  const u8 *address, unsigned short protocol,
  void (*receive)(void *, const u8 *, const u8 *, size_t), void *context, int header)
{
  (void)bridge; (void)name; (void)address; (void)protocol;
  (void)receive; (void)context; (void)header;
  return NULL;
}
int l2_packet_get_own_addr(struct l2_packet_data *handle, u8 *address)
{
  if (!handle) return -1;
  os_memcpy(address, handle->address, ETH_ALEN); return 0;
}
int l2_packet_send(struct l2_packet_data *handle, const u8 *destination,
                   u16 protocol, const u8 *payload, size_t length)
{
  (void)handle; (void)destination; (void)protocol; (void)payload; (void)length;
  errno = EOPNOTSUPP; return -1;
}
void l2_packet_deinit(struct l2_packet_data *handle) { os_free(handle); }
int l2_packet_get_ip_addr(struct l2_packet_data *handle, char *buffer, size_t size)
{
  (void)handle; (void)buffer; (void)size; return -1;
}
void l2_packet_notify_auth_start(struct l2_packet_data *handle) { (void)handle; }
int l2_packet_set_packet_filter(struct l2_packet_data *handle, enum l2_packet_filter_type type)
{
  (void)handle; (void)type; return -1;
}
