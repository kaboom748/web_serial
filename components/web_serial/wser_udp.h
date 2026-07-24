#pragma once
// wser_udp: a raw-lwip UDP socket for platforms whose ESPHome socket impl
// rejects SOCK_DGRAM (ESP8266's lwip_raw_tcp). Exposes the exact surface
// web_serial's port_net_loop_ already consumes from a BSD UDP socket --
// bind / recvfrom / sendto / close, all non-blocking -- so the hub swaps
// implementations at ONE creation site and stays blind to the difference.
//
// Multi-instance by construction: each object owns its own udp_pcb bound
// to its own local port; lwip demuxes, udp_recv carries `this` as arg.
//
// RX path: the lwip callback (no preemption on ESP8266: same context as
// the loop) copies each datagram into a small per-socket ring; recvfrom()
// serves from the ring. A full ring drops the datagram -- normal UDP
// semantics -- and counts it. Oversized datagrams are truncated to the
// slot size (WSER_UDP_DGRAM_MAX), also counted.
//
// v2 (hardened): public surface speaks wser_endpoint ONLY -- no sockaddr,
// no platform headers. IPv4 only by design.

#include "wser_net_compat.h"

#ifndef WSER_UDP_RING
#define WSER_UDP_RING 3          // datagrams buffered per socket
#endif
#ifndef WSER_UDP_DGRAM_MAX
#define WSER_UDP_DGRAM_MAX 128   // matches the hub's UDP read buffer
#endif

namespace wser {

// PIEGES P3 (ESP8266 cont stack ~4 KB): this object is ~440 B -- NEVER
// stack-allocate it. Hold it as a long-lived member (the hub's Port), a
// static, or a short-lived heap allocation. A deep-path stack instance
// crashes far from the culprit.
class WserUdpSocket {
 public:
  WserUdpSocket() = default;
  ~WserUdpSocket() { this->close(); }
  WserUdpSocket(const WserUdpSocket &) = delete;
  WserUdpSocket &operator=(const WserUdpSocket &) = delete;

  // Bind the local port (0.0.0.0). false on failure (errno set).
  bool bind(uint16_t local_port);

  // Non-blocking. >0 = datagram length copied (truncated to len if needed),
  // -1/EWOULDBLOCK when the ring is empty, -1/EBADF when not bound.
  // Fills `from` (source endpoint) when provided.
  long recvfrom(void *buf, size_t len, wser_endpoint *from);

  // Non-blocking. len on success; -1 with errno (EBADF, EINVAL on zero
  // endpoint, ENOMEM on pbuf starvation, EIO on stack refusal).
  long sendto(const void *buf, size_t len, const wser_endpoint &to);

  void close();

  bool bound() const { return this->pcb_ != nullptr; }
  // the port actually bound -- ESSENTIAL with bind(0) (stack-assigned)
  // datagrams currently waiting in the ring (P2 buffer-fill telemetry)
  uint8_t pending() const { return this->n_; }
  uint16_t local_port() const { return this->pcb_ ? this->pcb_->local_port : 0; }
  uint32_t rx_dropped() const { return this->rx_dropped_; }   // ring-full + truncations

 private:
  static void recv_cb_(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port);

  struct Dgram {
    uint16_t len;
    uint32_t src_ip_be;   // network byte order, as lwip carries it
    uint16_t src_port;    // host order (lwip callback convention)
    uint8_t data[WSER_UDP_DGRAM_MAX];
  };
  struct udp_pcb *pcb_{nullptr};
  Dgram ring_[WSER_UDP_RING];
  uint8_t rd_{0}, wr_{0}, n_{0};
  uint32_t rx_dropped_{0};
};

}  // namespace wser
