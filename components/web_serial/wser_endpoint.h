#pragma once
// wser_endpoint -- THE boundary type of the wser transports. The whole
// hardening strategy hangs on this struct: the module's public surface
// speaks only wser_endpoint (plain bytes, zero includes), so the module
// depends on NOTHING but lwip internally. Host code (ESPHome, Arduino,
// anything) converts to/from its own address types in ITS world, at the
// call site -- never inside the module.
//
// Convention: ip[4] = a.b.c.d as written (network order), port = HOST
// byte order. POD, comparable, 6 bytes.

#include <cstdint>

namespace wser {

struct wser_endpoint {
  uint8_t ip[4]{0, 0, 0, 0};
  uint16_t port{0};

  bool operator==(const wser_endpoint &o) const {
    return ip[0] == o.ip[0] && ip[1] == o.ip[1] && ip[2] == o.ip[2] && ip[3] == o.ip[3] && port == o.port;
  }
  bool operator!=(const wser_endpoint &o) const { return !(*this == o); }
  bool is_zero() const { return ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0 && port == 0; }

  // lwip carries IPv4 as a u32 in NETWORK byte order (a in the low byte on
  // little-endian wire representation): a.b.c.d -> 0xDDCCBBAA as lwip's
  // ip_addr u32. These two are the ONLY conversions the module ever does.
  uint32_t to_u32_lwip() const {
    return (uint32_t) ip[0] | ((uint32_t) ip[1] << 8) | ((uint32_t) ip[2] << 16) | ((uint32_t) ip[3] << 24);
  }
  static wser_endpoint from_u32_lwip(uint32_t u, uint16_t port_host) {
    wser_endpoint e;
    e.ip[0] = (uint8_t) (u & 0xFF);
    e.ip[1] = (uint8_t) ((u >> 8) & 0xFF);
    e.ip[2] = (uint8_t) ((u >> 16) & 0xFF);
    e.ip[3] = (uint8_t) ((u >> 24) & 0xFF);
    e.port = port_host;
    return e;
  }
};

inline wser_endpoint make_endpoint(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
  wser_endpoint e;
  e.ip[0] = a;
  e.ip[1] = b;
  e.ip[2] = c;
  e.ip[3] = d;
  e.port = port;
  return e;
}

}  // namespace wser
