#pragma once
// web_serial: an in-browser SERIAL HUB for ESP8266/ESP32. Like an Ethernet
// hub, but for serial: every byte entering one port is dispatched to all the
// others. Ports: the ESP's physical UART (QoS), the firmware's own use of the
// same uart bus (native QoS bypass), the web frontend (best-effort, text+hex,
// lag-tolerant), a raw TCP232 server (PuTTY/telnet/Eltima direct), and the
// browser's LOCAL PC serial port bridged in via the Web Serial API.
// Protocol decoders: Modbus RTU (CRC16), DMX512 (RX + experimental TX),
// NMEA 0183, AT. RS485 half-duplex via de_pin; RS422 is transparent.
// NO build-time graft: the uart debug callback is a public runtime seam.
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/hal.h"
#include "esphome/components/uart/uart_component.h"
#include "esphome/components/socket/socket.h"
#ifdef USE_SOCKET_IMPL_LWIP_TCP
// raw-lwip platforms (ESP8266): the socket layer rejects SOCK_DGRAM; the
// wser module provides UDP on the raw udp_pcb API (lwip-only dependency,
// gauntlet-tested standalone -- see components/web_serial/wser_*.{h,cpp})
#include "wser_udp.h"
#endif
#include "esphome/core/preferences.h"
#include <memory>
#include <vector>
#include <string>
#include <cstdint>

#define WSER_LOG_BACKLOG 2048
#define WSER_BUS_FRAME 512
#define WSER_RUNS 6
#define WSER_SNIP 32
#define WSER_FRAME_CAP 2048
// ---- switch limits, per platform (the memory guard enforces these) ----
#ifdef USE_ESP8266
#define WSER_HEAP_FLOOR 8192      // refuse to allocate below this LARGEST-block
#define WSER_PORT_BUF_DEF 256
#define WSER_PORT_BUF_MAX 1024
#define WSER_MAX_PORTS 8          // 4 fixed-ish + a few dynamic
#else
#define WSER_HEAP_FLOOR 20480
#define WSER_PORT_BUF_DEF 512
#define WSER_PORT_BUF_MAX 4096
#define WSER_MAX_PORTS 16
#endif
#define WSER_VLANS 8

namespace esphome {
namespace web_serial {

namespace ws {

class Sha1 {
 public:
  Sha1() { reset(); }
  void reset();
  void update(const uint8_t *data, size_t len);
  void finish(uint8_t out[20]);

 private:
  static uint32_t rol_(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }
  void process_(const uint8_t *p);
  uint32_t h_[5];
  uint64_t len_;
  uint8_t buf_[64];
  size_t buf_len_;
};

// Compute Sec-WebSocket-Accept from the client key.
std::string accept_key(const std::string &client_key);

enum Opcode : uint8_t {
  OP_CONT = 0x0,
  OP_TEXT = 0x1,
  OP_BIN = 0x2,
  OP_CLOSE = 0x8,
  OP_PING = 0x9,
  OP_PONG = 0xA,
};

// A decoded frame.
struct Frame {
  bool fin;
  uint8_t opcode;
  std::vector<uint8_t> payload;
};

// Decode ONE frame from buf. Returns the bytes consumed, or 0 if the frame is
// incomplete (wait for more data).
size_t decode_frame(const uint8_t *buf, size_t len, Frame &out);

// Encode a server->client frame (never masked).
void encode_frame(std::vector<uint8_t> &out, uint8_t opcode, const uint8_t *payload, size_t plen);

// Append ONLY a server->client frame header (FIN+opcode+length) to out; the
// caller then pushes plen payload bytes straight into out. Lets the mirror
// build a binary tile frame directly in out_ with no intermediate buffer.
void encode_frame_header(std::vector<uint8_t> &out, uint8_t opcode, size_t plen);

}  // namespace ws

struct SerRun {
  uint8_t dir;   // 0 = TX (toward the wire), 1 = RX (from the wire)
  uint8_t n;
  uint32_t total;
  uint8_t snip[WSER_SNIP];
};
struct SerFrame {
  bool open{false};
  bool self{false};   // opened by web_serial's own console/bridge write
  uint8_t nruns{0};
  uint32_t total{0};
  uint32_t us{0}, dur_us{0};
  bool byd_{false};  // closed by the delimiter (vs the silence gap)
  SerRun runs[WSER_RUNS];
};

class WebSerial : public Component {
 public:
  void set_port(uint16_t p) { this->port_ = p; }
  void set_tcp_port(uint16_t p) { this->init_tcp_port_ = p; }
  void set_uart(uart::UARTComponent *u) { this->uart_ = u; }
  void set_de_pin(GPIOPin *p) { this->de_pin_ = p; }
  void set_owner(bool o) { this->owner_ = o; }
  void set_gap_ms(uint16_t g) { this->gap_ms_ = g; }
  void set_floor(uint32_t f) { this->heap_floor_ = f; }
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const { return setup_priority::AFTER_WIFI; }
  void on_uart_byte(uart::UARTDirection dir, uint8_t b);  // the runtime seam

