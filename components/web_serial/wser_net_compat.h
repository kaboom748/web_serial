#pragma once
// wser_net_compat.h v2 -- the module's ONLY window on the world.
// After the hardening pass the rule is absolute: the transports depend on
// lwip and libc, NOTHING else. No ESPHome header, no <sys/socket.h> on
// target, no Arduino.h. Address types are wser_endpoint (ours).
//
// ---- Execution-model portability (the lock macros) ----
// On the ESP8266 Arduino core, lwip callbacks and user code share ONE
// cooperative context: no locking needed, macros are no-ops (default).
// On preemptive ports (ESP32 tcpip_thread, Pico W cyw43, Teensy
// QNEthernet), user-context lwip calls must be bracketed. Define both
// macros before including any wser header, e.g.:
//   ESP32:  #define WSER_LWIP_LOCK()   LOCK_TCPIP_CORE()
//           #define WSER_LWIP_UNLOCK() UNLOCK_TCPIP_CORE()
//   PicoW:  cyw43_arch_lwip_begin() / cyw43_arch_lwip_end()
// The module brackets every user-context lwip call; callbacks (already
// inside lwip) are NEVER bracketed -- doing so would deadlock.

#include <cstdint>
#include <cstring>
#include <cerrno>
#include "wser_endpoint.h"

#ifndef WSER_LWIP_LOCK
#define WSER_LWIP_LOCK()
#define WSER_LWIP_UNLOCK()
#endif

#ifdef WSER_HOST_TEST
#include "stub_lwip.h"
// Address conversion shims -- the ONLY raw ip_addr access in the module.
// (Field-found lesson: the stub once invented ip_addr_get_u32/set_u32,
// macros REAL lwip does not have; the module learned a fake API and died
// at target compile. The stub may only mirror upstream names.)
static inline uint32_t wser_ip4_get(const ip_addr_t *a) { return a->addr; }
static inline void wser_ip4_set(ip_addr_t *a, uint32_t u) { a->addr = u; }
#else
#include <lwip/ip_addr.h>
#include <lwip/pbuf.h>
#include <lwip/tcp.h>
#include <lwip/udp.h>
// Address conversion shims -- the ONLY raw ip_addr access in the module.
// IPv4-only lwip (ESP8266 default): ip_addr_t IS ip4_addr_t. Dual-stack:
// go through ip_2_ip4 / the v4 setter. Deterministic, no reliance on
// compat macros that vary across lwip configs.
#if defined(LWIP_IPV6) && LWIP_IPV6
static inline uint32_t wser_ip4_get(const ip_addr_t *a) { return ip4_addr_get_u32(ip_2_ip4(a)); }
static inline void wser_ip4_set(ip_addr_t *a, uint32_t u) {
  ip4_addr_set_u32(ip_2_ip4(a), u);
  IP_SET_TYPE(a, IPADDR_TYPE_V4);
}
#else
static inline uint32_t wser_ip4_get(const ip_addr_t *a) { return ip4_addr_get_u32(a); }
static inline void wser_ip4_set(ip_addr_t *a, uint32_t u) { ip4_addr_set_u32(a, u); }
#endif
#endif

// ---- Compile-time canaries (the loud-failure doctrine) ----
// If a future lwip/SDK release changes a value or shape the module RELIES
// on, the build stops HERE with a named contract -- never a silently
// wrong binary. Values below are lwip's, stable for 15+ years; if one of
// these fires, read lwip's err.h diff before touching the module.
static_assert(ERR_OK == 0, "wser canary: lwip ERR_OK must be 0 (success tests use it)");
static_assert(ERR_MEM != ERR_OK, "wser canary: ERR_MEM must be a failure code");
static_assert(ERR_ABRT != ERR_OK, "wser canary: ERR_ABRT must be a failure code (abort contract)");
static_assert(ERR_ABRT != ERR_MEM, "wser canary: ERR_ABRT and ERR_MEM must be distinct (recv cb: refuse vs abort)");
static_assert(sizeof(u16_t) == 2, "wser canary: lwip u16_t shape changed");
static_assert(sizeof(((struct pbuf *) nullptr)->tot_len) == 2, "wser canary: pbuf.tot_len is u16 (ring math relies on it)");
static_assert(sizeof(wser::wser_endpoint) == 6, "wser canary: endpoint must stay a 6-byte POD (persisted by hosts)");
#ifdef WSER_UDP_RING
static_assert(WSER_UDP_RING >= 1 && WSER_UDP_RING <= 255, "wser canary: udp ring must fit its uint8_t indices");
#endif
