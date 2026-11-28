/* WEXT is disabled; only declaration tags from its headers are needed. */
#ifndef LX_WIRELESS_H
#define LX_WIRELESS_H
#ifdef CONFIG_CFG80211_WEXT
#error The XNU backend uses the native control interface, not Wireless Extensions.
#endif
struct iw_freq;
struct iw_point;
struct iw_param;
struct iw_request_info;
struct iw_handler_def;
union iwreq_data;
#endif