 protected:
  // ---- transport (proven web_spi core) ----
  void start_server_();
  void accept_client_();
  void read_client_();
  bool do_handshake_(const char *request, size_t len);
  void handle_ws_frame_(const ws::Frame &frame);
  bool flush_tx_();
  void ws_send_text_(const std::string &text);
  void ws_send_text_(const char *text);  // ALLOCQUIET: no temporary std::string
  bool ws_send_(uint8_t opcode, const uint8_t *payload, size_t len);  // true = encoded; false = a valve dropped it
  void drop_client_(const char *why);
  uint32_t free_heap_();
  void send_info_();
  void handle_command_(const std::string &cmd);

  uint16_t port_{0};
  std::unique_ptr<socket::ListenSocket> server_;
  std::unique_ptr<socket::Socket> pending_client_;
  std::unique_ptr<socket::Socket> stream_client_;
  bool server_started_{false};
  uint32_t pending_since_{0};
  char request_[1024]{};
  size_t request_len_{0};
  uint8_t nl_{0};
  bool serving_page_{false};
  size_t page_pos_{0};
  size_t page_len_{0};
  // Egress backlog caps (PIEGES: throttle, never crash). Field crash: out_
  // was the ONLY unbounded buffer in the component -- under a log storm its
  // vector DOUBLING asked the heap for 10752 B it did not have (OOM panic).
  // Soft cap: droppable traffic (logs/frames/info) is skipped and counted.
  // Hard cap: client too slow or storm too big -- drop the CLIENT (it
  // reconnects and resyncs from the next info push).
#ifdef USE_ESP8266
  static const size_t WSER_OUT_SOFT = 2048, WSER_OUT_HARD = 4096;
#else
  static const size_t WSER_OUT_SOFT = 8192, WSER_OUT_HARD = 16384;
#endif
  // Cohabitation valve floor (captain design), BOTH platforms: not an
  // egress cap -- an allocator-courtesy threshold. Effectively inert on the
  // ESP32 (largest rarely dips below 6 KB); the courtesy is 8266-scale.
  static const uint32_t WSER_COH_FLOOR = 6144;
  uint32_t ws_drop_{0};   // messages skipped by the soft cap (info JSON: wsdrop)
  // ---- CPU governor (field lesson: 3 sustained loops pushed one pass to
  // 974 ms; the WiFi keepalive starved and the AP dropped us. Every guard
  // bounded MEMORY or per-port RATE -- nothing bounded per-pass TIME).
  // The pass serves ports until the budget is spent, remembers where it
  // stopped, and resumes there next pass (round-robin fairness). Work is
  // SPREAD, not lost: buffers absorb, eviction polices, WiFi breathes.
#ifdef USE_ESP8266
  static const uint32_t WSER_LOOP_BUDGET_US = 20000;  // cooperative core: WiFi lives between passes
#else
  static const uint32_t WSER_LOOP_BUDGET_US = 30000;  // FreeRTOS WiFi task: budget is mere citizenship
#endif
  uint8_t net_cursor_{0};   // next port to serve (fairness under overload)
  // ---- duty-cycle bound (field crash 6: the per-pass budget alone was a
  // trap -- 20 ms budget on a 21 ms period = 95% CPU duty, 47 passes/s,
  // WiFi FIQ flooded, HW WDT wedged inside wDev_ProcessFiq. Bounding time
  // PER PASS without bounding OCCUPANCY just moves the starvation).
  // Token pool: refills at DUTY% of wall time, spent by port service +
  // uart drain; empty pool = skip the phase, WiFi gets the rest -- a hard
  // duty ceiling whatever the loop period.
#ifdef USE_ESP8266
  static const uint32_t WSER_DUTY_PCT = 35, WSER_POOL_CAP_US = 30000;
#else
  static const uint32_t WSER_DUTY_PCT = 70, WSER_POOL_CAP_US = 60000;
#endif
  uint32_t pool_us_{WSER_POOL_CAP_US};
  uint32_t pool_last_us_{0};
  // ---- radio heap brake (field crashes 6-8: three HWDT wedges inside
  // wDev_ProcessFiq -- the DOCUMENTED ESP8266 failure mode when lwip can't
  // allocate pbufs under packet flood. Our storm egress INVITES the flood
  // (every byte out echoes back in); duty% bounds our CPU, not the packet
  // rate. Brake: when the largest heap block drops below the radio floor,
  // LOOPY ingress (net/bridge, never the real wire) is dropped+counted --
  // the storm self-quenches, lwip keeps its pbufs, the FIQ never wedges.)
  static const uint32_t WSER_RADIO_FLOOR = 5120;   // the DEFAULT radio-brake floor
  uint32_t radio_floor_{WSER_RADIO_FLOOR};  // RADIOFLOOR <n>; 0 = brake OFF
                                            // (documented FIQ-wedge risk!) --
                                            // NOT persisted: experiments die at reboot
  uint32_t largest_cache_{0};     // sampled every 250 ms (umm walk is costly)
  uint32_t largest_cache_ms_{0};
#ifndef USE_ESP8266
  // ---- Goliath unchained (ESP32-class): the storm benchmark exposed it --
  // on FreeRTOS platforms the main loop NAPS between passes (WiFi lives in
  // its own task, no urgency), so a loop-clocked ring pumped HALF the 8266's
  // throughput despite 10x the silicon. When traffic flows, request the
  // high-frequency loop; release at idle. Duty governor still bounds us.
  // NOT on the 8266: its 16 ms nap IS the WiFi budget (wedge history).
  HighFrequencyLoopRequester high_freq_;
  bool hf_on_{false};
#endif
  // ---- heavy-phase pacer (field: the unchained 147 us loop turned the
  // duty pool into a relaxation oscillator and the uart min-quota into a
  // FIFO flood -- TX sawtoothed "gas-brake". High frequency buys LATENCY;
  // the heavy phase keeps its own metronome: served at most 1 kHz, and the
  // uart fed by ELAPSED wire-time, never a floor.)
  uint32_t phase_last_us_{0};
  uint32_t uart_drain_last_us_{0};
  // ---- traffic instruments (P3) ----
  uint32_t peak_rate_{0};       // session max of tx+rx B/s
  uint32_t eload_spent_us_{0};  // duty-pool spend inside the window
  uint32_t eload_win_ms_{0};
  uint8_t eload_pct_{0};        // engine load %, 1 s window
  uint32_t bstat_ms_{0};        // 4 Hz buffer-fill telemetry pacing
  // switched-traffic meter: EVERYTHING the fanout carries, boundary or
  // internal -- the true work of the switch (TX/RX only see the boundary)
  uint32_t switched_bytes_{0};
  uint32_t sw_last_{0};
  uint32_t switched_rate_{0};
  // BISECT-A: the AIMD adaptive governor (13) ONLY
  uint32_t drain_quota_{256};
  int8_t quota_dir_{0};
  uint8_t throttle_cause_{0};
  uint32_t throttle_events_{0};
  bool duty_starved_{false};
  // CHRONO probes: worst-case micros per suspect block since boot. The
  // umm heap walk (largest_block_) runs under an interrupt lock and its
  // cost GROWS with fragmentation -- prime suspect for the SDK-queue
  // starvation (ets_post / ets_intr_unlock HWDT).
  uint32_t max_pass_us_{0};
  uint32_t max_win_us_{0};
  uint32_t max_drain_us_{0};
  uint32_t max_walk_us_{0};
  bool walk_on_{true};   // WALK OFF = ablation of the umm heap walk
  // STACKFIX: the killer named by the stack dump was cont_check -- the 4 KB
  // CONT stack overflowed on the DEEPEST call path (ws frame -> parser ->
  // ADD -> prefs -> send_info_'s mega-string). Handlers now only RAISE A
  // FLAG; the loop sends info at depth ~zero next pass.
  bool info_pending_{false};

