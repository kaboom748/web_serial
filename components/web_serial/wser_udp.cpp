// wser_udp.cpp -- see wser_udp.h. Compiled for real on the raw-lwip target
// and under WSER_HOST_TEST against the instrumented stub; empty elsewhere.
// Build guard: toolchain macros only (ESP8266/ARDUINO_ARCH_ESP8266 come
// from PlatformIO itself) plus the module-owned WSER_TARGET_LWIP any build
// system can set. NEVER an ESPHome macro here: this file includes no
// ESPHome header, so USE_SOCKET_IMPL_LWIP_TCP is invisible in this TU --
// guarding on it compiled the module EMPTY on the device (field-found as
// undefined references at link).
#if defined(WSER_HOST_TEST) || defined(WSER_TARGET_LWIP) || defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266)

#include "wser_udp.h"

namespace wser {

bool WserUdpSocket::bind(uint16_t local_port) {
  if (this->pcb_ != nullptr) {
    errno = EINVAL;  // already bound; close() first
    return false;
  }
  WSER_LWIP_LOCK();
  this->pcb_ = udp_new();
  if (this->pcb_ == nullptr) {
    WSER_LWIP_UNLOCK();
    errno = ENOMEM;
    return false;
  }
  if (udp_bind(this->pcb_, IP_ADDR_ANY, local_port) != ERR_OK) {
    udp_remove(this->pcb_);
    this->pcb_ = nullptr;
    WSER_LWIP_UNLOCK();
    errno = EADDRINUSE;
    return false;
  }
  udp_recv(this->pcb_, &WserUdpSocket::recv_cb_, this);
  WSER_LWIP_UNLOCK();
  this->rd_ = this->wr_ = this->n_ = 0;
  return true;
}

void WserUdpSocket::recv_cb_(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                             const ip_addr_t *addr, u16_t port) {
  (void) pcb;
  auto *self = static_cast<WserUdpSocket *>(arg);
  if (p == nullptr)
    return;
  if (self->n_ >= WSER_UDP_RING) {
    self->rx_dropped_++;          // ring full: normal UDP loss, counted
    pbuf_free(p);
    return;
  }
  Dgram &d = self->ring_[self->wr_];
  uint16_t take = p->tot_len;
  if (take > WSER_UDP_DGRAM_MAX) {
    self->rx_dropped_++;          // truncation is loss too: make it visible
    take = WSER_UDP_DGRAM_MAX;
  }
  pbuf_copy_partial(p, d.data, take, 0);
  d.len = take;
  d.src_ip_be = wser_ip4_get(addr);
  d.src_port = port;
  self->wr_ = (uint8_t) ((self->wr_ + 1) % WSER_UDP_RING);
  self->n_++;
  pbuf_free(p);                   // ALWAYS ours to free -- the lwip contract
}

long WserUdpSocket::recvfrom(void *buf, size_t len, wser_endpoint *from) {
  // pure ring read: NO lwip call, hence no lock -- callable at any rate
  if (this->pcb_ == nullptr) {
    errno = EBADF;
    return -1;
  }
  if (this->n_ == 0) {
    errno = EWOULDBLOCK;
    return -1;
  }
  Dgram &d = this->ring_[this->rd_];
  size_t k = d.len < len ? d.len : len;   // datagram semantics: excess is cut
  memcpy(buf, d.data, k);
  if (from != nullptr)
    *from = wser_endpoint::from_u32_lwip(d.src_ip_be, d.src_port);
  this->rd_ = (uint8_t) ((this->rd_ + 1) % WSER_UDP_RING);
  this->n_--;
  return (long) k;
}

long WserUdpSocket::sendto(const void *buf, size_t len, const wser_endpoint &to) {
  if (this->pcb_ == nullptr) {
    errno = EBADF;
    return -1;
  }
  if (to.is_zero()) {
    errno = EINVAL;
    return -1;
  }
  if (len > 65507) {
    // the (u16_t) cast below would have SILENTLY truncated a 70000-byte
    // send to 4464 while reporting 70000 sent -- the worst kind of lie.
    errno = EMSGSIZE;
    return -1;
  }
  ip_addr_t dst;
  wser_ip4_set(&dst, to.to_u32_lwip());
  WSER_LWIP_LOCK();
  struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t) len, PBUF_RAM);
  if (p == nullptr) {
    WSER_LWIP_UNLOCK();
    errno = ENOMEM;
    return -1;
  }
  pbuf_take(p, buf, (u16_t) len);
  err_t e = udp_sendto(this->pcb_, p, &dst, to.port);
  pbuf_free(p);                   // udp_sendto does NOT take ownership
  WSER_LWIP_UNLOCK();
  if (e != ERR_OK) {
    errno = (e == ERR_MEM) ? ENOMEM : EIO;
    return -1;
  }
  return (long) len;
}

void WserUdpSocket::close() {
  if (this->pcb_ == nullptr)
    return;
  WSER_LWIP_LOCK();
  udp_recv(this->pcb_, nullptr, nullptr);   // disarm BEFORE removal
  udp_remove(this->pcb_);
  WSER_LWIP_UNLOCK();
  this->pcb_ = nullptr;
  this->rd_ = this->wr_ = this->n_ = 0;
}

}  // namespace wser

#endif  // WSER_HOST_TEST || USE_SOCKET_IMPL_LWIP_TCP