  // ---- UART transport audit (verdict injected at codegen; see __init__) ----
  bool uart_hw_{true};
  const char *uart_name_{"hardware"};
  int uart_tx_{-1}, uart_rx_{-1};
 public:
  void set_uart_audit(bool hw, const char *name, int tx, int rx) {
    this->uart_hw_ = hw;
    this->uart_name_ = name;
    this->uart_tx_ = tx;
    this->uart_rx_ = rx;
  }
 protected:
  std::vector<uint8_t> out_;
  size_t out_pos_{0};
  std::vector<uint8_t> ws_accum_;
  uint32_t last_info_{0};
  uint32_t last_loop_us_{0}, loop_ema_us_{0};
  uint32_t last_rate_ms_{0};

  // ================= the managed switch =================
  // A Port is any endpoint the hub forwards between. Ethernet-hub semantics:
  // a byte in on port P goes to every UP port Q!=P sharing P's VLAN.
  enum PortType : uint8_t { PT_UART = 0, PT_CONSOLE, PT_TCP, PT_UDP, PT_BRIDGE };
  struct Port {
    bool used{false};
    PortType type{PT_TCP};
    uint8_t vlan{1};
    bool up{true};
    uint16_t net_port{0};        // TCP/UDP listen port
    uint32_t rate_cap{0};
    // ---- egress shaper (P1: "out N B/s" pill). Same token-bucket family
    // as the ingress rate; refilled lazily AT DRAIN TIME (idle port = zero
    // cost). Excess stays in buf -> eviction + drop count at THIS port,
    // exactly the mismatched-speed switch behavior (19200 -> 9600 demo).
    uint32_t out_cap{0};      // 0 = unlimited
    uint32_t out_tok{0};
    uint32_t out_tok_ms{0};
    // Fractional refill accumulators (milli-tokens, remainder < 1000).
    // WHY: the old law computed add = cap*dt/1000 with integer division and
    // advanced the timestamp EVEN WHEN add truncated to 0 -- every pass
    // under 1000/cap ms threw its elapsed time away, so any rate below
    // ~1000/pass_period B/s starved to ZERO (field-observed: 'out 10'
    // killed a trunk dead instead of shaping it to 10 B/s). The remainder
    // now survives in these fields; long-run delivery equals the number
    // asked, exactly. RAM-only (Port, not SavedCfg): no ABI/magic change.
    uint32_t out_acc{0};      // egress bucket remainder
    uint32_t rate_acc{0};     // ingress bucket remainder
    // ---- TRUNK: virtual wire to another slot-less bridge (RAM null-modem).
    // Egress ENQUEUES here; a governed drain injects into the peer's
    // ingress. The 'out' shaper IS the wire's baud. Persisted (magic v5):
    // an inter-VLAN trunk that evaporates on reboot is cardboard infra.
    int8_t loop_peer{-1};
    uint32_t hbrake{0};       // heap-brake drops, SIGNED separately from drop
    uint32_t rl_last{0};      // link-rate window snapshot (tx+rx cumul)
    uint32_t rate_bps{0};     // per-link B/s over the 1 s window        // storm control: bytes/s ingress, 0 = unlimited
    // token bucket (ingress rate limit)
    uint32_t tok{0}, tok_ms{0};
    // per-port egress buffer (dynamically sized; nullptr for UART/CONSOLE)
    uint8_t *buf{nullptr};
    uint16_t buf_cap{0}, buf_n{0};
    // sockets for TCP/UDP ports
    std::unique_ptr<socket::ListenSocket> listen;
    std::unique_ptr<socket::Socket> client;
#ifdef USE_SOCKET_IMPL_LWIP_TCP
    // UDP on raw-lwip: wudp replaces `listen` entirely for PT_UDP ports.
    // unique_ptr member (heap): the object is 440 B -- never on the stack
    // (PIEGES P3, warning in its header).
    std::unique_ptr<wser::WserUdpSocket> wudp;
    wser::wser_endpoint wpeer{};   // learned peer (mirror of peer_addr)
#endif
    bool udp{false};
    // peer address MUST be a real sockaddr type, never a byte array: lwip's
    // sendto validates pointer ALIGNMENT and returns EINVAL on a misaligned
    // sockaddr -- the field-found bug behind txerr climbing forever while
    // recvfrom (local, naturally aligned storage) worked fine.
    struct sockaddr_storage peer_addr {};
    uint8_t peer_len{0}; uint16_t peer_port{0}; bool have_peer{false};
    // counters
    uint32_t tx{0}, rx{0}, drop{0}, txerr{0};
    uint8_t txfail{0};   // consecutive hard sendto failures (drives recreation)
    // loop detect (passive): ring of recent EGRESS fingerprints + streak
    struct LpFp { uint32_t h; uint16_t len; uint32_t ms; };
    LpFp lp_ring[4]{};
    uint8_t lp_w{0};       // ring write index
    uint8_t lp_streak{0};  // consecutive SAME-vlan echoes -> LOOP
    uint8_t xv_streak{0};  // consecutive CROSS-vlan echoes -> XVLAN bridge
    bool lp{false};        // sticky LOOP badge
    bool xlp{false};       // sticky XVLAN badge
    char name[20]{0};
  };
  static_assert(alignof(struct sockaddr_storage) >= 4,
                "peer_addr must satisfy lwip's sockaddr alignment check");
  Port ports_[WSER_MAX_PORTS];
  int uart_pi_{-1}, console_pi_{-1};  // indices of the two fixed ports
  uint8_t console_vlan_{1};           // the VLAN the web console talks on

  int port_alloc_(PortType t, uint16_t net_port, uint16_t buf_cap, bool udp);
  void port_free_(int idx);
  bool port_rate_ok_(Port &p, size_t n);
  size_t egress_room_(Port &p);
  void egress_spend_(Port &p, size_t n);
  void bstat_tick_();
  void port_wire_drain_(int idx);
  // wire-injection context: the declared crossing is XVLAN-exempt, and the
  // origin rule traverses the wire (no ping-pong back into the pair)
  bool wire_ctx_{false};
  int8_t wire_src_{-1};   // token bucket
  void port_enqueue_(int idx, const uint8_t *d, size_t n);  // egress buffer
  void switch_ingress_(int src_idx, const uint8_t *d, size_t n);  // the hub core
  void port_net_loop_(int idx);
  int find_free_port_();
  // persistence: dynamic ports + VLANs survive reboot (the startup-config)
  void save_config_();
  void load_config_();
  ESPPreferenceObject pref_;
  struct SavedPort { uint8_t type; uint8_t vlan; uint8_t up; uint16_t net_port; uint16_t buf_cap; uint32_t rate; uint32_t orate; int8_t wire; };
  struct SavedCfg { uint8_t magic; uint8_t console_vlan; uint8_t nports; uint8_t loopdet; uint8_t xvlandet; SavedPort ports[WSER_MAX_PORTS]; };

  // ---- loop detection (PASSIVE; storm control stays independent) ----
  // Zero bytes injected on any port, ever. OFF = 0, ON = 1 (badge + log),
  // KILL = 2 (also DOWN the guilty port). See lp_ingress_ for the mechanism.
  uint8_t loopdet_{1};
  // XVLANDETECT: same engine, other verdict. A match whose fingerprint came
  // from a port in ANOTHER vlan is not a loop (the frame crosses once and
  // dies) -- it is an inter-VLAN BRIDGE, i.e. broken isolation. Independent
  // switch so deliberate bridging can be allowed (OFF) while loops stay
  // hunted, or vice versa. Default ON: accidental bridges deserve a badge.
  uint8_t xvlandet_{1};
  int loop_pi_{-1};             // last port a loop was declared on
  void lp_egress_(Port &p, const uint8_t *d, size_t n);      // fingerprint what we send
  bool lp_ingress_(int pi, const uint8_t *d, size_t n);      // true = KILL dropped it

  // ---- memory guard (heap / largest block / fragmentation) ----
  uint32_t largest_block_();
  uint8_t frag_pct_();
  bool guard_ok_(size_t want);   // true if allocating `want` keeps us above floor
  uint32_t buffers_alloc_{0};    // total bytes held in port buffers
  uint32_t heap_floor_{WSER_HEAP_FLOOR};  // configurable safety floor

  // wire RX staging: chunks, not bytes, enter the switch (perf + fingerprints)
  uint8_t rx_stage_[64];
  uint8_t rx_stage_n_{0};
  void rx_stage_flush_();

  uart::UARTComponent *uart_{nullptr};
  GPIOPin *de_pin_{nullptr};
  bool owner_{true};
  uint16_t gap_ms_{10};
  uint8_t delim_{0};
  void wire_write_(const uint8_t *d, size_t n, bool self, int origin_pi = -1);
  bool self_active_{false};
  uint16_t init_tcp_port_{0};    // from YAML: pre-provisioned at boot

  // ---- framing pipeline (family DNA, direction-fair throttle) ----
  SerFrame fr_{};
  uint32_t last_byte_us_{0};
  bool tap_off_{false}, tap_full_{true}, tap_batch_{false};
  int tap_filter_dir_{-1};
  int arm_dir_{-2};
  int arm_byte_{-1};
  uint32_t observed_{0}, dropped_{0}, last_observed_{0}, obs_rate_{0};
  // Tap-drop causes (VIEWSTAB backend phase): the aggregate above stays
  // authoritative, these split it for the funnel view. RAM only.
  uint32_t drop_thr_{0};   // 20 ms same-direction throttle
  uint32_t drop_heap_{0};  // heap guard (largest < 1536, 8266)
  uint32_t drop_bkl_{0};   // WS backlog gate
  uint8_t info_pg_{0};     // PGINFO: alternating ports half (0=ids 0-7, 1=ids 8-15)
  // QOSLANE: control-plane starvation guard. info_ok_ms_ = last SUCCESSFUL
  // info push; info_starve_ms_ throttles the distress line (5 s).
  uint32_t info_ok_ms_{0};
  uint32_t info_starve_ms_{0};
  // BBSHOTS: the black-box report fires UNCONDITIONALLY on the logger at
  // 15/25/35 s post-boot (the OTA viewer is attached by then) -- no web
  // client prerequisite. bb_valid_ = a crumb matched at setup this boot.
  bool bb_valid_{false};
  uint8_t bb_shots_{0};
  // TRUTHCONS: the console egress ring -- variable records [src][len][bytes],
  // loss record [0xFF][4][u32-LE lost]. Loss is possible, lying is not.
  uint8_t cons_ring_[1024];
  uint16_t cr_w_{0}, cr_r_{0};
  uint32_t cons_gap_{0};
  void cons_record_(uint8_t src, const uint8_t *d, size_t n);
  void cons_drain_();
  // ALLOCQUIET: the info/bb builder reuses ONE buffer reserved at setup --
  // the main-context allocator occupancy per push drops from ~50 ops to ~0,
  // closing the CONT-vs-SYS umm collision window (field HWDT, ctx sys,
  // lwip->umm on top, our send chain beneath).
  std::string info_buf_;
  bool coh_exempt_{false};   // control frames (info/bb) bypass the coh cap
  uint8_t pass_subs_{0};     // ws encodes this pass (coh valve counter)
  // Black-box snapshot: copied from the .noinit crumb at boot BEFORE its
  // reset, pushed once to the first web client (cold path).
  bool bb_pending_{false};
  uint32_t bb_phase_{0}, bb_passes_{0}, bb_mxp_{0}, bb_mxw_{0}, bb_mxh_{0}, bb_evt_{0}, bb_evtp_{0};
  void bb_push_();
  uint32_t tx_bytes_{0}, rx_bytes_{0}, tx_rate_{0}, rx_rate_{0};
  uint32_t last_tx_b_{0}, last_rx_b_{0};
  uint32_t last_tap_log_us_{0};
  uint8_t last_emit_dir_{2};
  int8_t frame_src_pi_{-1};
  bool flood_{false};
  bool tap_visible_();  // wire-tap display follows the console's VLAN
  void frame_byte_(uint8_t b, uint8_t dir);
  void frame_close_();
  void flush_frame_();
  struct AggE { uint8_t dir; bool self; uint32_t total; uint32_t count; uint32_t us; };
  AggE agg_[10];
  uint8_t agg_used_{0};
  void flush_agg_();

  // ---- experimental DMX TX (break via baud trick) ----
  void dmx_tx_(const uint8_t *ch, size_t n);
  void bridge_push_(int port_id, const uint8_t *d, size_t n);  // to browser (0x02 id data)
  void console_push_(int src_pi, const uint8_t *d, size_t n);  // switch egress to the web log
  int bridge_pi_[4]{-1,-1,-1,-1};   // up to 4 PC-serial bridges
};

// Modbus CRC16 (poly 0xA001 reflected), shared with the host tests
uint16_t wser_crc16(const uint8_t *d, size_t n);

}  // namespace web_serial
}  // namespace esphome
