#include "web_serial.h"
#ifdef WSER_HOST_TEST
// sandbox-only: the stub lwip types collide with esphome's HOST
// ip_address.h (ip_addr_t = in_addr there). The call site only needs a
// declaration; device builds below use the real header + real lwip.
namespace esphome {
namespace network {
bool is_connected();
}
}  // namespace esphome
#else
#include "esphome/components/network/util.h"
#endif
#ifdef USE_ESP32
#include <esp_heap_caps.h>   // heap_caps_get_largest_free_block
#include <esp_system.h>      // esp_get_free_heap_size
#include <esp_private/esp_clk.h>  // esp_clk_cpu_freq -- arduino AND idf (IDF >= 4)
#endif
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include <cstring>
#include <cstdio>
#ifdef USE_ESP8266
#include <Arduino.h>
#endif
#ifdef USE_ESP32
#include <esp_system.h>
#include <esp_heap_caps.h>
#endif

namespace esphome {
namespace web_serial {

static const char *const TAG = "web_serial";

#ifdef USE_ESP8266
// Black box: HW-WDT resets on the 8266 save NO context -- the "PC/BT" the
// crash handler prints is a FOSSIL from RTC (proven: byte-identical traces
// across five different builds). So we lay our own breadcrumbs in .noinit
// RAM, which survives a watchdog reset: one write per phase, read at boot.
// The crumb tells the truth the backtrace could not.
struct WserCrumb {
  uint32_t magic;
  uint32_t phase;
  uint32_t passes;
  // FORENSIC: worst-case block durations (us) survive the reset with the
  // crumb -- the boot report prints them, page or no page.
  uint32_t mxp;   // whole pass
  uint32_t mxw;   // 1 s window
  uint32_t mxh;   // umm heap walk (interrupt-locked!)
  uint8_t last_evt;      // 1 ws-connect, 2 info-send, 3 page-serve
  uint32_t last_evt_pass;
};
static WserCrumb s_crumb __attribute__((section(".noinit")));
#define WSER_EVT(e) do { s_crumb.last_evt = (e); s_crumb.last_evt_pass = s_crumb.passes; } while (0)
#define WSER_MAXC(field, v) do { if ((v) > s_crumb.field) s_crumb.field = (v); } while (0)
#define WSER_CRUMB(p) \
  do { \
    s_crumb.magic = 0x57435232UL; \
    s_crumb.phase = (p); \
  } while (0)
#else
#define WSER_EVT(e)
#define WSER_MAXC(field, v)
#define WSER_CRUMB(p)
#endif

static bool wser_read_hexv(const char *&p, uint32_t &out) {
  out = 0;
  bool any = false;
  while (*p) {
    char c = *p;
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else break;
    out = (out << 4) | (uint32_t) v;
    any = true;
    p++;
  }
  return any;
}

static const char WSER_PAGE[] PROGMEM = R"HTMLDOC(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>web_serial</title><style>
:root{--bg:#12161c;--panel:#1a2028;--bord:#2a3340;--line:#232b36;--fg:#cfd4db;--dim:#7a8494;--accent:#5dcaa5;--warn:#f6c177;--err:#e06c75;}
*{box-sizing:border-box;margin:0}body{background:var(--bg);color:var(--fg);font:14px/1.45 ui-sans-serif,system-ui,sans-serif;padding:10px 14px}
h1{font-size:15px;letter-spacing:.06em;color:var(--accent);display:flex;align-items:center;gap:8px}
#conn-dot{width:9px;height:9px;border-radius:50%;background:#e06c75;display:inline-block}
.badge{font-size:10px;border:1px solid var(--warn);color:var(--warn);border-radius:4px;padding:0 5px;display:none}
.wrap{display:flex;gap:12px;margin-top:8px}.main{flex:1;min-width:0}.side{width:280px;flex:none}
.card{background:var(--panel);border:1px solid var(--bord);border-radius:8px;padding:10px;margin-bottom:10px}
.lbl{font-size:11px;letter-spacing:.1em;color:var(--dim);margin-bottom:6px}
button{background:#222a34;color:var(--fg);border:1px solid var(--bord);border-radius:6px;padding:5px 10px;cursor:pointer;font-size:12.5px}
button:hover{border-color:var(--accent)}button.on{background:var(--accent);color:#0c1310;border-color:var(--accent)}
input,select{background:#10151b;color:var(--fg);border:1px solid var(--bord);border-radius:5px;padding:4px 6px;font-size:12.5px}
.row{display:flex;gap:6px;align-items:center;flex-wrap:wrap;margin:4px 0}
.hint{font-size:11.5px;color:var(--dim)}.seg-lbl{font-size:12px;color:var(--dim);min-width:56px;display:inline-block}
.prow2{display:flex;align-items:center;gap:4px;margin:-2px 0 4px 100px;font-size:11px}
.bar{display:inline-block;width:64px;height:6px;background:#20242c;border-radius:3px;overflow:hidden;vertical-align:middle}
.bar i{display:block;height:100%;width:0;background:#5dcaa5;transition:width .15s}
.bar i.hot{background:#e06c75}
.seg-b{padding:3px 9px;border-radius:5px;font-size:12px}.mono{font-family:ui-monospace,Menlo,Consolas,monospace}
#log-row{display:flex;gap:8px;margin-top:10px;height:560px}
#log-col{flex:1.15;min-width:0;display:flex;flex-direction:column}
#right-col{flex:1;min-width:0;display:flex;flex-direction:column}
#log-col #send-card{margin-bottom:8px}
#raw-wrap{flex:1;min-height:0;display:flex;flex-direction:column;background:var(--panel);border:1px solid var(--bord);border-radius:8px}
#raw-bar{display:flex;gap:6px;align-items:center;padding:6px 8px;border-bottom:1px solid var(--line)}
#raw-out{flex:1;overflow-y:auto;padding:6px 8px;font-family:ui-monospace,monospace;font-size:12px;line-height:1.35;white-space:pre}
#raw-out>div{min-height:1.35em}
#raw-out:focus{outline:1px solid var(--accent);outline-offset:-1px}
#raw-bar input[type=checkbox]{padding:0;margin:0;width:12px;height:12px;vertical-align:-1px}
#tabbar{display:flex;gap:22px;border-bottom:1px solid var(--bord);margin:2px 2px 10px;padding:0}
button.tb{background:none;border:none;border-bottom:2px solid transparent;border-radius:0;color:var(--dim);font-size:12.5px;letter-spacing:.04em;padding:5px 1px;margin-bottom:-1px;cursor:pointer}
button.tb:hover{color:var(--fg);border-color:var(--line)}
button.tb.on{color:var(--accent);border-bottom-color:var(--accent)}
#vgrid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
#vgrid .card{margin-bottom:0}
#vgrid canvas{width:100%;background:#10151b;border-radius:6px;display:block}
.vrow{display:flex;gap:8px}
.vstat{flex:1;background:#10151b;border-radius:6px;padding:6px 8px;display:flex;flex-direction:column;gap:2px}
.vstat b{font-size:15px}
.vbar{height:8px;background:#10151b;border-radius:3px;overflow:hidden;display:flex;margin:3px 0 8px}
.vbar i{display:block;height:100%}
.vgauge{position:relative;height:14px;background:#10151b;border-radius:4px;margin-top:12px}
.vgauge i{position:absolute;left:0;top:0;bottom:0;background:#0f6e56;border-radius:4px 0 0 4px}
.vgauge b{position:absolute;top:-5px;bottom:-5px;width:2px}
#vw-mm-g{background:#f6c177}#vw-mm-r{background:#e06c75}#vw-mm-o{background:#7aa2f7}
.raw-cur{display:inline-block;width:.55em;height:1.05em;background:var(--accent);vertical-align:-2px;animation:rawblink 1.1s steps(1) infinite}
@keyframes rawblink{50%{opacity:0}}
#raw-out:not(:focus) .raw-cur{background:transparent;box-shadow:inset 0 0 0 1px var(--dim);animation:none}
#log-wrap{flex:1.2;min-height:0;display:flex;flex-direction:column;background:var(--panel);border:1px solid var(--bord);border-radius:8px}
#log-bar{display:flex;gap:6px;align-items:center;padding:6px 8px;border-bottom:1px solid var(--line)}
.ib{width:28px;height:26px;padding:0;display:inline-flex;align-items:center;justify-content:center}
.ib svg{width:15px;height:15px}.ib.act,.ib.armed{background:var(--warn);color:#141414;border-color:var(--warn)}
.fbox{display:inline-flex;align-items:center;gap:4px;background:#10151b;border:1px solid var(--bord);border-radius:5px;padding:2px 6px}
.fbox svg{width:12px;height:12px;color:var(--dim)}.fbox input{border:none;background:none;padding:2px;width:56px}
#out{flex:1;overflow-y:auto;padding:6px 8px;font-family:ui-monospace,monospace;font-size:12px}
.lts{color:#4d5663;margin-right:7px;font-size:10.5px}
.l-sys{color:#8f9aab}.l-err{color:var(--err)}.l-ok{color:var(--accent)}
.l-tx{color:#e0a765}.l-rx{color:#6fb0c9}.l-txd{color:#7a6547}.l-rxd{color:#586b7a}
#decode-mid{display:flex;align-items:center;justify-content:center;flex:0 0 30px;background:var(--bg)}
#dec-btn{width:30px;height:30px;border-radius:50%;border:1px solid var(--bord);background:var(--panel);color:var(--accent);cursor:pointer;display:flex;align-items:center;justify-content:center;padding:0}
#dec-btn:hover{border-color:var(--accent)}#dec-btn svg{width:16px;height:16px}
#dec-panel{flex:1;min-height:0;display:flex;flex-direction:column;background:var(--panel);border:1px solid var(--bord);border-radius:8px}
.dec-head{display:flex;gap:6px;align-items:center;padding:6px 8px;border-bottom:1px solid var(--line)}
#dec-out{flex:1;overflow-y:auto;padding:8px;font-size:12px}
.dempty{color:var(--dim);text-align:center;padding:30px 10px;line-height:1.7}
.dtxn{border-bottom:1px solid var(--line);padding:6px 2px;font-family:ui-monospace,monospace}
.d-ts{color:#5f6570;font-size:11px}.drow{display:flex;gap:10px}.dhx{min-width:38px;color:#9aa4b2}
.dhx.d-tx{color:#e3b341}.dhx.d-rx{color:#6fb0c9}.dmn{color:#cfd4db}
.dsum{color:#cfd4db;font-size:11.5px;margin-top:3px;padding-top:3px;border-top:1px dashed var(--line)}
.d-note{color:#5f6570;font-size:10.5px;font-style:italic}
.dnote{background:#3a2f1a;border:1px solid var(--warn);color:#e8c98a;border-radius:6px;padding:8px 10px;font-size:12px;margin-bottom:8px}
.dmxbar{display:inline-block;width:5px;background:var(--accent);vertical-align:bottom;margin-right:1px}
.kv{display:flex;justify-content:space-between;font-size:12.5px;margin:2px 0}.kv b{font-family:ui-monospace,monospace;font-weight:500}
.rgrp{font-size:10px;letter-spacing:.12em;color:var(--dim);margin:10px 0 4px}
canvas.spark{width:100%;height:30px;background:#10151b;border-radius:4px}
</style></head><body>
<h1><span id="conn-dot"></span> web_serial <span class="badge" id="p-flood">FLOODED</span><span class="badge" id="p-tcp">TCP232</span><span class="badge" id="p-pc">PC SERIAL</span></h1>
<div class="wrap"><div class="main">
<div id="tabbar"><button class="tb on" id="tb-general">General</button><button class="tb" id="tb-uart">UART</button><button class="tb" id="tb-views">Views</button></div>
<div id="tab-general">
<div class="card"><div class="lbl">SWITCH PORTS <span class="hint" id="port-count"></span>
    <span style="float:right"><button class="seg-b" id="port-reset" title="zero all tx/rx/drop counters and clear LOOP/XVLAN badges">Reset counters</button> <button class="seg-b" id="add-tcp">+ TCP</button> <button class="seg-b" id="add-udp" title="UDP hub port (VCOM UDP mode) -- the peer is LEARNED from the first incoming datagram, so the client must speak first">+ UDP</button> <button class="seg-b" id="add-bridge">+ Bridge</button></span></div>
  <div class="row" style="margin:2px 0 6px"><span class="seg-lbl">New buffer</span><input id="new-buf" value="256" style="width:60px"><span class="hint">bytes/port</span>
    <span class="hint" id="buf-guard" style="margin-left:6px"></span>
    <span class="seg-lbl" style="min-width:0;margin-left:10px">Floor</span><input id="floor" style="width:56px"><span class="hint">B</span>
    <span class="seg-lbl" style="min-width:0;margin-left:10px">Loop detect</span><button class="seg-b" id="ld-off" title="no detection">Off</button><button class="seg-b" id="ld-on" title="passive: flags a port whose ingress matches recent hub egress from the SAME vlan (3x in 3s); nothing is ever injected in the stream">On</button><button class="seg-b" id="ld-kill" title="on detection, the guilty port also goes DOWN">Kill</button><span class="seg-lbl" style="min-width:0;margin-left:10px">XVLAN detect</span><button class="seg-b" id="xv-off" title="inter-VLAN bridging is tolerated silently">Off</button><button class="seg-b" id="xv-on" title="passive: flags a port whose ingress matches recent hub egress from ANOTHER vlan (3x in 3s) -- broken isolation">On</button><button class="seg-b" id="xv-kill" title="on detection, the bridged-into port also goes DOWN">Kill</button></div>
  <div id="ports"></div>
  <p class="hint" style="margin-top:6px">VLAN pill = click to cycle. Ports in different VLANs never see each other. <span style="color:#e06c75">red dot</span> = storm control tripping (rate cap hit, excess dropped). <span style="color:#e06c75">&#10226; LOOP</span> = passive loop detect: ingress kept matching recent hub egress from the SAME vlan (3x in 3s) -- the frame came back where it started, recirculation. <span style="color:#f6c177">&#8644; XVLAN</span> = ingress kept matching recent hub egress from ANOTHER vlan -- an external path bridges two vlans (crosses once, no recirculation). Independent On/Off/Kill each; zero bytes ever injected; Kill downs the ingress port; PORT UP or a vlan change clears badges and restarts detection.</p>
</div>
<div class="card"><div class="lbl">VLAN MAP <span class="hint">who reaches whom -- the console's VLAN is the pill on its own port row</span></div>
  <div id="vlanmap" style="display:flex;gap:8px;flex-wrap:wrap"></div>
</div>
<div id="log-row">
  <div id="log-col">
  <div class="card" id="send-card"><div class="lbl">SEND ON THE WIRE <span class="hint">-- real TX when the uart shares the console&#39;s VLAN (tapped as &#171; TX self &#187;); otherwise the send follows the VLAN map</span></div>
    <div class="row"><select id="tx-mode"><option value="text">Text</option><option value="hex">Hex</option></select>
      <input id="tx-data" class="mono" placeholder="hello or 01 03 00 00 00 02" style="flex:1;min-width:120px">
      <select id="tx-eol"><option value="">none</option><option value="crlf" selected>CR+LF</option><option value="cr">CR</option><option value="lf">LF</option></select>
      <button id="tx-send">Send</button></div>
    <div class="row"><span class="seg-lbl">DMX</span><input id="dmx-ch" class="mono" placeholder="FF 80 00 ..." style="flex:1;min-width:100px">
      <button id="dmx-send" title="EXPERIMENTAL: break via baud trick, then start code 00 + channels">TX frame</button>
      <span class="hint">experimental (break by baud switch)</span></div>
  </div>
  <div id="raw-wrap"><div id="raw-bar">
    <span class="lbl" style="margin:0">RAW SERIAL</span>
    <span class="hint" title="bytes the local COM devices SENT us (bridge 0x01 ingress) -- our own egress is deliberately absent: a device with ECHO ON returns it on the wire">vlan terminal &middot; 80x25, no scrollback</span>
    <span id="raw-vlan" class="hint" title="the whole vlan, byte-exact when in-sync; every gap is confessed inline and here"></span>
    <span id="raw-kbd" class="hint" title="click the screen to type: keystrokes are broadcast into the console's VLAN (same path as the Send box), so they reach the wire, the net ports AND any COM bridge in that vlan. Ctrl+A..Z send 0x01..0x1A; Shift+Tab leaves the screen">click to type</span>
    <label class="hint" style="display:inline-flex;align-items:center;gap:3px" title="LOCAL ECHO -- purely cosmetic. It only DRAWS your keystrokes on this screen; it sends nothing extra, and leaves the hub's loopback and the serial tx/rx paths completely untouched. If the device also echoes, you will see each character twice: that is exactly what this switch is for."><input type="checkbox" id="raw-echo"> echo</label>
    <span id="raw-legend" style="flex:1;text-align:right"></span>
    <span id="raw-prog" style="display:none"><span class="hint" id="raw-prog-t"></span>
      <button class="ib" id="raw-abort" title="Abort the transfer"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><rect x="6" y="6" width="12" height="12" rx="1"/></svg></button></span>
    <input type="file" id="raw-fin" style="display:none">
    <button class="ib" id="raw-file" title="Dump a file onto the console's VLAN -- paced by the browser to the wire's speed (baud/10), with backpressure on the uart buffer: the ESP is never asked to absorb more than it can drain"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 16V4M8 8l4-4 4 4"/><path d="M4 17v2a1 1 0 001 1h14a1 1 0 001-1v-2"/></svg></button>
    <button class="ib" id="raw-paste" title="Paste the clipboard onto the VLAN (Ctrl+V on the screen does the same, with no permission prompt)"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linejoin="round"><rect x="6" y="4" width="12" height="16" rx="1"/><rect x="9" y="2" width="6" height="4" rx="1"/></svg></button>
    <button class="ib" id="raw-clear" title="Clear the screen"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M4 7h16M9 7V5h6v2M6 7l1 13h10l1-13"/></svg></button>
  </div><div id="raw-out" tabindex="0"></div></div>
  </div>
  <div id="right-col">
  <div id="log-wrap"><div id="log-bar">
    <button class="ib" id="log-pause" title="Pause"><svg viewBox="0 0 24 24" fill="currentColor"><rect x="6" y="5" width="4" height="14" rx="1"/><rect x="14" y="5" width="4" height="14" rx="1"/></svg></button>
    <span class="fbox"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M3 5 H21 L14 13 V19 L10 21 V13 Z"/></svg><input id="log-filter" placeholder="filter"></span>
    <span class="fbox" title="One-shot: dir TX/RX/* + first byte hex"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="6.5"/><line x1="12" y1="1.5" x2="12" y2="5.5"/><line x1="12" y1="18.5" x2="12" y2="22.5"/><line x1="1.5" y1="12" x2="5.5" y2="12"/><line x1="18.5" y1="12" x2="22.5" y2="12"/></svg><input id="arm-dir" placeholder="*" style="width:26px"><input id="arm-b" placeholder="b0" style="width:30px"></span>
    <button class="ib" id="arm-btn" title="Arm one-shot capture past the throttle"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="8"/><circle cx="12" cy="12" r="2" fill="currentColor"/></svg></button>
    <span style="flex:1"></span>
    <button class="ib" id="log-csv" title="Export CSV"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="4" width="18" height="16" rx="1"/><line x1="3" y1="9.5" x2="21" y2="9.5"/><line x1="9" y1="9.5" x2="9" y2="20"/></svg></button>
    <button class="ib" id="log-clear" title="Clear"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M4 7h16M9 7V5h6v2M6 7l1 13h10l1-13"/></svg></button>
  </div><div id="out"></div></div>
  <div id="decode-mid"><button id="dec-btn" title="Decode the frames currently in the log"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="transform:rotate(90deg)"><line x1="4" y1="12" x2="18" y2="12"/><path d="M13 6 L19 12 L13 18"/></svg></button></div>
  <div id="dec-panel"><div class="dec-head"><span class="lbl" style="margin:0">SERIAL DECODE</span>
    <span class="fbox" style="margin-left:6px"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M3 5 H21 L14 13 V19 L10 21 V13 Z"/></svg><input id="dec-filter" placeholder="filter"></span>
    <span style="flex:1"></span>
    <button class="ib" id="dec-clear" title="Clear"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M4 7h16M9 7V5h6v2M6 7l1 13h10l1-13"/></svg></button>
  </div><div id="dec-out"><div class="dempty">No decode yet &mdash;<br>click the <b>&#8595;</b> arrow to decode the<br>frames currently in the log.<br><span class="hint">Auto-detects Modbus RTU (CRC16), DMX512, NMEA 0183, AT.</span></div></div></div>
  </div>
</div>
</div>
<div id="tab-uart" style="display:none">
<div class="card"><div class="lbl">LINE SETTINGS <span class="hint">-- live via load_settings(), no reflash</span></div>
  <div class="row">
    <span class="seg-lbl">Baud</span><input id="s-baud" value="115200" style="width:74px">
    <span class="seg-lbl" style="min-width:34px">Data</span><select id="s-data"><option>8</option><option>7</option></select>
    <span class="seg-lbl" style="min-width:40px">Parity</span><select id="s-par"><option>N</option><option>E</option><option>O</option></select>
    <span class="seg-lbl" style="min-width:34px">Stop</span><select id="s-stop"><option>1</option><option>2</option></select>
    <button id="s-apply">Apply</button>
    <button id="s-dmx" title="250000 8N2 + 4ms gap">DMX preset</button>
    <button id="s-mb" title="gap = 4 ms (3.5 chars at 9600+)">Modbus preset</button></div>
  <div class="row"><span class="seg-lbl">Frame gap</span><input id="s-gap" value="10" style="width:44px"><span class="hint">ms of silence closing a frame</span><span class="hint" id="gap-warn" style="display:none;color:#f6c177"></span>
    <span class="seg-lbl" style="min-width:40px">Delim</span><input id="s-delim" placeholder="0A" style="width:36px"><button id="s-fapply">Set</button></div>
</div>
<div class="card"><div class="lbl">UART &amp; WIRING GUIDE <span class="hint">-- how to configure the YAML and cable the chip (the map 9 field resets paid for)</span></div>
<details><summary class="hint" style="cursor:pointer">open the guide -- pin combos, strapping traps, DMX, recipes (your build's verdict is the UART row in SYSTEM)</summary>
<div style="margin-top:8px;padding:8px;border-left:3px solid #56c8d8">
<b>GPIO1/3 vs GPIO15/13 -- the FORENSICS trade-off (learned the hard way)</b><br>
Crash postmortems (Exception decode + stack) print on <b>UART0 wherever it
currently points</b>; the ROM boot banner always hits GPIO1 (74880 bd, and
mirrors on GPIO2 during boot).<br>
<b>GPIO15/13 (swap, "production")</b>: clean bus, USB=power-only, logger free
on GPIO2 -- BUT every crash postmortem goes out GPIO15 <i>into your device</i>,
invisible: your only witness is the RTC black box. Strap: GPIO15 must be LOW
at boot.<br>
<b>GPIO1/3 (default, "debug / first choice")</b>: the CH340 becomes a free
forensics console -- ROM banner (74880), HWDT stack dump (74880), exception
postmortem (at the bus baud, e.g. 19200) all land on the board's USB COM --
BUT the 74880 boot spew is injected into whatever device sits on the bus, and
PC-side noise can too.<br>
<b>Rule of thumb</b>: bring-up &amp; bug hunts on GPIO1/3; deployed benches on
GPIO15/13 with the black box as coroner. Soft-uart (bit-bang) for data: avoid
-- CPU-hungry, wedge-prone; that era is closed.
</div>
<div class="hint" style="line-height:1.55;margin-top:6px">
<b>ESP8266 -- exactly three HARDWARE combos; anything else falls back to SOFTWARE bit-bang, silently:</b><br>
&nbsp;&nbsp;UART0 &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;tx GPIO1 / rx GPIO3 &nbsp;-- shared with the USB chip: avoid for data<br>
&nbsp;&nbsp;UART0-swap &nbsp;tx GPIO15 / rx GPIO13 -- THE full-duplex choice; needs the logger off UART0 (recipe below)<br>
&nbsp;&nbsp;UART1 &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;tx GPIO2, NO rx_pin &nbsp;-- TX-only by silicon (its RX is eaten by the flash)<br>
<b>Strapping-pin wiring rules (boot depends on them):</b> GPIO15 must read LOW at reset (D1 mini has the pulldown) -- a peer idling the line HIGH blocks boot: 470R in series, or unplug to flash. GPIO0 pulled low at reset = flash mode; GPIO2 must be high. Never let external gear load them during reset.<br>
<b>Never for serial:</b> GPIO6-11 (SPI flash bus), GPIO16 (no interrupt path).<br>
<b>SOFTWARE bit-bang</b> (any other pin combo): ~57600 practical max; sustained TX under WiFi load can wedge the chip (this hub auto-shields its egress to ~2 ms/pass and the UART row turns amber) -- fine for a slow sensor, wrong for a switch.<br>
<b>DMX512</b> (250000 8N2 -- a baud the 8266 hits with 0.000% divider error): a full frame is 513 B, so RX capture needs <code>rx_buffer_size: 1024</code>.<br>
<b>ESP32:</b> pin matrix -- any pins, up to 5 Mbaud, fractional divider. Only rules: GPIO34-39 are input-only (RX ok, TX impossible) and 6-11 stay with the flash. No other constraint: this hub saturates whatever wire you give it.<br>
<b>YAML recipes:</b></div>
<pre class="hint" style="margin:6px 0 0 0">
# TX-only (UART1)      # Full-duplex (UART0-swap)     # DMX-ready
uart:                  logger:                        uart:
  tx_pin: GPIO2          hardware_uart: UART1           tx_pin: GPIO15
  baud_rate: 19200     uart:                            rx_pin: GPIO13
                         tx_pin: GPIO15                 baud_rate: 250000
                         rx_pin: GPIO13                 stop_bits: 2
                                                        rx_buffer_size: 1024</pre>
</details></div>
<div class="card"><div class="lbl">PC SERIAL BRIDGE <span class="hint">-- your computer's COM port becomes a hub port (Web Serial API)</span></div>
  <div class="row"><button id="pc-conn">Connect a local port</button><span class="hint" id="pc-st">no port connected</span>
    <input id="pc-baud" value="115200" style="width:74px"><span class="hint">baud applied at connect</span></div>
  <div id="pc-list"></div>
  <p class="hint">Chrome only; on plain http, enable chrome://flags/#unsafely-treat-insecure-origin-as-secure for this origin. The browser never reveals which COM number you picked (privacy): links are named by USB chip id -- a VCOM/virtual port shows as 'virtual/unnamed'. Up to 4 local ports, each its own hub port: bytes from a PC port enter the hub through its bridge; hub egress to that bridge is written back to it.</p>
</div>
</div>
<div id="tab-views" style="display:none">
<div id="vgrid">
<div class="card"><div class="lbl">TOPOLOGY <span class="hint">ports, wires, vlans -- click a node for its blast radius</span><span id="vw-age" class="hint" style="float:right"></span></div>
  <canvas id="vw-topo" width="440" height="230"></canvas>
  <div class="hint" id="vw-topo-note">red ring = LOOP badge -- amber = XVLAN -- self-arc = hairpin -- chord = wire -- dim = DOWN</div></div>
<div class="card"><div class="lbl">GOVERNOR <span class="hint">drain quota AIMD -- last 5 min</span></div>
  <canvas id="vw-gov" width="440" height="96"></canvas>
  <div class="hint" id="vw-gov-note">waiting for samples</div></div>
<div class="card"><div class="lbl">LIVE MODEL <span class="hint">predicted vs measured -- a continuous regression test</span></div>
  <div class="vrow"><span class="vstat"><span class="hint">D measured = swr / I</span><b id="vw-d">-</b></span>
    <span class="vstat"><span class="hint">D predicted (bottleneck law)</span><b id="vw-pd">-</b></span>
    <span class="vstat"><span class="hint">link predicted (I+2)D</span><b id="vw-pl">-</b></span></div>
  <div class="row" style="margin-top:7px"><span id="vw-badge" class="hint">no active bench</span></div>
  <div class="row" style="margin-top:2px"><span class="hint" id="vw-gk"></span><div class="row" style="margin-top:2px"><span class="hint" id="vw-cost"></span></div></div>
  <div class="row" style="margin-top:7px"><span class="hint">what-if</span>
    <span class="hint">I</span><input type="range" id="vw-wi" min="1" max="14" value="6" style="width:80px"><span class="hint mono" id="vw-wio">6</span>
    <span class="hint">quota</span><input type="range" id="vw-wq" min="64" max="2048" step="64" value="2048" style="width:80px"><span class="hint mono" id="vw-wqo">2048</span>
    <span class="hint mono" id="vw-wr"></span></div></div>
<div class="card"><div class="lbl">FLOW &amp; CONSERVATION <span class="hint" id="vw-fl-vlan">dominant vlan</span></div>
  <div class="hint">accepted injection <span class="mono" id="vw-fl-i">-</span></div><div class="vbar"><i id="vw-fl-ib" style="background:#0f6e56"></i></div>
  <div class="hint">fabric deliveries <span class="mono" id="vw-fl-d">-</span></div><div class="vbar"><i id="vw-fl-db" style="background:#185fa5"></i></div>
  <div class="hint">buffer eviction <span class="mono" id="vw-fl-e">-</span> -- recycled <span class="mono" id="vw-fl-r">-</span></div>
  <div class="vbar"><i id="vw-fl-eb" style="background:#993c1d"></i><i id="vw-fl-rb" style="background:#0f6e56"></i></div></div>
<div class="card"><div class="lbl">INTER-ARRIVALS <span class="hint">frame gaps sampled from the log buffer</span></div>
  <canvas id="vw-gaps" width="440" height="96"></canvas>
  <div class="hint" id="vw-gaps-note">-</div></div>
<div class="card"><div class="lbl">MEMORY MAP <span class="hint">largest block vs the three floors</span></div>
  <div class="vgauge"><i id="vw-mm-f"></i><b id="vw-mm-g"></b><b id="vw-mm-r"></b><b id="vw-mm-o"></b></div>
  <div class="hint" style="margin-top:9px"><span style="color:#f6c177">|</span> guard 1536 &nbsp; <span style="color:#e06c75">|</span> radio brake <span id="vw-mm-rl">-</span> &nbsp; <span style="color:#7aa2f7">|</span> floor <span id="vw-mm-fl">-</span></div>
  <div class="hint" id="vw-mm-note">-</div></div>
<div class="card"><div class="lbl">TAP FUNNEL <span class="hint">why a frame did not reach the log (cumulative)</span></div>
  <div class="hint">offered to the tap <span class="mono" id="vw-fn-o">-</span></div><div class="vbar"><i id="vw-fn-ob" style="background:#185fa5"></i></div>
  <div class="hint">throttle 20 ms <span class="mono" id="vw-fn-t">-</span> -- heap guard <span class="mono" id="vw-fn-h">-</span> -- ws backlog <span class="mono" id="vw-fn-b">-</span></div>
  <div class="vbar"><i id="vw-fn-tb" style="background:#993c1d"></i><i id="vw-fn-hb" style="background:#854f0b"></i><i id="vw-fn-bb" style="background:#72243e"></i><i id="vw-fn-sb" style="background:#0f6e56"></i></div>
  <div class="hint">shown in the log <span class="mono" id="vw-fn-s">-</span> <span class="hint" id="vw-fn-note"></span></div></div>
<div class="card"><div class="lbl">ENDURANCE <span class="hint">session health -- valve firings per hour</span></div>
  <div class="hint">uptime <span class="mono" id="vw-en-up">-</span> -- throttle/h <span class="mono" id="vw-en-th">-</span> -- ws drops/h <span class="mono" id="vw-en-ws">-</span> -- tap drops/h <span class="mono" id="vw-en-dp">-</span></div>
  <div class="hint" id="vw-en-bb">black box: -</div>
  <div class="hint" id="vw-en-lg">-</div></div>
</div>
</div>
</div>
<div class="side">
<div class="card"><p class="rgrp">MEMORY</p>
  <div class="kv"><span>Free heap</span><b id="i-heap">-</b></div><canvas class="spark" id="sp-heap" width="250" height="26"></canvas>
  <div class="kv" style="margin-top:4px"><span>Largest block</span><b id="i-largest" style="color:#6fb0c9">-</b></div>
  <div class="kv"><span>Fragmentation</span><b id="i-frag" style="color:#f6c177">-</b></div>
  <div class="kv"><span>Port buffers</span><b id="i-bufs">-</b></div>
  <p class="rgrp">THROUGHPUT</p>
  <div class="kv"><span>TX</span><b id="i-tx">0 B/s</b></div><canvas class="spark" id="sp-tx" width="250" height="26"></canvas>
  <div class="kv" style="margin-top:4px"><span>RX</span><b id="i-rx">0 B/s</b></div><canvas class="spark" id="sp-rx" width="250" height="26"></canvas>
  <div class="kv" style="margin-top:4px"><span>Switched</span><b id="i-swr" title="EVERYTHING the fanout carries per second -- boundary and internal (trunks, hairpins). TX/RX above only see the boundary; this is the true work of the switch.">-</b></div>
  <p class="rgrp">MONITOR</p>
  <div class="kv"><span>Frames</span><b id="i-obs">0</b></div>
  <div class="kv"><span>Dropped</span><b id="i-drop">0</b></div>
  <div class="kv"><span>WS drops</span><b id="i-wsdrop">0</b></div>
  <div class="kv"><span class="hint">drop causes</span><b class="hint mono" id="i-dropc" title="the Dropped aggregate split: 20 ms throttle / heap guard / WS backlog">-</b></div>
  <p class="rgrp">LOGGING</p>
  <div class="row"><span class="seg-lbl">Detail</span><button class="seg-b" id="tap-full">Full</button><button class="seg-b" id="tap-sum">Summary</button></div>
  <div class="row"><span class="seg-lbl">Delivery</span><button class="seg-b" id="tap-live">Live</button><button class="seg-b" id="tap-batch">Batch</button><button class="seg-b" id="tap-off">Off</button></div>
  <div class="row"><span class="seg-lbl">Filter</span><select id="tap-filter"><option value="">all</option><option value="0">TX</option><option value="1">RX</option></select></div>
  <div class="kv" id="tap-note" style="display:none;color:#8a93a3"><span>Wire tap</span><b>hidden (console VLAN)</b></div>
  <p class="rgrp">SYSTEM</p>
  <div class="kv"><span>Mode</span><b id="i-mode">-</b></div>
  <div class="kv"><span>UART</span><b id="i-uart" title="which serial transport this build got, and what it means for throughput and stability">-</b></div>
  <div class="kv"><span>UART RX buf</span><b id="i-urx" title="uart rx_buffer_size from your YAML -- the ring the loop drains; a full DMX frame is 513 B">-</b></div>
  <div class="kv"><span>UART TX</span><b id="i-utx" title="hardware = 128 B silicon FIFO, non-blocking; software = bit-bang, no buffer">-</b></div>
  <div class="kv"><span>CPU</span><b id="i-mhz" title="runtime clock -- the missing variable for cycle-cost math; an 8266 can run 80 OR 160 MHz on the same binary">-</b></div>
  <div class="kv"><span>Radio brake</span><b id="i-rfloor" title="heap floor under which loopy ingress is dropped (the anti-FIQ-wedge extinguisher). RADIOFLOOR <n> to tune, 0 = OFF (documented wedge risk). Not persisted.">-</b></div>
  <div class="kv"><span>Drain quota</span><b id="i-dq" class="mono">-</b></div>
  <div class="kv"><span>Throttle</span><b id="i-thr">-</b></div>
  <div class="kv"><span>Max pass / win</span><b id="i-mx1" class="mono" title="worst-case loop pass and 1s-window durations since boot (us). If any block ever holds the CPU long enough, the SDK event queue starves -- these gauges catch the culprit red-handed.">-</b></div>
  <div class="kv"><span>Max drain / heapwalk</span><b id="i-mx2" class="mono" title="worst-case wire-drain and umm heap-walk durations (us). The heap walk runs under an interrupt lock and grows with fragmentation -- prime suspect.">-</b></div>
  <div class="kv"><span>Peak (session)</span><b id="i-peak" title="highest tx+rx seen since boot -- the mark to beat">-</b></div>
  <div class="kv"><span>Engine load</span><b id="i-eload" title="duty-pool utilisation over 1 s: how much of the switch's processing budget is in use right now">-</b></div>
  <div class="kv"><span>Headroom est.</span><b id="i-head" title="current throughput / engine load -- a defensible ESTIMATE of what the switch could sustain right now">-</b></div>
  <div class="row" style="margin-top:8px"><input id="cmd-in" placeholder="cmd &gt;  WIRE, ADOPT, RADIOFLOOR, REBOOT..." title="raw control-plane console: the line is sent verbatim to the hub's command parser; replies land in the log pane. Every power now has a handle." style="flex:1;min-width:0"><button class="seg-b" id="cmd-go">Send</button></div>
  <div class="row" style="margin-top:6px"><button class="seg-b" id="sys-reboot" style="border:1px solid #f6c177" title="clean framework reboot (App.safe_reboot): ports and trunks are SAVED; the page reconnects in ~10 s; COM links need their re-click (Web Serial law) -- or an ADOPT.">Reboot hub</button></div>
  <div class="kv"><span>Loop</span><b id="i-loop">-</b></div>
  <div class="kv"><span>Uptime</span><b id="i-up">-</b></div>
</div></div></div>
<script>
function $(i){return document.getElementById(i)}
var ws=null,logs=[],paused=false,filt='',MAXLOG=2000,lastTxUs=null,logSkipped=0,decFilter='',lastDropTime=0,prevDropped=-1,lastInfo=null;
var DEC_EMPTY=$('dec-out')?null:null;
function esc(s){return (s+'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')}
function nowts(){var d=new Date();return d.toTimeString().slice(0,8)+'.'+('00'+d.getMilliseconds()).slice(-3)}
function fmtDelta(us){if(us<1000)return '+'+us+'\u00b5s';if(us<1000000)return '+'+(us/1000).toFixed(1)+'ms';return '+'+(us/1000000).toFixed(2)+'s'}
function hx(n){return ('0'+((n&255).toString(16).toUpperCase())).slice(-2)}
function hexArr(s){if(!s)return[];return s.trim().split(/\s+/).map(function(t){return parseInt(t,16)&255})}
function asciiOf(a){var s='';for(var i=0;i<a.length;i++){var c=a[i];s+=(c>=32&&c<127)?String.fromCharCode(c):'.'}return s}
/* Modbus CRC16 poly A001 reflected -- mirrored in the host tests */
function crc16(a,n){var c=0xFFFF;n=n||a.length;for(var i=0;i<n;i++){c^=a[i];for(var j=0;j<8;j++){c=(c&1)?((c>>1)^0xA001):(c>>1)}}return c}
var MBFN={1:'Read Coils',2:'Read Discrete Inputs',3:'Read Holding Registers',4:'Read Input Registers',5:'Write Single Coil',6:'Write Single Register',15:'Write Multiple Coils',16:'Write Multiple Registers'};
function decModbus(a){ if(a.length<4)return null;
  var c=crc16(a,a.length-2), got=a[a.length-2]|(a[a.length-1]<<8);
  if(c!==got)return null;
  var fn=a[1]&0x7F, ex=(a[1]&0x80)!==0;
  return {addr:a[0],fn:fn,name:MBFN[fn]||('fn '+fn),ex:ex,exCode:ex?a[2]:0,crc:true,data:a.slice(2,a.length-2)}}
function decNmea(a){ var s=asciiOf(a).trim();
  if(s[0]!=='$'||s.indexOf('*')<0)return null;
  var star=s.lastIndexOf('*'),body=s.slice(1,star),want=parseInt(s.slice(star+1,star+3),16);
  var x=0;for(var i=0;i<body.length;i++)x^=body.charCodeAt(i);
  return {sent:body.split(',')[0],ok:x===want,line:s}}
function decDmx(a){ if(!a.length||a[0]!==0)return null; return {n:a.length-1,ch:a.slice(1,33)}}
function isAt(a){var s=asciiOf(a);return /^AT[\s\S]*/.test(s)||/^(OK|ERROR|\+\w+)/.test(s.trim())}
function logMatch(e){if(filt&&e.txt.toLowerCase().indexOf(filt)<0)return false;return true}
function logLineEl(e){var d=document.createElement('div');d.className=e.cls;
  if(e.txn)d.dataset.txn=JSON.stringify(e.txn);
  d.innerHTML='<span class="lts">'+(e.dus!==undefined?fmtDelta(e.dus):e.ts)+'</span>'+esc(e.txt);return d}
var ICO_PAUSE='<svg viewBox="0 0 24 24" fill="currentColor"><rect x="6" y="5" width="4" height="14" rx="1"/><rect x="14" y="5" width="4" height="14" rx="1"/></svg>';
var ICO_PLAY='<svg viewBox="0 0 24 24" fill="currentColor"><path d="M7 5 L19 12 L7 19 Z"/></svg>';
function updPauseBtn(){var b=$('log-pause');
  if(paused){b.innerHTML=ICO_PLAY+(logSkipped>0?'<span style="font-size:11px;margin-left:3px">'+logSkipped+'</span>':'');b.style.width='auto';b.style.padding='0 7px'}
  else{b.innerHTML=ICO_PAUSE;b.style.width='';b.style.padding=''}
  b.className='ib'+(paused?' act':'');b.title=paused?('Resume -- '+logSkipped+' buffered'):'Pause'}
function log(cls,txt,us,txn){var e={cls:cls,txt:txt,ts:nowts()};
  if(typeof us==='number'){e.us=us;if(lastTxUs!==null)e.dus=(us-lastTxUs)>>>0;lastTxUs=us}
  if(txn)e.txn=txn;
  logs.push(e);if(logs.length>MAXLOG)logs.shift();
  if(paused){logSkipped++;updPauseBtn()}
  else if(logMatch(e)){var o=$('out'),at=o.scrollTop+o.clientHeight>=o.scrollHeight-30;
    o.appendChild(logLineEl(e));while(o.childNodes.length>MAXLOG)o.removeChild(o.firstChild);
    if(at)o.scrollTop=o.scrollHeight}}
function renderLog(){var o=$('out'),f=document.createDocumentFragment();
  for(var i=0;i<logs.length;i++)if(logMatch(logs[i]))f.appendChild(logLineEl(logs[i]));
  o.textContent='';o.appendChild(f);o.scrollTop=o.scrollHeight}
function frameLine(m){ var runs=(m.runs||[]).map(function(r){var b=hexArr(r.b);
    return '['+b.map(hx).join(' ')+(r.tot>r.n?' +'+(r.tot-r.n)+'B':'')+']'}).join('');
  var dir=m.dir? '\u2190 RX ':'\u2192 TX ';
  return dir+(m.self?'self ':'')+m.total+'B '+runs+(m.trg?' \u2605':'')}
function decTxnHtml(t){ var m=t.m,a=[]; (m.runs||[]).forEach(function(r){a=a.concat(hexArr(r.b))});
  var h='<div class="dtxn">';
  h+='<div class="d-ts">'+(t.dus!==undefined?fmtDelta(t.dus)+' \u00b7 ':'')+(m.dir?'RX':'TX')+(m.self?' self':'')+' \u00b7 '+m.total+' B'+(m.dur?' \u00b7 '+m.dur+'\u00b5s':'')+(m.trg?' \u00b7 <span style="color:#f6c177">\u2605</span>':'')+'</div>';
  var mb=decModbus(a),nm=decNmea(a),dx=decDmx(a);
  if(mb){ h+='<div class="drow"><span class="dhx '+(m.dir?'d-rx':'d-tx')+'">'+hx(mb.addr)+'</span><span class="dmn">slave addr '+mb.addr+'</span></div>';
    h+='<div class="drow"><span class="dhx">'+hx(a[1])+'</span><span class="dmn">'+(mb.ex?('EXCEPTION on '+mb.name+' \u00b7 code '+mb.exCode):mb.name)+'</span></div>';
    for(var i=0;i<mb.data.length&&i<16;i++)h+='<div class="drow"><span class="dhx">'+hx(mb.data[i])+'</span><span class="dmn">data</span></div>';
    if(mb.data.length>16)h+='<div class="drow"><span class="dhx"></span><span class="dmn">+'+(mb.data.length-16)+' B</span></div>';
    h+='<div class="dsum">Modbus RTU \u00b7 <b style="color:#5dcaa5">CRC16 ok</b></div>';
  } else if(dx&&m.total>24){ var bars='';
    for(var i2=0;i2<dx.ch.length;i2++)bars+='<span class="dmxbar" style="height:'+(2+dx.ch[i2]*24/255)+'px" title="ch'+(i2+1)+'='+dx.ch[i2]+'"></span>';
    h+='<div class="drow"><span class="dhx">00</span><span class="dmn">DMX512 start code</span></div>';
    h+='<div class="drow"><span class="dhx"></span><span class="dmn">'+dx.n+' channels \u00b7 first 32: '+bars+'</span></div>';
    h+='<div class="dsum">DMX512 frame</div>';
  } else if(nm){ h+='<div class="drow"><span class="dhx d-rx">$</span><span class="dmn">'+esc(nm.line.slice(0,70))+'</span></div>';
    h+='<div class="dsum">NMEA '+esc(nm.sent)+' \u00b7 checksum '+(nm.ok?'<b style="color:#5dcaa5">ok</b>':'<b style="color:#e06c75">BAD</b>')+'</div>';
  } else { for(var q=0;q<a.length&&q<16;q++)h+='<div class="drow"><span class="dhx '+(m.dir?'d-rx':'d-tx')+'">'+hx(a[q])+'</span><span class="dmn">'+esc(asciiOf([a[q]]))+'</span></div>';
    if(m.total>a.length)h+='<div class="drow"><span class="dhx"></span><span class="dmn">+'+(m.total-a.length)+' B</span></div>';
    h+='<div class="dsum">'+(isAt(a)?'AT dialog \u00b7 ':'')+'ascii: '+esc(asciiOf(a).slice(0,48))+'</div>'}
  h+='<div class="d-note">framing by '+(m.byDelim?'delimiter':'silence gap')+'</div>';
  return h+'</div>'}
function decRun(){ var o=$('out'),txns=[],prevUs=null;
  Array.prototype.forEach.call(o.children,function(div){ if(!div.dataset.txn)return;
    var m;try{m=JSON.parse(div.dataset.txn)}catch(e){return}
    var t={m:m};
    if(typeof m.us==='number'&&prevUs!==null)t.dus=(m.us-prevUs)>>>0;
    if(typeof m.us==='number')prevUs=m.us;
    txns.push(t)});
  var note='';
  if(txns.length&&Date.now()-lastDropTime<3000)note='<div class="dnote">Link flooded \u2014 frames were dropped; the sequence has gaps. Batch, filter a direction, or ARM.</div>';
  $('dec-out').innerHTML=txns.length?note+txns.map(decTxnHtml).join(''):'<div class="dsum" style="color:var(--dim);padding:12px">No frames in the log to decode.</div>';
  applyDecFilter()}
function applyDecFilter(){var f=decFilter;document.querySelectorAll('#dec-out .dtxn').forEach(function(b){b.style.display=(!f||b.textContent.toLowerCase().indexOf(f)>=0)?'':'none'})}
/* sparklines */
var hTx=[],hRx=[];
function spark(id,h){var c=$(id),x=c.getContext('2d');x.fillStyle='#10151b';x.fillRect(0,0,250,30);
  if(h.length>1){var mx=Math.max.apply(null,h.concat([1]));x.strokeStyle='#5dcaa5';x.beginPath();
    for(var i=0;i<h.length;i++){var px=i*250/Math.max(h.length-1,1),py=28-(h[i]/mx)*26;i?x.lineTo(px,py):x.moveTo(px,py)}x.stroke()}}
/* Keyboard -> the console's VLAN. It is a HUB: a keystroke is broadcast on
   the same path as the Send box (the TX command, injected from the console
   port), so it reaches the wire, the net ports AND any COM bridge sharing
   that vlan -- the switch does the routing, we do not pick a target.
   NO local echo by design: what comes back is the device's own echo, the
   honest one. Keystrokes are coalesced over 30 ms -- fast typing becomes a
   few TX commands instead of one per character (the 8266 parses each one,
   and TX answers with a notice when the topology ate the bytes). */
var rawTx=[],rawTxT=0;
function rawTxFlush(){if(!rawTx.length)return;var h=rawTx.map(hx).join(' ');rawTx=[];send('TX '+h)}
function rawTxByte(b){rawTx.push(b&255);
  if(rawTx.length>=64){if(rawTxT){clearTimeout(rawTxT);rawTxT=0}rawTxFlush();return}  /* TX caps at 128 B */
  if(!rawTxT)rawTxT=setTimeout(function(){rawTxT=0;rawTxFlush()},30)}
/* Local echo is DISPLAY ONLY: it draws the bytes we just sent onto this
   screen and does nothing else. Not one extra byte leaves the browser, the
   hub's loopback is untouched, the serial tx/rx paths are untouched. With a
   device that echoes too, every character shows twice -- that is the point
   of the switch, exactly like PuTTY's. Dumps and pastes are NOT echoed: a
   paced 10 KB transfer would flood a 25-line screen while still going out
   slowly, which would lie about progress. */
var RAW_SEQ={ArrowUp:'A',ArrowDown:'B',ArrowRight:'C',ArrowLeft:'D',Home:'H',End:'F'};
function rawEcho(b){var e=$('raw-echo');if(e&&e.checked)rawFeed(RAW_ECHO,b)}
function rawSeq(s){for(var i=0;i<s.length;i++)rawTxByte(s.charCodeAt(i))}
function rawKey(e){
  var k=e.key;
  if(e.altKey||e.metaKey)return;                    /* Alt / Cmd stay the OS's */
  if(e.ctrlKey){
    if(k.length!==1)return;
    var u=k.toUpperCase().charCodeAt(0);
    if(u<65||u>90)return;
    if(u===67&&window.getSelection().toString()!==''){return}  /* Ctrl+C on a selection: let the browser copy */
    if(u===78||u===84||u===87)return;               /* N/T/W: the browser takes them whatever we do */
    rawTxByte(u-64);e.preventDefault();return}      /* Ctrl+A..Z -> 0x01..0x1A */
  if(k==='Enter'){var eol=$('tx-eol').value,b=[];
    if(eol==='crlf'){b=[13,10]}else if(eol==='cr'){b=[13]}else if(eol==='lf'){b=[10]}
    for(var i=0;i<b.length;i++)rawTxByte(b[i]);
    rawEcho(b);e.preventDefault();return}
  if(k==='Backspace'){rawTxByte(8);rawEcho([8]);e.preventDefault();return}
  if(k==='Tab'){if(e.shiftKey)return;               /* Shift+Tab is the way out of the screen */
    rawTxByte(9);e.preventDefault();return}
  if(k==='Escape'){rawTxByte(27);e.preventDefault();return}
  if(k==='Delete'){rawSeq('\u001b[3~');e.preventDefault();return}
  if(RAW_SEQ[k]){rawSeq('\u001b['+RAW_SEQ[k]);e.preventDefault();return}
  if(k.length===1){var c=k.charCodeAt(0);
    if(c>=32&&c<127){rawTxByte(c);rawEcho([c]);e.preventDefault()}}}
function rawKbdState(on){var e=$('raw-kbd');if(!e)return;
  e.textContent=on?'typing -> console VLAN':'click to type';
  e.style.color=on?'#5dcaa5':''}
/* RAW terminal -- a real 80x25 screen, PuTTY-style: NO scrollback, the
   oldest line falls off the top, so the browser can never be saturated by
   a flood (the cap IS the screen). Fed ONLY by the bridge 0x01 stream --
   bytes the local COM DEVICE sent us. Our own egress is deliberately
   absent: a device with ECHO ON returns it on the wire, and that echo is
   the honest one. One colour per bridge slot; characters from several
   bridges merge into the same screen, exactly as they arrived in time. */
var RAW_COLS=80,RAW_ROWS=25;
var RAW_COL=['#5dcaa5','#f6c177','#7aa2f7','#c792ea'];
var RAW_ECHO=4,RAW_ECHO_COL='#cfd4db',RAW_DATA_COL='#7fc9a4';  /* party-line doctrine: bytes are bytes -- one tint for ALL data (com, bridge, uart, tcp); color is reserved for INSTRUMENTS (lost=red, receipt=blue, local echo=gray) */
var rawLines=[[]],rawCR=false,rawRaf=0;
function rawLen(ln){var n=0;for(var i=0;i<ln.length;i++)n+=ln[i].t.length;return n}
function rawNewline(){rawLines.push([]);while(rawLines.length>RAW_ROWS)rawLines.shift()}
function rawPut(slot,ch){var ln=rawLines[rawLines.length-1],last=ln[ln.length-1];
  if(last&&last.s===slot)last.t+=ch;else ln.push({s:slot,t:ch});
  if(rawLen(ln)>=RAW_COLS)rawNewline()}
/* Paced sender -- file dump / clipboard paste onto the console's VLAN.
   Why pace at all: the wire drains at baud/10 B/s and the uart egress
   buffer is 256 B. Firing a whole file at the hub would overrun it, and
   port_enqueue_ EVICTS THE OLDEST bytes and counts them in drop -- you
   would get a silently corrupted transfer, no error anywhere. So the
   browser sizes each chunk to the wire and watches the live buffer-fill
   telemetry: the ESP is never handed more than it can drain. Bytes go out
   verbatim (binary safe), on the same TX path as typing -- it is a hub,
   so the whole vlan receives them. */
var dumpQ=null,dumpPos=0,dumpT=0,dumpName='',dumpBaud=9600,dumpNote='',rawUartFill=-1;
function dumpBusy(){return dumpQ!==null}
function dumpProgress(){var e=$('raw-prog');if(!e)return;
  if(!dumpQ){e.style.display='none';return}
  e.style.display='';
  $('raw-prog-t').textContent=dumpName+' '+dumpPos+'/'+dumpQ.length+' B'+(rawUartFill>=50?' (buffer full, waiting)':'')+(dumpNote?' -- '+dumpNote:'')}
function dumpStop(msg){if(dumpT){clearTimeout(dumpT);dumpT=0}
  var done=dumpPos,tot=dumpQ?dumpQ.length:0,nm=dumpName;
  dumpQ=null;dumpProgress();
  if(msg)log(done>=tot?'l-ok':'l-err',msg+': '+nm+' -- '+done+'/'+tot+' B')}
function dumpTick(){dumpT=0;
  if(!dumpQ)return;
  if(!ws||ws.readyState!==1){dumpStop('transfer aborted, link down');return}
  /* F9: the pacing lives on lastInfo.baud, and under WS backlog that info
     can be STALE -- a baud drop we have not heard about yet would make us
     over-race the wire and evict the transfer. Hold while telemetry is old;
     when a live LINE change does reach us, repace and SAY so. */
  if(vwStamp&&Date.now()-vwStamp>3000){
    dumpNote='telemetry stale -- holding';dumpProgress();dumpT=setTimeout(dumpTick,250);return}
  dumpNote='';
  if(rawUartFill>=50){dumpProgress();dumpT=setTimeout(dumpTick,120);return}   /* backpressure: let the wire drain */
  var baud=(lastInfo&&lastInfo.baud)||9600;
  if(baud!==dumpBaud){dumpBaud=baud;log('l-sys','transfer repaced to '+baud+' baud')}
  var rate=Math.max(120,Math.floor(baud/10*0.8));        /* stay under the wire, never race it */
  var chunk=Math.min(96,dumpQ.length-dumpPos);           /* the TX command caps at 128 B */
  var h=[];for(var i=0;i<chunk;i++)h.push(hx(dumpQ[dumpPos+i]));
  send('TX '+h.join(' '));
  dumpPos+=chunk;dumpProgress();
  if(dumpPos>=dumpQ.length){dumpStop('transfer complete');txRcptArm();return}
  dumpT=setTimeout(dumpTick,Math.max(30,Math.ceil(chunk*1000/rate)))}
function dumpStart(bytes,name){
  if(dumpBusy()){log('l-err','a transfer is already running');return}
  if(!bytes||!bytes.length){log('l-err','nothing to send');return}
  txRcptStart('transfer '+(name||'clipboard'),true);
  dumpQ=bytes;dumpPos=0;dumpName=name||'clipboard';
  dumpBaud=(lastInfo&&lastInfo.baud)||9600;dumpNote='';
  log('l-sys','sending '+dumpName+' ('+bytes.length+' B) onto the console VLAN');
  dumpTick()}
function dumpText(t,name){if(!t)return;dumpStart(new TextEncoder().encode(t),name)}
function rawBack(){var ln=rawLines[rawLines.length-1];
  while(ln.length&&!ln[ln.length-1].t.length)ln.pop();
  if(!ln.length)return;                          /* start of line: a real terminal stops here */
  var seg=ln[ln.length-1];seg.t=seg.t.slice(0,-1);
  if(!seg.t.length)ln.pop()}
function rawClear(){rawLines=[[]];rawCR=false;rawRender()}
var vlanLost=0;
function rawMark(tag,txt){rawNewline();for(var i=0;i<txt.length;i++)rawPut(tag,txt[i]);rawNewline();
  if(!rawRaf)rawRaf=requestAnimationFrame(rawRender)}
function rawTruth(){var e=$('raw-vlan');if(!e)return;var cv='?';
  if(lastInfo&&lastInfo.ports)lastInfo.ports.forEach(function(p){if(p.type==='console')cv=p.vlan});
  e.textContent='vlan '+cv+' raw -- '+(vlanLost>0?('LOST '+vlanLost+' B (console buffer)'):'in-sync');
  e.style.color=vlanLost>0?'#e06c75':'#5dcaa5'}
var txRcpt=null;
function txRcptStart(label,defer){if(!lastInfo||!lastInfo.ports)return;var cv=-1;
  lastInfo.ports.forEach(function(p){if(p.type==='console')cv=p.vlan});
  var map={};lastInfo.ports.forEach(function(p){
    if(p.vlan===cv&&p.up&&p.type!=='console')map[p.id]={tx:p.tx||0,drop:p.drop||0,nm:(p.type[0]||'p')+p.id}});
  txRcpt={label:label,map:map,armed:!defer,seen:0}}
function txRcptArm(){if(txRcpt&&!txRcpt.armed){txRcpt.armed=true;txRcpt.seen=0}}
function txRcptTick(m){if(!txRcpt||!txRcpt.armed||!m.ports)return;
  txRcpt.seen++;var parts=[],sum=0;
  m.ports.forEach(function(p){var b=txRcpt.map[p.id];if(!b)return;
    var dt=(p.tx||0)-b.tx,dd=(p.drop||0)-b.drop;sum+=dt;
    parts.push(b.nm+' +'+dt+(dd>0?(' (drop +'+dd+')'):''))});
  if(sum>0||txRcpt.seen>=2){
    rawMark('R','delivered['+txRcpt.label+'] -> '+(parts.length?parts.join(' | '):'no vlan members'));
    txRcpt=null}}
function vlanFeed(src,bytes){
  if(src===0xFF&&bytes.length>=4){var g=bytes[0]|(bytes[1]<<8)|(bytes[2]<<16)|(bytes[3]<<24);
    vlanLost+=g;rawMark('L','[lost '+g+' B]');rawTruth();return}
  rawFeed(100+src,bytes);rawTruth()}
function rawFeed(slot,bytes){
  for(var i=0;i<bytes.length;i++){var b=bytes[i];
    if(b===13){rawNewline();rawCR=true;continue}                 /* CR closes the line */
    if(b===10){if(rawCR){rawCR=false;continue}rawNewline();continue} /* LF right after CR: same break */
    rawCR=false;
    if(b===8||b===127){rawBack();continue}                       /* BS / DEL erase, like a terminal */
    rawPut(slot,(b>=32&&b<127)?String.fromCharCode(b):'.')}      /* same rule as asciiOf */
  if(!rawRaf)rawRaf=requestAnimationFrame(rawRender)}            /* coalesce: at most one paint per frame */
function rawRender(){rawRaf=0;var o=$('raw-out');if(!o)return;
  var f=document.createDocumentFragment();
  for(var i=0;i<rawLines.length;i++){var d=document.createElement('div'),ln=rawLines[i];
    for(var j=0;j<ln.length;j++){var sp=document.createElement('span');
      sp.style.color=(ln[j].s===RAW_ECHO)?RAW_ECHO_COL:(ln[j].s==='L')?'#e06c75':(ln[j].s==='R')?'#7aa2f7':RAW_DATA_COL;sp.textContent=ln[j].t;d.appendChild(sp)}
    if(i===rawLines.length-1){var cur=document.createElement('span');cur.className='raw-cur';d.appendChild(cur)}
    f.appendChild(d)}
  o.textContent='';o.appendChild(f)}
/* Tabs are SHOW/HIDE only -- nothing is torn down or rebuilt. The raw
   screen keeps filling, the log keeps accumulating, a paced transfer keeps
   going: switching tabs costs no state. Coming back to General repaints the
   screen, which had zero height while hidden. */
/* ===== VIEWS tab =====
   Everything below is computed CLIENT-SIDE from data the hub already pushes
   (info on-change + 2 s, bs 4 Hz, and the log buffer already in RAM). No new
   backend channel, and NO external library: this page lives in PROGMEM and
   must work on a LAN with no internet, so every chart is hand-rolled canvas
   like spark(). Panels repaint only while the tab is visible; the
   accumulators keep feeding in the background (the MAXLOG pattern).
   LIVE MODEL implements the reconciled storm model:
     D_meas = swr / I            (I = UP hairpins of the dominant vlan)
     D_pred = min(bufcap, quota) * 1e6 / loop_us    (the bottleneck law)
     link = (I+2)*D   switched = I*D   deliveries = I*M*D   evict = (M-1)/M
   The math is MIRRORED in tests/host/test_ui.js -- change one, change both.
   Honesty rules: a stale info (WS drops can eat pushes under storm) is
   disclosed by the age stamp; no bench means "no bench", never NaN; and a
   big model gap has three known causes the badge tells apart: a real bug,
   budget-broken serving (the pass budget skips ports), or an OSCILLATING
   governor -- the quota snapshot then overstates the mean, and the recent
   throttle ticks in vwHist.ev are the witness. */
var VW_CAP=150,vwHist={dq:[],ev:[]},vwLastThr=-1,vwStamp=0,vwSel=-1,vwNodes=[],vwHeapMax=0,vwLastDraw=0,vwKickT=0,vwPortCache={},vwBB='';
var VW_VCOL=['#5dcaa5','#f6c177','#7aa2f7','#c792ea','#ed93b1','#97c459','#f0997b','#d3d1c7'];
function viewsOn(){var e=$('tab-views');return !!e&&e.style.display!=='none'}
function vwFmt(n){return Math.round(n).toString().replace(/\B(?=(\d{3})+(?!\d))/g,' ')}
function vwFeed(m){vwStamp=Date.now();
  if(typeof m.dq==='number'){var ev=0;
    if(typeof m.thr==='number'){if(vwLastThr>=0&&m.thr>vwLastThr)ev=(m.thc|0)||3;vwLastThr=m.thr}
    vwHist.dq.push(m.dq);vwHist.ev.push(ev);
    if(vwHist.dq.length>VW_CAP){vwHist.dq.shift();vwHist.ev.shift()}}
  vwKick()}
/* Flood hardening: under storm the hub pushes info fast, and repainting all
   7 panels per message is the only NEW per-message work the Views tab ever
   added to the load path. The FEED path is therefore coalesced to <= 4 Hz;
   tab switches still render immediately (tabShow stamps the clock and calls
   vwRender directly). */
function vwKick(){if(!viewsOn())return;
  var now=Date.now();
  if(now-vwLastDraw>=250){vwLastDraw=now;vwRender();return}
  if(!vwKickT)vwKickT=setTimeout(function(){vwKickT=0;
    if(viewsOn()){vwLastDraw=Date.now();vwRender()}},260)}
function vwEndur(m){var e=$('vw-en-up');if(!e||!m)return;
  var up=m.up||0,hh=Math.floor(up/3600),mm=Math.floor(up%3600/60);
  e.textContent=up>0?(hh+'h '+mm+'m'):'-';
  var rt=function(x){return up>0?Math.round((x||0)*3600/up):0};
  $('vw-en-th').textContent=rt(m.thr);$('vw-en-ws').textContent=rt(m.wsdrop);$('vw-en-dp').textContent=rt(m.dropped);
  $('vw-en-bb').textContent='black box: '+(vwBB||'clean boot');
  $('vw-en-lg').textContent='largest now '+(m.largest||0)+' B -- frag '+(m.frag||0)+' %'}
function vwModel(m){var byv={},uartUp=false;
  (m.ports||[]).forEach(function(p){if(!p.up)return;
    if(p.type==='uart')uartUp=true;
    var v=byv[p.vlan]||(byv[p.vlan]={I:0,M:0,buf:0});v.M++;
    if(p.type==='bridge'&&p.wire===p.id){v.I++;if(p.bufcap)v.buf=p.bufcap}});
  var best=null,bi=0;
  for(var k in byv)if(byv[k].I>bi){bi=byv[k].I;best=k}
  if(best===null)return null;
  var g=byv[best],others=false,sumI=0;
  for(var k2 in byv){sumI+=byv[k2].I;if(k2!==best&&byv[k2].I>0)others=true}
  var D=sumI>0?(m.swr||0)/sumI:0;
  var buf=g.buf||1024,q=(typeof m.dq==='number'&&m.dq>0)?m.dq:64;
  var pred=(m.loop>0)?Math.min(buf,q)*1e6/m.loop:0;
  return {vlan:best,I:g.I,sumI:sumI,M:g.M,bufcap:buf,D:D,pred:pred,link:(g.I+2)*D,
    gk:buf<=q?('buffer ('+buf+' B)'):('quota ('+q+' B)'),
    ecart:pred>0?Math.abs(pred-D)/Math.max(pred,1):1,
    approx:others,uartUp:uartUp}}
function vwModelRender(m){var r=vwModel(m),b=$('vw-badge');if(!b)return;
  if(!r||r.I===0||r.D<=0){$('vw-d').textContent='-';$('vw-pd').textContent='-';$('vw-pl').textContent='-';
    b.textContent='no active bench (no UP hairpin is injecting)';b.style.color='';
    $('vw-gk').textContent='';var ce0=$('vw-cost');if(ce0)ce0.textContent='';vwFlow(null);return}
  $('vw-d').textContent=vwFmt(r.D)+' B/s';
  $('vw-pd').textContent=vwFmt(r.pred)+' B/s';
  $('vw-pl').textContent=vwFmt(r.link)+' B/s';
  var pc=Math.round(r.ecart*100);
  var osc=0;for(var oi=Math.max(0,vwHist.ev.length-12);oi<vwHist.ev.length;oi++)if(vwHist.ev[oi])osc++;
  if(pc<2){b.textContent='model gap '+pc+' % -- healthy';b.style.color='#5dcaa5'}
  else if(pc<10){b.textContent='model gap '+pc+' % -- watch';b.style.color='#f6c177'}
  else if(osc>=3){b.textContent='model gap '+pc+' % -- governor oscillating ('+osc+' throttle hits in the window): the quota snapshot '+(r.D>r.pred?'understates':'overstates')+' the mean';b.style.color='#f6c177'}
  else{b.textContent='model gap '+pc+' % -- breach: bug, or budget-broken serving';b.style.color='#e06c75'}
  if(r.approx)b.textContent+=' -- multi-vlan exact (Sigma I='+r.sumI+')';
  $('vw-gk').textContent='bottleneck: '+r.gk+' -- vlan '+r.vlan+' -- I='+r.I+' M='+r.M+(r.uartUp?' -- uart UP':'');
  var ce=$('vw-cost');
  if(ce){var cyc=(m.swr>0&&m.mhz>0&&typeof m.eload==='number')?(m.eload*m.mhz*1e4/m.swr):0,
    sv=r.pred>0?(r.D/r.pred):0;
    ce.textContent=(cyc>0?('cost '+(cyc>=100?Math.round(cyc):cyc.toFixed(1))+' cyc/B on the engine core ('+m.mhz+' MHz)'):'cost -')+
      ' -- service D/D_pred '+sv.toFixed(2)}
  vwFlow(r)}
function vwFlow(r){var v=$('vw-fl-vlan');if(!v)return;
  if(!r){v.textContent='no active bench';
    ['vw-fl-i','vw-fl-d','vw-fl-e','vw-fl-r'].forEach(function(i){$(i).textContent='-'});
    ['vw-fl-ib','vw-fl-db','vw-fl-eb','vw-fl-rb'].forEach(function(i){$(i).style.width='0'});return}
  var inj=r.I*r.D,del=r.I*r.M*r.D,evp=r.M>0?(r.M-1)/r.M:0;
  v.textContent='vlan '+r.vlan+' -- x'+r.M+' amplification';
  $('vw-fl-i').textContent=vwFmt(inj)+' B/s';
  $('vw-fl-ib').style.width=(del>0?Math.min(100,Math.round(inj*100/del)):0)+'%';
  $('vw-fl-d').textContent=vwFmt(del)+' B/s';$('vw-fl-db').style.width='100%';
  $('vw-fl-e').textContent=Math.round(evp*100)+' % ('+vwFmt(del-inj)+' B/s)';
  $('vw-fl-r').textContent=vwFmt(inj)+' B/s';
  $('vw-fl-eb').style.width=Math.round(evp*100)+'%';
  $('vw-fl-rb').style.width=(100-Math.round(evp*100))+'%'}
function vwGovY(v,H){return H-14-((v-64)/(2048-64))*(H-26)}
function vwGov(){var c=$('vw-gov');if(!c||!c.getContext)return;
  var x=c.getContext('2d'),W=c.width,H=c.height;
  x.fillStyle='#10151b';x.fillRect(0,0,W,H);
  /* Labeled rails (esp32 field: a plateau pinned at max over an unlabeled
     void READS as a glitch; against a named rail it reads as health). */
  var yT=vwGovY(2048,H),yB=vwGovY(64,H);
  x.strokeStyle='#2a3644';x.setLineDash([3,3]);
  x.beginPath();x.moveTo(4,yT);x.lineTo(W-32,yT);x.stroke();
  x.beginPath();x.moveTo(4,yB);x.lineTo(W-32,yB);x.stroke();
  x.setLineDash([]);
  x.fillStyle='#5a6a7a';x.font='9px monospace';
  x.fillText('2048',W-29,yT+3);x.fillText('64',W-22,yB+3);
  var h=vwHist.dq;if(h.length<2)return;
  x.strokeStyle='#5dcaa5';x.beginPath();
  for(var i=0;i<h.length;i++){var px=4+i*(W-8)/Math.max(VW_CAP-1,1),
    py=vwGovY(h[i],H);
    if(i)x.lineTo(px,py);else x.moveTo(px,py)}
  x.stroke();
  for(var j=0;j<h.length;j++)if(vwHist.ev[j]){
    x.fillStyle=vwHist.ev[j]===3?'#e06c75':(vwHist.ev[j]===2?'#f6c177':'#7aa2f7');
    x.fillRect(3+j*(W-8)/Math.max(VW_CAP-1,1),H-10,2,7)}
  var n=$('vw-gov-note');
  if(n&&lastInfo)n.textContent='quota '+lastInfo.dq+' B/pass -- throttle '+lastInfo.thr+' -- ticks: red backlog, amber duty, blue heap'}
function vwGaps(){var c=$('vw-gaps');if(!c||!c.getContext)return;
  var x=c.getContext('2d'),W=c.width,H=c.height;
  x.fillStyle='#10151b';x.fillRect(0,0,W,H);
  var ed=[1000,3000,10000,30000,100000,300000,1000000];
  var lab=['<1ms','3ms','10ms','30ms','.1s','.3s','1s','>1s'];
  var b=[0,0,0,0,0,0,0,0],n=0;
  for(var i=0;i<logs.length;i++){var e=logs[i];
    if(!e.txn||typeof e.dus!=='number')continue;n++;
    var k=0;while(k<7&&e.dus>=ed[k])k++;b[k]++}
  var note=$('vw-gaps-note');
  if(!n){if(note){var ud=lastInfo&&(lastInfo.ports||[]).some(function(p2){return p2.type==='uart'&&p2.up});
    note.textContent=ud?'no framed traffic in the log buffer yet':'no framed traffic -- the tap only sees the WIRE, and the uart is DOWN'}return}
  var mx=Math.max.apply(null,b.concat([1])),bw=(W-16)/8;
  for(var j2=0;j2<8;j2++){var bh=(b[j2]/mx)*(H-30);
    x.fillStyle='#5dcaa5';x.fillRect(8+j2*bw+3,H-16-bh,bw-6,bh);
    x.fillStyle='#5a6272';x.fillText(lab[j2],8+j2*bw+3,H-4)}
  if(note)note.textContent=n+' frames sampled -- two modes = a request/response signature'}
function vwMem(m){var f=$('vw-mm-f');if(!f)return;
  /* Scale anchored on the session heap MAX (floor 40960): the floor markers
     stay put while the fill moves -- a dial, not a drifting ratio. The
     session anchor (not a constant) keeps the gauge honest on ESP32 too. */
  var heap=m.heap||1;
  vwHeapMax=Math.max(vwHeapMax,heap,40960);var sc=vwHeapMax;
  f.style.width=Math.min(100,Math.round((m.largest||0)*100/sc))+'%';
  var mk=function(id,v){var e=$(id);if(e)e.style.left=Math.min(99,v*100/sc).toFixed(1)+'%'};
  mk('vw-mm-g',1536);mk('vw-mm-r',m.rfloor||0);mk('vw-mm-o',m.floor||0);
  $('vw-mm-rl').textContent=(m.rfloor===0)?'OFF':vwFmt(m.rfloor)+' B';
  $('vw-mm-fl').textContent=vwFmt(m.floor||0)+' B';
  $('vw-mm-note').textContent='free '+vwFmt(heap)+' B -- largest '+vwFmt(m.largest||0)+' B -- frag '+(m.frag||0)+' % -- port buffers '+vwFmt(m.bufs||0)+' B -- scale 0..'+vwFmt(sc)+' B'}
function vwTopo(m){var c=$('vw-topo');if(!c||!c.getContext)return;
  var x=c.getContext('2d'),W=c.width,H=c.height;
  x.fillStyle='#10151b';x.fillRect(0,0,W,H);
  var ps=m.ports||[];vwNodes=[];
  if(!ps.length)return;
  var byv={};ps.forEach(function(p){(byv[p.vlan]=byv[p.vlan]||[]).push(p)});
  var vlans=Object.keys(byv).sort(function(a,b2){return a-b2});
  var cx=W/2,cy=H/2+4,R=Math.min(W,H)/2-36,a=0,tot=ps.length;
  vlans.forEach(function(v){var g=byv[v],span=g.length/tot*2*Math.PI;
    x.strokeStyle=VW_VCOL[((v|0)-1)&7];x.setLineDash([4,4]);
    x.beginPath();x.arc(cx,cy,R+16,a+0.1,a+span-0.1);x.stroke();x.setLineDash([]);
    x.fillStyle=VW_VCOL[((v|0)-1)&7];
    x.fillText('v'+v,cx+(R+27)*Math.cos(a+span/2)-6,cy+(R+27)*Math.sin(a+span/2)+3);
    g.forEach(function(p,i){var an=a+(i+0.5)*span/g.length;
      vwNodes.push({p:p,x:cx+R*Math.cos(an),y:cy+R*Math.sin(an),a:an})});
    a+=span});
  vwNodes.forEach(function(n){var p=n.p;
    if(p.type==='bridge'&&p.wire>=0&&p.wire!==p.id&&p.wire>p.id){
      var q=null;vwNodes.forEach(function(n2){if(n2.p.id===p.wire)q=n2});
      if(q){x.strokeStyle='#5dcaa5';x.beginPath();x.moveTo(n.x,n.y);x.lineTo(q.x,q.y);x.stroke()}}});
  var sel=null;vwNodes.forEach(function(n){if(n.p.id===vwSel)sel=n});
  vwNodes.forEach(function(n){var p=n.p;
    var inBlast=sel&&p.up&&p.id!==vwSel&&(p.vlan===sel.p.vlan||p.id===sel.p.wire);
    x.beginPath();x.arc(n.x,n.y,10,0,2*Math.PI);
    x.fillStyle=sel?((inBlast||p.id===vwSel)?'#1d2a38':'#141920'):'#232b36';x.fill();
    x.strokeStyle=p.lp?'#e06c75':(p.xlp?'#f6c177':(p.up?VW_VCOL[((p.vlan|0)-1)&7]:'#3a4350'));
    x.stroke();
    if(p.type==='bridge'&&p.wire===p.id){x.beginPath();
      x.arc(n.x+Math.cos(n.a)*15,n.y+Math.sin(n.a)*15,6,0.3,5.6);x.stroke()}
    x.fillStyle=p.up?'#cfd4db':'#5a6272';
    x.fillText(p.type.charAt(0)+p.id,n.x-7,n.y+3)});
  var note=$('vw-topo-note');
  if(sel&&note){var reach=0;
    vwNodes.forEach(function(n){if(n.p.up&&n.p.id!==vwSel&&(n.p.vlan===sel.p.vlan||n.p.id===sel.p.wire))reach++});
    note.textContent='a byte from '+sel.p.type+' '+vwSel+' reaches '+reach+' port(s) -- 1 hop = 1 pass ('+(lastInfo?lastInfo.loop:'-')+' us) -- click empty space to clear'}
  else if(note)note.textContent='red ring = LOOP badge -- amber = XVLAN -- self-arc = hairpin -- chord = wire -- dim = DOWN'}
function vwWhat(){var wi=$('vw-wi');if(!wi)return;
  var I=+wi.value,q=+$('vw-wq').value;
  $('vw-wio').textContent=I;$('vw-wqo').textContent=q;
  var lp=(lastInfo&&lastInfo.loop>0)?lastInfo.loop:20000,buf=1024;
  if(lastInfo){var r=vwModel(lastInfo);if(r&&r.bufcap)buf=r.bufcap}
  var D=Math.min(buf,q)*1e6/lp;
  $('vw-wr').textContent='-> D '+vwFmt(D)+' -- link '+vwFmt((I+2)*D)+' B/s'}
function vwFunnel(m){var o=$('vw-fn-o');if(!o)return;
  var dt=m.dth|0,dh=m.dhp|0,db=m.dbk|0,shown=m.obs|0,off=shown+dt+dh+db;
  if(!off){o.textContent='-';
    ['vw-fn-t','vw-fn-h','vw-fn-b','vw-fn-s'].forEach(function(i){$(i).textContent='-'});
    ['vw-fn-ob','vw-fn-tb','vw-fn-hb','vw-fn-bb','vw-fn-sb'].forEach(function(i){$(i).style.width='0'});
    $('vw-fn-note').textContent='no tap traffic yet';return}
  o.textContent=vwFmt(off)+' frames';$('vw-fn-ob').style.width='100%';
  $('vw-fn-t').textContent=vwFmt(dt);$('vw-fn-h').textContent=vwFmt(dh);$('vw-fn-b').textContent=vwFmt(db);
  $('vw-fn-s').textContent=vwFmt(shown)+' ('+Math.round(shown*100/off)+' %)';
  var w2=function(v){return Math.round(v*100/off)+'%'};
  $('vw-fn-tb').style.width=w2(dt);$('vw-fn-hb').style.width=w2(dh);
  $('vw-fn-bb').style.width=w2(db);$('vw-fn-sb').style.width=w2(shown);
  $('vw-fn-note').textContent='-- the wire flow is NEVER touched: only the reporting is'}
function vwRender(){if(!lastInfo)return;
  vwTopo(lastInfo);vwGov();vwModelRender(lastInfo);vwGaps();vwMem(lastInfo);vwFunnel(lastInfo);vwEndur(lastInfo);vwWhat()}
function tabShow(n){['general','uart','views'].forEach(function(t){
    $('tab-'+t).style.display=(t===n)?'':'none';
    $('tb-'+t).className='tb'+(t===n?' on':'')});
  if(n==='general')rawRender();
  if(n==='views'){vwLastDraw=Date.now();vwRender()}}
function rawLegend(){var e=$('raw-legend');if(!e)return;
  if(!pcConns.length){e.innerHTML='<span class="hint">no bridge connected</span>';return}
  e.innerHTML=pcConns.map(function(c){
    return '<span style="color:'+RAW_COL[c.slot&3]+'">'+esc(pcLabel(c))+'</span>'}).join('<span class="hint"> | </span>')}
/* Web Serial bridge -- up to 4 local ports, one hub bridge port each.
   Both WS directions carry the bridge SLOT id: 0x01 slot data (PC->hub),
   0x02 slot data (hub->PC). pcPending holds a freshly-opened COM port until
   the hub's {"t":"bridge","pi","slot"} reply binds it. */
var pcConns=[],pcPending=null,pcSeq=0;
function pcBadge(){$('p-pc').style.display=pcConns.length?'':'none';
  $('pc-st').textContent=pcConns.length?(pcConns.length+' port'+(pcConns.length>1?'s':'')+' bridged'):'no port connected';
  $('pc-conn').disabled=pcConns.length>=4;}
/* The browser NEVER reveals the OS port name (COM7, ttyUSB0) -- Web
   Serial privacy design. The only identity available is the USB
   vendor/product id; virtual ports (VCOM, com0com) have none at all.
   So: name links by USB chip, never by a fake COM number. */
function chipName(v){return {6790:'CH340',1027:'FTDI',4292:'CP210x',1659:'Prolific',9025:'Arduino',12346:'Espressif'}[v]||null}
function pcLabel(c){var id=c.vid?((chipName(c.vid)||'USB')+' '+c.vid.toString(16).padStart(4,'0')+':'+c.pid.toString(16).padStart(4,'0')):'virtual/unnamed port';
  return 'link '+c.n+' \u00b7 '+id}
function pcList(){var h='';pcConns.forEach(function(c){
    h+='<div class="row"><span class="mono" style="min-width:120px" title="the browser hides the real COM name (Web Serial privacy); USB ids are the only identity it exposes">'+pcLabel(c)+' \u00b7 slot '+c.slot+' \u00b7 hub port '+c.pi+'</span>'
      +'<span class="hint">'+c.baud+' baud</span>'
      +'<button class="seg-b pc-relink" data-slot="'+c.slot+'" title="re-attach this open COM link to an existing slot-less bridge port (after a hub reboot, instead of delete-and-recreate)">Re-link</button>'
      +'<button class="seg-b pc-del" data-slot="'+c.slot+'">Disconnect</button></div>'});
  $('pc-list').innerHTML=h;pcBadge();rawLegend()}
async function pcDrop(c,tellHub){ if(c.closing)return; c.closing=true;
  pcConns=pcConns.filter(function(x){return x!==c});
  if(tellHub&&c.pi>=0)send('PORT DEL '+c.pi);  // no more orphaned bridge ports
  pcList();
  try{if(c.reader)await c.reader.cancel()}catch(e){}   // read() resolves done, lock releases in the loop
  try{if(c.writer)c.writer.releaseLock()}catch(e){}
  try{await c.port.close()}catch(e){}}                 // now legal: no lock held
async function pcConnect(){
  if(!navigator.serial){$('pc-st').textContent='Web Serial API unavailable (Chrome + secure-origin flag needed)';return}
  if(pcConns.length>=4){$('pc-st').textContent='4 bridges max';return}
  if(pcPending){$('pc-st').textContent='previous connect still binding...';return}
  try{ var port=await navigator.serial.requestPort();
    var baud=parseInt($('pc-baud').value,10)||115200;
    await port.open({baudRate:baud});
    var gi=(port.getInfo&&port.getInfo())||{};
    pcPending={port:port,writer:port.writable.getWriter(),reader:null,closing:false,
               slot:-1,pi:-1,baud:baud,n:++pcSeq,vid:gi.usbVendorId||0,pid:gi.usbProductId||0};
    send('PORT ADD BRIDGE');   // the t:"bridge" reply completes the binding
    setTimeout(function(){if(pcPending&&pcPending.slot<0){var c=pcPending;pcPending=null;pcDrop(c,false);
      $('pc-st').textContent='hub did not assign a bridge slot'}},3000);
  }catch(e){$('pc-st').textContent='failed: '+e.message}}
function pcBind(m){ if(!pcPending)return; var c=pcPending;pcPending=null;
  c.slot=m.slot;c.pi=m.pi;
  if(c.relink){c.relink=false;pcList();$('pc-st').textContent='re-linked to hub port '+m.pi;return}
  pcConns.push(c);pcList();
  (async function(){
    try{ c.reader=c.port.readable.getReader();
      while(true){ var r=await c.reader.read(); if(r.done)break;
        if(r.value&&r.value.length){
          if(ws&&ws.readyState===1){
            var b=new Uint8Array(r.value.length+2);b[0]=0x01;b[1]=c.slot;b.set(r.value,2);ws.send(b)}}}
    }catch(e){}
    try{c.reader.releaseLock()}catch(e){}
    if(!c.closing)pcDrop(c,true);  // unplugged: clean the hub side too
  })()}
async function pcWrite(slot,bytes){ for(var i=0;i<pcConns.length;i++){var c=pcConns[i];
    if(c.slot===slot){try{await c.writer.write(bytes)}catch(e){}return}}}
/* ws */
function send(t){if(ws&&ws.readyState===1){ws.send(t);}else log('l-err','not connected')}
function onMsg(m){
  if(m.t==='frame'){log((m.dir?'l-rx':'l-tx'),frameLine(m),m.us,m);return}
  if(m.t==='fbatch'){(m.txns||[]).forEach(function(e){log(e.dir?'l-rxd':'l-txd',(e.dir?'\u2190 RX ':'\u2192 TX ')+e.total+'B \u00d7'+e.count,e.us,e)});return}
  if(m.t==='info'){updInfo(m);vwFeed(m);return}
  if(m.t==='cons'){var a=hexArr(m.b);log('l-rx','\u2192 console \u00b7 from '+m.src+' \u00b7 '+m.n+'B ['+m.b+(m.n>a.length?' +'+(m.n-a.length)+'B':'')+'] '+asciiOf(a));return}
  if(m.t==='bs'){rawUartFill=-1;
    if(lastInfo&&lastInfo.ports)lastInfo.ports.forEach(function(pp){
      if(pp.type==='uart'&&m.o&&typeof m.o[pp.id]==='number')rawUartFill=m.o[pp.id]});
    (m.o||[]).forEach(function(v,i){var el=document.getElementById('b-o'+i);
      if(el&&v>=0){el.style.width=v+'%';el.className=v>=90?'hot':'';}});
    (m.i||[]).forEach(function(v,i){var el=document.getElementById('b-i'+i);
      if(el&&v>=0){el.style.width=v+'%';el.className=v>=90?'hot':'';}});
    return}
  if(m.t==='bridge'){pcBind(m);log('l-ok','bridge slot '+m.slot+' bound (port '+m.pi+')');return}
  if(m.t==='pdel'){var z=pcConns.filter(function(c){return c.pi===m.pi});
    z.forEach(function(c){pcDrop(c,false)});
    if(z.length)$('pc-st').textContent='hub port '+m.pi+' deleted -- link closed (no more zombies)';
    return}
  if(m.t==='err'){log('l-err',m.msg);return}
  if(m.t==='ok'){log('l-ok',m.msg);return}
  if(m.t==='sys'){if(m.msg&&m.msg.indexOf('BLACK BOX')===0)vwBB=m.msg;log('l-sys',m.msg);return}
}
function guardCheck(m){rawTruth();var g=$('buf-guard');if(!m){g.textContent='';return}
  var b=parseInt($('new-buf').value,10)||0;
  if(b>m.bufmax){g.textContent='max '+m.bufmax+' B on this platform';g.style.color='#f6c177';return}
  var ok=(m.largest>=b)&&((m.largest-b)>=m.floor);
  g.textContent=b?(ok?'fits ('+((m.largest-b)/1024).toFixed(1)+'k left)':'would break the floor'):'';
  g.style.color=ok?'#5dcaa5':'#e06c75'}
function portTitle(p){return p.type+(p.np?' :'+p.np:'')}
function renderPorts(m){var ps=m.ports||[],h='';
  $('port-count').textContent='-- '+ps.length+' active';
  ps.forEach(function(p){
    h+='<div class="row" data-id="'+p.id+'">'
      +'<span class="mono" style="min-width:96px"'+(p.bufcap?' title="egress buffer '+p.bufcap+' B"':'')+'>'+p.id+' \u00b7 '+esc(portTitle(p))+'</span>'
      +'<button class="seg-b p-vlan" title="click to cycle the VLAN (1-8)">VLAN '+p.vlan+'</button>'
      +'<button class="seg-b p-updn'+(p.up?' on':'')+'">'+(p.up?'UP':'DOWN')+'</button>'
      +'<button class="seg-b p-rate'+(p.rate?' on':'')+'" title="storm control: ingress cap in bytes/s (0 = unlimited); click to change">rate '+(p.rate?p.rate+'B/s':'\u221e')+'</button>'
      +'<button class="seg-b p-orate'+(p.orate?' on':'')+'" title="egress shaper: this port TRANSMITS at most N bytes/s (0 = unlimited); excess overflows its buffer and is counted in drop -- the mismatched-speed switch demo">out '+(p.orate?p.orate+'B/s':'\u221e')+'</button>'
      +(p.type==='bridge'?'<button class="seg-b p-wire'+(p.wire>=0?' on':'')+'" title="TRUNK: virtual RAM wire to another slot-less bridge (a null-modem inside the hub). Wire two bridges in DIFFERENT vlans and you have a shaped, counted, DECLARED inter-VLAN trunk -- the out pill is its baud. Click to wire/unwire.">'+(p.wire===p.id?'wire\u21a9':(p.wire>=0?'wire\u21c4'+p.wire:'wire \u21c4'))+'</button>':'')
      +(p.conn?'<span class="hint" style="color:#5dcaa5">\u25cf linked</span>':'')
      +((p.rate&&p.drop)?'<span style="color:#e06c75" title="storm control tripping">\u25cf</span>':'')
      +(p.lp?'<span style="color:#e06c75;font-weight:600" title="loop detected: this port echoes the hub&#39;s own egress">\u27f2 LOOP</span>':'')
      +(p.xlp?'<span style="color:#f6c177;font-weight:600" title="inter-VLAN bridge: this port&#39;s ingress matches hub egress from another vlan">\u21c4 XVLAN</span>':'')
      +(p.wire===p.id?'<span style="color:#56c8d8;font-weight:600" title="LOOPBACK (hairpin): this port&#39;s egress re-enters its own ingress via the governed drain -- the integrated loopback plug. The out pill is the oscillator&#39;s period; rate governs its admission; the LOOP badge will light and that is the witness telling the truth about a mirror.">\u21a9 LOOPBACK</span>':(p.wire>=0?'<span style="color:#5dcaa5;font-weight:600" title="declared trunk crossing: XVLAN detection EXEMPTS this wire by design -- clandestine inter-VLAN paths are still hunted. Loop detect stays fully armed around the trunk.">\u21c4 XVLAN ALLOWED \u00b7 wired to '+p.wire+'</span>':''))
      +(p.iso?'<span style="color:#8a93a3" title="alone in its VLAN: everything this port sends or receives dies at the switch -- add a port to VLAN '+p.vlan+' or move this one">\u2205 alone in VLAN '+p.vlan+'</span>':'')
      +'<span class="hint mono">tx '+p.tx+' rx '+p.rx+(p.drop?' drop '+p.drop:'')+(p.txerr?' <span style="color:#f6c177" title="egress send failures (sendto returned an error; see the device log for errno; the socket self-recreates after 5 in a row)">txerr '+p.txerr+'</span>':'')+(p.hbrake?' <span style="color:#f6c177" title="dropped by the RADIO HEAP BRAKE (the anti-FIQ-wedge extinguisher) -- every extinguisher signs its catches. RADIOFLOOR <n> adjusts, 0 disables (risky).">hbrake '+p.hbrake+'</span>':'')+'</span>'
      +(p.fixed?'':'<button class="seg-b p-del" title="delete this port">\u00d7</button>')
      +'</div><div class="prow2" data-id="'+p.id+'">'
      +'<span class="hint">in</span>'
      +(p.type==='tcp'?'<span class="hint" title="TCP has no local ingress buffer BY DESIGN: backpressure lives in the TCP window, at the peer">flow-ctl</span>'
        :(p.type==='uart'||p.type==='udp')?'<span class="bar"><i id="b-i'+p.id+'"></i></span>':'<span class="hint">\u2014</span>')
      +'<span class="hint" style="margin-left:8px">out</span><span class="bar"><i id="b-o'+p.id+'"></i></span>'
      +'<span class="hint mono" id="b-t'+p.id+'">'+(p.type==='uart'&&lastInfo&&lastInfo.baud?('wire '+Math.round(lastInfo.baud/10)+' B/s max'):'')+'</span>'
      +((typeof p.lr==='number'&&p.lr>0)?'<span class="hint mono" style="margin-left:8px" title="this link&#39;s own throughput (tx+rx over 1 s) -- internal trunk traffic included, unlike the boundary TX/RX gauges">\u2195 '+p.lr+' B/s</span>':'')
      +'</div>'});
  $('ports').innerHTML=h||'<p class="hint">no ports</p>'}
function renderVlanMap(m){var by={};(m.ports||[]).forEach(function(p){if(!p.up)return;(by[p.vlan]=by[p.vlan]||[]).push(portTitle(p))});
  var h='';Object.keys(by).sort().forEach(function(v){
    var solo=by[v].length===1;
    h+='<div style="background:#10151b;border:1px solid '+(solo?'#5a4a2a':'var(--bord)')+';border-radius:6px;padding:5px 8px">'
      +'<div class="lbl" style="margin-bottom:2px">VLAN '+v+(solo?' <span style="color:#8a93a3;text-transform:none">\u2205 isolated</span>':'')+'</div>'
      +'<span class="hint">'+by[v].map(esc).join(' \u00b7 ')+'</span></div>'});
  $('vlanmap').innerHTML=h}
function updInfo(m){
  /* PGINFO merge (mirror of send_info_ paging, and of test_ui.js): with 9+
     ports the hub sends ids 0-7 and 8-15 on alternating pushes so one info
     frame can never breach the WS hard valve. A page is AUTHORITATIVE for
     its id range: an id absent from its own page is a deleted port. */
  if(typeof m.pg==='number'){
    var lo=m.pg*8,hi=lo+8;
    for(var pk in vwPortCache){var pid=+pk;if(pid>=lo&&pid<hi)delete vwPortCache[pk]}
    (m.ports||[]).forEach(function(pp){vwPortCache[pp.id]=pp});
    var mg=[];for(var mk in vwPortCache)mg.push(vwPortCache[mk]);
    mg.sort(function(a,b){return a.id-b.id});
    m.ports=mg;
  } else { vwPortCache={}; (m.ports||[]).forEach(function(pp){vwPortCache[pp.id]=pp}); }
  $('i-obs').textContent=m.obs;$('i-drop').textContent=m.dropped;
  $('i-tx').textContent=m.txr+' B/s';$('i-rx').textContent=m.rxr+' B/s';
  hTx.push(m.txr);if(hTx.length>60)hTx.shift();hRx.push(m.rxr);if(hRx.length>60)hRx.shift();
  spark('sp-tx',hTx);spark('sp-rx',hRx);
  $('i-heap').textContent=(m.heap/1024).toFixed(1)+'k';
  $('i-largest').textContent=(m.largest/1024).toFixed(1)+'k';
  $('i-frag').textContent=m.frag+'%';
  $('i-bufs').textContent=(m.bufs/1024).toFixed(1)+'k';$('i-loop').textContent=m.loop+'\u00b5s';$('i-up').textContent=m.up+'s';
  if(lastInfo&&typeof lastInfo.up==='number'&&m.up<lastInfo.up&&pcConns.length){
    log('l-err','hub restarted -- local COM bridges dropped, reconnect them');
    pcConns.slice().forEach(function(c){pcDrop(c,false)})}
  if(lastInfo&&lastInfo.tapvis!==m.tapvis){
    if(m.tapvis)log('l-sys','wire tap visible again -- the console shares the uart\u0027s VLAN');
    else log('l-sys','wire tap hidden -- the console is not in the uart\u0027s VLAN (counters keep running; move the console to see the wire)')}
  lastInfo=m;guardCheck(m);renderPorts(m);renderVlanMap(m);txRcptTick(m);
  var ld=m.loopdet|0;$('ld-off').className='seg-b'+(ld===0?' on':'');$('ld-on').className='seg-b'+(ld===1?' on':'');$('ld-kill').className='seg-b'+(ld===2?' on':'');
  var xv=m.xvlandet|0;$('xv-off').className='seg-b'+(xv===0?' on':'');$('xv-on').className='seg-b'+(xv===1?' on':'');$('xv-kill').className='seg-b'+(xv===2?' on':'');
  $('floor').placeholder=m.floor;
  var tapOn=!m.tapoff;
  $('tap-full').className='seg-b'+(tapOn&&m.tapfull?' on':'');$('tap-sum').className='seg-b'+(tapOn&&!m.tapfull?' on':'');
  $('tap-live').className='seg-b'+(tapOn&&!m.tapbatch?' on':'');$('tap-batch').className='seg-b'+(tapOn&&m.tapbatch?' on':'');
  $('tap-off').className='seg-b'+(!tapOn?' on':'');
  $('arm-btn').className='ib'+(m.armed?' armed':'');
  $('p-flood').style.display=m.flood?'':'none';
  if(m.flood||(prevDropped>=0&&m.dropped>prevDropped))lastDropTime=Date.now();
  prevDropped=m.dropped;
  if(m.baud)syncIf('s-baud',''+m.baud);   // F10: value (guarded), not placeholder -- Apply must never send a stale 115200
  function syncIf(id,v){var el=$(id);if(document.activeElement!==el)el.value=v}
  if(m.dbits)syncIf('s-data',''+m.dbits);
  if(m.par)syncIf('s-par',m.par);
  if(m.sbits)syncIf('s-stop',''+m.sbits);
  if(typeof m.gap==='number')syncIf('s-gap',''+m.gap);
  if(typeof m.delim==='number')syncIf('s-delim',m.delim?hx(m.delim):'');
  /* F6: the framing gap is in fixed ms while a char lasts 10/baud s -- at
     300 baud one byte takes 33 ms, so a 10 ms gap SPLITS real frames. The
     tap's config stays sovereign (an observer must not rewrite it), but
     the trap is disclosed the moment baud and gap disagree. */
  var gw=$('gap-warn');
  if(gw&&m.baud&&typeof m.gap==='number'){var ch2=10000/m.baud;
    if(m.gap<2*ch2){gw.style.display='';
      gw.textContent='gap '+m.gap+' ms < 2 chars ('+(2*ch2).toFixed(1)+' ms) at '+m.baud+' baud -- the tap will SPLIT real frames';}
    else gw.style.display='none';}
  $('i-mode').textContent=(m.owner?'owner':'tap-only');
  $('tap-note').style.display=(m.tapvis===false&&!m.tapoff)?'':'none';
  if(typeof m.wsdrop==='number')$('i-wsdrop').textContent=m.wsdrop;
  if(typeof m.mhz==='number')$('i-mhz').textContent=m.mhz>0?(m.mhz+' MHz'):'-';
  if(typeof m.dth==='number')$('i-dropc').textContent='thr '+m.dth+' / heap '+m.dhp+' / bkl '+m.dbk;
  if(m.uname){var u=$('i-uart');u.textContent=m.uname;
    u.style.color=(m.uhw===false)?'#f6c177':'';
    u.title=(m.uhw===false)?'SOFTWARE bit-bang: TX can wedge the 8266 under WiFi load; egress shielded to ~2 ms/pass. Hardware recipe: tx GPIO2 + no rx = UART1':'hardware FIFO: egress saturates the wire, no artificial limits';}
  if(typeof m.urx==='number'){var rb=$('i-urx');var dmx=(m.baud>=250000&&m.urx<1024);
    rb.textContent=m.urx+' B'+(dmx?' -- DMX needs 1024':'');rb.style.color=dmx?'#f6c177':'';}
  $('i-utx').textContent=(m.uhw===false)?'bit-bang (none)':'128 B FIFO';
  if(typeof m.swr==='number')$('i-swr').textContent=m.swr+' B/s';
  if(typeof m.dq==='number')$('i-dq').textContent=m.dq+' B/pass '+(m.dqd>0?'\u25b2':(m.dqd<0?'\u25bc':'\u00b7'));
  if(typeof m.mxp==='number'){$('i-mx1').textContent=m.mxp+' / '+m.mxw+' \u00b5s';
    $('i-mx2').textContent=m.mxd+' / '+m.mxh+' \u00b5s';
    if(m.mxh>50000)$('i-mx2').style.color='#e06c75';}
  if(typeof m.thr==='number'){var cz=['','heap','duty','backlog'][m.thc|0]||'';$('i-thr').textContent=m.thr+(m.thr>0&&cz?(' (last: '+cz+')'):'');}
  if(typeof m.rfloor==='number'){var rf=$('i-rfloor');
    rf.textContent=(m.rfloor===0)?'OFF -- wedge risk':(m.rfloor+' B');
    rf.style.color=(m.rfloor===5120)?'':( m.rfloor===0?'#e06c75':'#f6c177');}
  if(typeof m.peak==='number')$('i-peak').textContent=m.peak+' B/s';
  if(typeof m.eload==='number'){$('i-eload').textContent=m.eload+' %';
    var tot=(m.txr||0)+(m.rxr||0);
    $('i-head').textContent=(m.eload>=5)?('~'+Math.round(tot*100/m.eload)+' B/s'):'\u2014';}
  if(m.udpok===false){$('add-udp').disabled=true;$('add-udp').title='UDP is not supported by this platform\u0027s socket stack'}
}
function connect(){ ws=new WebSocket('ws://'+location.host+'/ws');ws.binaryType='arraybuffer';
  ws.onopen=function(){$('conn-dot').style.background='#5dcaa5';log('l-sys','connected')};
  ws.onclose=function(){$('conn-dot').style.background='#e06c75';log('l-sys','disconnected -- retrying');setTimeout(connect,2000)};
  ws.onmessage=function(ev){ if(ev.data instanceof ArrayBuffer){var u=new Uint8Array(ev.data);
      if(u.length>2&&u[0]===0x02)pcWrite(u[1],u.slice(2));
      else if(u.length>2&&u[0]===0x03)vlanFeed(u[1],u.slice(2)); return}
    var m;try{m=JSON.parse(ev.data)}catch(e){log('sys','BAD FRAME from hub: '+e.message+' -- '+String(ev.data).slice(0,80));return}onMsg(m)}}
function dlBlob(txt,mime,name){var b=new Blob([txt],{type:mime}),u=URL.createObjectURL(b),a=document.createElement('a');a.href=u;a.download=name;a.click();setTimeout(function(){URL.revokeObjectURL(u)},1000)}
function boot(){
  $('tx-send').onclick=function(){txRcptStart('send');var m=$('tx-mode').value,d=$('tx-data').value,eol=$('tx-eol').value;
    var hex='';
    if(m==='hex'){hex=d.trim()}
    else{for(var i=0;i<d.length;i++)hex+=hx(d.charCodeAt(i))+' ';
      if(eol==='crlf')hex+='0D 0A';else if(eol==='cr')hex+='0D';else if(eol==='lf')hex+='0A'}
    if(!hex.trim()){log('l-err','nothing to send');return}
    send('TX '+hex.trim())};
  $('tx-data').addEventListener('keydown',function(e){if(e.key==='Enter')$('tx-send').click()});
  $('dmx-send').onclick=function(){var v=$('dmx-ch').value.trim();if(!v){log('l-err','give channel bytes');return}send('DMXTX '+v)};
  $('s-apply').onclick=function(){send('LINE '+($('s-baud').value||'115200')+' '+$('s-data').value+$('s-par').value+$('s-stop').value)};
  $('s-dmx').onclick=function(){$('s-baud').value='250000';$('s-data').value='8';$('s-par').value='N';$('s-stop').value='2';$('s-gap').value='4';$('s-apply').onclick();$('s-fapply').onclick()};
  $('s-mb').onclick=function(){$('s-gap').value='4';$('s-fapply').onclick()};
  $('s-fapply').onclick=function(){send('FRAME '+($('s-gap').value||'10')+' '+($('s-delim').value.trim()||'-'))};
  $('pc-conn').onclick=pcConnect;
  $('port-reset').onclick=function(){send('PORT RESET')};
  $('add-tcp').onclick=function(){var b=parseInt($('new-buf').value,10)||256;var p=prompt('TCP listen port','2324');if(p)send('PORT ADD TCP '+p+' '+b)};
  $('add-udp').onclick=function(){var b=parseInt($('new-buf').value,10)||256;var p=prompt('UDP port','5000');if(p)send('PORT ADD UDP '+p+' '+b)};
  $('add-bridge').onclick=function(){var b=parseInt($('new-buf').value,10)||256;send('PORT ADD BRIDGE TRUNK '+b)};  // slot-less TRUNK socket for the
  // virtual wire (wire \u21c4 pill). The comment that lived here for eras -- "a bridge without a COM
  // port is a dead end" -- was true until the trunk existed; a COM-backed bridge is the Connect button.
  $('pc-list').addEventListener('click',function(e){
    if(e.target.classList.contains('pc-relink')){var sl=parseInt(e.target.getAttribute('data-slot'),10);
      var c=pcConns.find(function(x){return x.slot===sl});
      if(c){var t=prompt('Adopt which hub bridge port id? (slot-less, un-wired)','');
        if(t!==null&&/^\d+$/.test(t.trim())){c.relink=true;pcPending=c;send('PORT ADOPT '+t.trim())}}
      return}
    if(!e.target.classList.contains('pc-del'))return;
    var sl=parseInt(e.target.dataset.slot,10);
    for(var i=0;i<pcConns.length;i++)if(pcConns[i].slot===sl){pcDrop(pcConns[i],true);break}});
  $('ports').addEventListener('click',function(e){
    var row=e.target.closest('.row');if(!row)return;
    var id=parseInt(row.dataset.id,10),p=null;
    ((lastInfo&&lastInfo.ports)||[]).forEach(function(q){if(q.id===id)p=q});
    if(!p)return;
    if(e.target.classList.contains('p-vlan'))send('PORT VLAN '+id+' '+((p.vlan%8)+1));
    else if(e.target.classList.contains('p-updn'))send('PORT '+(p.up?'DOWN':'UP')+' '+id);
    else if(e.target.classList.contains('p-rate')){var r=prompt('Ingress cap for port '+id+' (bytes/s, 0 = unlimited)',p.rate||0);if(r!==null&&/^\d+$/.test(r.trim()))send('PORT RATE '+id+' '+r.trim())}
    else if(e.target.classList.contains('p-orate')){var r2=prompt('Egress shaper for port '+id+' (bytes/s, 0 = unlimited)',p.orate||0);if(r2!==null&&/^\d+$/.test(r2.trim()))send('PORT ORATE '+id+' '+r2.trim())}
    else if(e.target.classList.contains('p-wire')){
      if(p.wire>=0){if(confirm('Unplug the virtual trunk between port '+id+' and port '+p.wire+'?'))send('PORT WIRE '+id+' -1')}
      else{var w=prompt('Wire port '+id+' to which bridge port? (its OWN id = loopback/hairpin; different VLANs = an inter-VLAN trunk)','');
        if(w!==null&&/^\d+$/.test(w.trim()))send('PORT WIRE '+id+' '+w.trim())}}
    else if(e.target.classList.contains('p-del')){if(confirm('Delete port '+id+'?'))send('PORT DEL '+id)}});
  rawLegend();rawRender();
  $('tb-general').onclick=function(){tabShow('general')};
  $('tb-uart').onclick=function(){tabShow('uart')};
  $('tb-views').onclick=function(){tabShow('views')};
  $('vw-wi').oninput=vwWhat;$('vw-wq').oninput=vwWhat;vwWhat();
  $('vw-topo').addEventListener('click',function(e){
    var r=this.getBoundingClientRect();if(!r.width||!r.height)return;
    var mx=(e.clientX-r.left)*this.width/r.width,my=(e.clientY-r.top)*this.height/r.height,hit=-1;
    vwNodes.forEach(function(n){var dx=n.x-mx,dy=n.y-my;if(dx*dx+dy*dy<196)hit=n.p.id});
    vwSel=(hit===vwSel)?-1:hit;if(lastInfo)vwTopo(lastInfo)});
  setInterval(function(){if(viewsOn()&&vwStamp){var e=$('vw-age');
    if(e)e.textContent='updated '+Math.max(0,Math.round((Date.now()-vwStamp)/1000))+'s ago'}},1000);
  var ro=$('raw-out');
  ro.addEventListener('keydown',rawKey);
  ro.addEventListener('focus',function(){rawKbdState(true)});
  ro.addEventListener('blur',function(){rawKbdState(false)});
  $('raw-clear').onclick=function(){rawClear();ro.focus()};
  $('raw-abort').onclick=function(){dumpStop('transfer aborted')};
  $('raw-file').onclick=function(){$('raw-fin').click()};
  $('raw-fin').addEventListener('change',function(){var f=this.files&&this.files[0];if(!f)return;
    var rd=new FileReader();rd.onload=function(){dumpStart(new Uint8Array(rd.result),f.name)};
    rd.onerror=function(){log('l-err','could not read '+f.name)};rd.readAsArrayBuffer(f);this.value=''});
  ro.addEventListener('paste',function(e){e.preventDefault();
    dumpText((e.clipboardData||window.clipboardData).getData('text'),'clipboard')});
  $('raw-paste').onclick=function(){
    if(!navigator.clipboard||!navigator.clipboard.readText){log('l-err','clipboard API unavailable -- use Ctrl+V on the screen');return}
    navigator.clipboard.readText().then(function(t){dumpText(t,'clipboard')})
      .catch(function(){log('l-err','clipboard read refused -- use Ctrl+V on the screen instead')})};
  $('new-buf').addEventListener('input',function(){if(lastInfo)guardCheck(lastInfo)});
  $('floor').addEventListener('change',function(){var v=parseInt(this.value,10);if(v>=1024)send('FLOOR '+v)});
  $('ld-off').onclick=function(){send('LOOPDETECT OFF')};$('ld-on').onclick=function(){send('LOOPDETECT ON')};$('ld-kill').onclick=function(){send('LOOPDETECT KILL')};
  $('sys-reboot').onclick=function(){if(confirm('Reboot the hub? Ports and trunks are saved; COM links will drop.'))send('REBOOT')};
  $('cmd-go').onclick=function(){var v=$('cmd-in').value.trim();if(v){send(v);$('cmd-in').value=''}};
  $('cmd-in').onkeydown=function(e){if(e.key==='Enter')$('cmd-go').onclick()};
  $('xv-off').onclick=function(){send('XVLANDETECT OFF')};$('xv-on').onclick=function(){send('XVLANDETECT ON')};$('xv-kill').onclick=function(){send('XVLANDETECT KILL')};
  $('tap-full').onclick=function(){send('TAP FULL '+$('tap-filter').value)};
  $('tap-sum').onclick=function(){send('TAP SUMMARY '+$('tap-filter').value)};
  $('tap-live').onclick=function(){send('TAP LIVE')};$('tap-batch').onclick=function(){send('TAP BATCH')};$('tap-off').onclick=function(){send('TAP OFF')};
  $('arm-btn').onclick=function(){var d=$('arm-dir').value.trim().toUpperCase();
    send('ARM '+(d==='TX'?'0':d==='RX'?'1':'*')+' '+($('arm-b').value.trim()||''))};
  $('log-pause').onclick=function(){paused=!paused;if(!paused){logSkipped=0;renderLog()}updPauseBtn()};
  $('log-filter').addEventListener('input',function(){filt=this.value.toLowerCase();renderLog()});
  $('log-clear').onclick=function(){logs=[];logSkipped=0;lastTxUs=null;renderLog();updPauseBtn()};
  $('dec-btn').onclick=decRun;
  $('dec-clear').onclick=function(){$('dec-out').innerHTML='<div class="dempty">No decode yet.</div>'};
  $('dec-filter').addEventListener('input',function(){decFilter=this.value.trim().toLowerCase();applyDecFilter()});
  $('log-csv').onclick=function(){var c='time,delta_ms,dir,bytes,line\n';
    logs.forEach(function(e){var t=e.txn||{};c+=e.ts+','+(e.dus!==undefined?(e.dus/1000).toFixed(3):'')+','+(t.dir!==undefined?(t.dir?'RX':'TX'):'')+','+(t.total!==undefined?t.total:'')+',"'+e.txt.replace(/"/g,'""')+'"\n'});
    dlBlob(c,'text/csv','web_serial_log.csv')};
  updPauseBtn();connect()}
document.addEventListener('DOMContentLoaded',boot);
</script></body></html>)HTMLDOC";
static const uint32_t WSER_PAGE_LEN = sizeof(WSER_PAGE) - 1;

namespace ws {

void Sha1::reset() {
  h_[0] = 0x67452301;
  h_[1] = 0xEFCDAB89;
  h_[2] = 0x98BADCFE;
  h_[3] = 0x10325476;
  h_[4] = 0xC3D2E1F0;
  len_ = 0;
  buf_len_ = 0;
}

void Sha1::update(const uint8_t *data, size_t len) {
  len_ += len;
  while (len > 0) {
    size_t take = 64 - buf_len_;
    if (take > len)
      take = len;
    memcpy(buf_ + buf_len_, data, take);
    buf_len_ += take;
    data += take;
    len -= take;
    if (buf_len_ == 64) {
      process_(buf_);
      buf_len_ = 0;
    }
  }
}

void Sha1::finish(uint8_t out[20]) {
  uint64_t bits = len_ * 8;
  uint8_t pad = 0x80;
  update(&pad, 1);
  uint8_t zero = 0;
  while (buf_len_ != 56)
    update(&zero, 1);
  uint8_t lenbuf[8];
  for (int i = 0; i < 8; i++)
    lenbuf[i] = (uint8_t) (bits >> (56 - i * 8));
  update(lenbuf, 8);
  for (int i = 0; i < 5; i++) {
    out[i * 4] = (uint8_t) (h_[i] >> 24);
    out[i * 4 + 1] = (uint8_t) (h_[i] >> 16);
    out[i * 4 + 2] = (uint8_t) (h_[i] >> 8);
    out[i * 4 + 3] = (uint8_t) (h_[i]);
  }
}

void Sha1::process_(const uint8_t *p) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++)
    w[i] = (p[i * 4] << 24) | (p[i * 4 + 1] << 16) | (p[i * 4 + 2] << 8) | p[i * 4 + 3];
  for (int i = 16; i < 80; i++)
    w[i] = rol_(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5A827999;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }
    uint32_t t = rol_(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rol_(b, 30);
    b = a;
    a = t;
  }
  h_[0] += a;
  h_[1] += b;
  h_[2] += c;
  h_[3] += d;
  h_[4] += e;
}

std::string accept_key(const std::string &client_key) {
  static const char MAGIC[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  std::string concat = client_key + MAGIC;
  Sha1 s;
  s.update((const uint8_t *) concat.data(), concat.size());
  uint8_t digest[20];
  s.finish(digest);
  return base64_encode(digest, 20);
}

size_t decode_frame(const uint8_t *buf, size_t len, Frame &out) {
  if (len < 2)
    return 0;
  out.fin = (buf[0] & 0x80) != 0;
  out.opcode = buf[0] & 0x0F;
  bool masked = (buf[1] & 0x80) != 0;
  uint64_t plen = buf[1] & 0x7F;
  size_t pos = 2;
  if (plen == 126) {
    if (len < pos + 2)
      return 0;
    plen = ((uint64_t) buf[pos] << 8) | buf[pos + 1];
    pos += 2;
  } else if (plen == 127) {
    if (len < pos + 8)
      return 0;
    plen = 0;
    for (int i = 0; i < 8; i++)
      plen = (plen << 8) | buf[pos + i];
    pos += 8;
  }
  uint8_t mask[4] = {0, 0, 0, 0};
  if (masked) {
    if (len < pos + 4)
      return 0;
    for (int i = 0; i < 4; i++)
      mask[i] = buf[pos + i];
    pos += 4;
  }
  if (len < pos + plen)
    return 0;  // incomplete payload
  out.payload.resize(plen);
  for (uint64_t i = 0; i < plen; i++)
    out.payload[i] = buf[pos + i] ^ (masked ? mask[i & 3] : 0);
  return pos + plen;
}

static void encode_header_(std::vector<uint8_t> &out, uint8_t opcode, size_t plen) {
  out.push_back(0x80 | (opcode & 0x0F));  // FIN + opcode
  if (plen < 126) {
    out.push_back((uint8_t) plen);
  } else if (plen <= 0xFFFF) {
    out.push_back(126);
    out.push_back((uint8_t) (plen >> 8));
    out.push_back((uint8_t) plen);
  } else {
    out.push_back(127);
    for (int i = 7; i >= 0; i--)
      out.push_back((uint8_t) (plen >> (i * 8)));
  }
}

static void frag_header_(std::vector<uint8_t> &out, uint8_t b0, size_t plen) {
  out.push_back(b0);
  if (plen < 126) {
    out.push_back((uint8_t) plen);
  } else {
    out.push_back(126);
    out.push_back((uint8_t) (plen >> 8));
    out.push_back((uint8_t) (plen & 0xFF));
  }
}

void encode_frame(std::vector<uint8_t> &out, uint8_t opcode, const uint8_t *payload, size_t plen) {
  // A-FRAG ablation: payloads > 1024 B are split into protocol-legal
  // WebSocket continuations (FIN=0 head, 0x00 middles, FIN=1 tail). The
  // browser reassembles transparently; the JSON never changes -- but the
  // 2.6 KB connect burst becomes <=1.03 KB TCP writes. If the burst SIZE
  // is the killer, deaths stop at this exact line.
  if (plen > 1024) {
    size_t off = 0;
    bool first = true;
    while (off < plen) {
      size_t k = plen - off;
      if (k > 1024)
        k = 1024;
      bool last = (off + k) == plen;
      uint8_t b0 = (uint8_t) ((last ? 0x80 : 0x00) | (first ? (opcode & 0x0F) : 0x00));
      frag_header_(out, b0, k);
      for (size_t i = 0; i < k; i++)
        out.push_back(payload[off + i]);
      off += k;
      first = false;
    }
    return;
  }
  encode_header_(out, opcode, plen);
  for (size_t i = 0; i < plen; i++)
    out.push_back(payload[i]);
}

void encode_frame_header(std::vector<uint8_t> &out, uint8_t opcode, size_t plen) {
  encode_header_(out, opcode, plen);
}

}  // namespace ws

// ======================================================================
//  helpers
// ======================================================================

// bytes -> hex string "3C AF 01"
static std::string hex_join(const uint8_t *data, size_t len) {
  std::string s;
  char buf[4];
  for (size_t i = 0; i < len; i++) {
    snprintf(buf, sizeof(buf), "%02X", data[i]);
    if (i)
      s += " ";
    s += buf;
  }
  return s;
}

// advance p to the next non-space token; false if end of string
static bool skip_to_token(const char *&p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
    p++;
  return *p != '\0';
}

// read ONE hex byte at position p ("3C" / "0x3C" / "3c"), WITHOUT allocating

// read a DECIMAL unsigned number (for RATE / lengths); advance p
static bool read_dec(const char *&p, uint32_t &out) {
  const char *s = p;
  uint32_t v = 0;
  int nd = 0;
  while (*s >= '0' && *s <= '9') {
    v = v * 10 + (uint32_t) (*s - '0');
    nd++;
    s++;
    if (nd > 9)
      return false;
  }
  if (nd == 0 || (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n'))
    return false;
  out = v;
  p = s;
  return true;
}


// ======================================================================
//  Component
// ======================================================================
void WebSerial::start_server_() {
  this->server_started_ = true;
  this->server_ = socket::socket_ip_loop_monitored(SOCK_STREAM, 0);
  if (this->server_ == nullptr) {
    ESP_LOGE(TAG, "socket failed");
    this->mark_failed();
    return;
  }
  if (this->server_->setblocking(false) != 0) {
    this->mark_failed();
    return;
  }
  int enable = 1;
  this->server_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));
  struct sockaddr_storage sa {};
  socklen_t sl = socket::set_sockaddr_any(reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa), this->port_);
  if (this->server_->bind(reinterpret_cast<struct sockaddr *>(&sa), sl) != 0 || this->server_->listen(4) != 0) {
    ESP_LOGE(TAG, "bind/listen failed on port %u", this->port_);
    this->server_ = nullptr;
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "listening on port %u", this->port_);
}

void WebSerial::accept_client_() {
  if (this->server_ == nullptr || this->pending_client_ != nullptr)
    return;
  auto client = this->server_->accept_loop_monitored(nullptr, nullptr);
  if (client == nullptr)
    return;
  // Second-client-replaces-first, same policy as the TCP232 ports: a stale
  // half-dead WS (browser crash, wifi roam) would otherwise hold the single
  // slot until TCP gives up on it -- minutes of locked-out UI. Never evict a
  // page transfer mid-flight, though: those finish in a few passes anyway.
  if (this->stream_client_ != nullptr) {
    if (this->serving_page_) {
      client->close();
      return;
    }
    this->drop_client_("replaced by a new client");
  }
  client->setblocking(false);
  this->pending_client_ = std::move(client);
  this->request_len_ = 0;
  this->nl_ = 0;
  this->pending_since_ = millis();
}

void WebSerial::read_client_() {
  // HTTP handshake
  if (this->pending_client_ != nullptr) {
    uint8_t b[128];
    for (;;) {
      const ssize_t len = this->pending_client_->read(b, sizeof(b));
      if (len == 0) {
        this->pending_client_->close();
        this->pending_client_ = nullptr;
        return;
      }
      if (len < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
          return;
        this->pending_client_->close();
        this->pending_client_ = nullptr;
        return;
      }
      this->pending_since_ = millis();
      for (ssize_t i = 0; i < len; i++) {
        const char c = (char) b[i];
        if (this->request_len_ + 1 < sizeof(this->request_) && c != '\r')
          this->request_[this->request_len_++] = c;
        if (c == '\n') {
          if (++this->nl_ >= 2) {
            this->request_[this->request_len_] = '\0';
            this->do_handshake_(this->request_, this->request_len_);
            return;
          }
        } else if (c != '\r') {
          this->nl_ = 0;
        }
      }
    }
  }
  // active WebSocket: drain + slice frames
  if (this->stream_client_ != nullptr) {
    uint8_t b[256];  // PIEGES P3: local > 256 o = alarme (pile 8266 ~4 Ko)
    for (;;) {
      const ssize_t len = this->stream_client_->read(b, sizeof(b));
      if (len == 0) {
        this->drop_client_("peer closed");
        return;
      }
      if (len < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
          break;
        this->drop_client_("read error");
        return;
      }
      this->ws_accum_.insert(this->ws_accum_.end(), b, b + len);
      if (this->ws_accum_.size() > 8192) {
        this->drop_client_("frame too large");
        return;
      }
    }
    for (;;) {
      ws::Frame fr;
      size_t used = ws::decode_frame(this->ws_accum_.data(), this->ws_accum_.size(), fr);
      if (used == 0)
        break;
      this->ws_accum_.erase(this->ws_accum_.begin(), this->ws_accum_.begin() + used);
      this->handle_ws_frame_(fr);
      if (this->stream_client_ == nullptr)
        break;
    }
  }
}

bool WebSerial::do_handshake_(const char *request, size_t len) {
  const char *kh = nullptr;
  static const char NEEDLE[] = "sec-websocket-key:";
  for (const char *p = request; *p; p++) {
    size_t j = 0;
    while (NEEDLE[j] && p[j] && (p[j] == NEEDLE[j] || (p[j] >= 'A' && p[j] <= 'Z' && (p[j] + 32) == NEEDLE[j])))
      j++;
    if (NEEDLE[j] == '\0') {
      kh = p + j;
      break;
    }
  }
  if (kh == nullptr) {
    // plain GET -> serve the page
    if (strncmp(request, "GET / ", 6) == 0 || strncmp(request, "GET /?", 6) == 0 ||
        strncmp(request, "GET / HTTP", 10) == 0) {
      this->page_len_ = WSER_PAGE_LEN;
      this->page_pos_ = 0;
      std::string headers =
          "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
          "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
          std::to_string(this->page_len_) + "\r\n\r\n";
      this->out_.assign(headers.begin(), headers.end());
      this->out_pos_ = 0;
      this->stream_client_ = std::move(this->pending_client_);
      this->pending_client_ = nullptr;
      this->serving_page_ = true;
  WSER_EVT(3);
      this->flush_tx_();
    } else {
      static const char NF[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      this->pending_client_->write(NF, sizeof(NF) - 1);
      this->pending_client_->close();
      this->pending_client_ = nullptr;
    }
    return false;
  }
  while (*kh == ' ')
    kh++;
  std::string key;
  while (*kh && *kh != '\r' && *kh != '\n')
    key.push_back(*kh++);
  std::string accept = ws::accept_key(key);
  std::string resp =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Accept: " +
      accept + "\r\n\r\n";
  this->stream_client_ = std::move(this->pending_client_);
  this->pending_client_ = nullptr;
  this->serving_page_ = false;
  this->ws_accum_.clear();
  this->ws_accum_.reserve(512);  // PIEGES P2: pas de churn realloc en debut de connexion
  this->out_.insert(this->out_.end(), resp.begin(), resp.end());
  this->out_pos_ = 0;
  this->flush_tx_();
  ESP_LOGD(TAG, "websocket client connected (free %u largest %u)", (unsigned) this->free_heap_(), (unsigned) this->largest_block_());
  WSER_EVT(1);
  this->ws_send_text_("{\"t\":\"hello\",\"msg\":\"web_serial hub ready\"}");
  this->last_info_ = 0;  // first info push happens from loop() (shallow stack)
  return true;
}

void WebSerial::handle_ws_frame_(const ws::Frame &frame) {
  switch (frame.opcode) {
    case ws::OP_CLOSE:
      this->drop_client_("close frame");
      break;
    case ws::OP_PING:
      this->ws_send_(ws::OP_PONG, frame.payload.data(), frame.payload.size());
      break;
    case ws::OP_BIN:
      // Web Serial bridge: 0x01 <bridge_id> <data> = a browser LOCAL PC port's
      // RX; enters the switch as that bridge port's ingress.
      if (frame.payload.size() > 2 && frame.payload[0] == 0x01) {
        int bid = frame.payload[1];
        if (bid >= 0 && bid < 4 && this->bridge_pi_[bid] >= 0)
          this->switch_ingress_(this->bridge_pi_[bid], frame.payload.data() + 2, frame.payload.size() - 2);
      }
      break;
    case ws::OP_TEXT:
      if (!frame.payload.empty()) {
        std::string line(reinterpret_cast<const char *>(frame.payload.data()), frame.payload.size());
        this->handle_command_(line);
      }
      break;
    default:
      break;
  }
}

void WebSerial::ws_send_text_(const std::string &text) {
  this->ws_send_(ws::OP_TEXT, reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

bool WebSerial::ws_send_(uint8_t opcode, const uint8_t *payload, size_t len) {
  if (this->stream_client_ == nullptr || this->serving_page_)
    return false;  // never splice a WS frame into the HTML page stream
#ifdef USE_ESP8266
  // OOM safety net (ESP8266 only): under severe heap pressure, DROP the frame
  // instead of letting the TX vector / TCP buffers push the heap to zero and
  // crash. Display data is expendable; uptime is not. ESP32 has ample RAM (and
  // Arduino ESP.* does not exist under ESP-IDF), so it is not gated here.
  if (this->free_heap_() < (uint32_t) (len + 4096))
    return false;
#endif
  if (this->out_.size() > WSER_OUT_HARD) {
    // beyond salvage: clearing via client drop is the only bounded move
    this->drop_client_("egress backlog (log storm or slow client)");
    return false;
  }
  if (this->out_.size() > WSER_OUT_SOFT) {
    this->ws_drop_++;   // droppable by design; the next info push resyncs
    return false;
  }
  if (!this->coh_exempt_ && this->largest_cache_ != 0 && this->largest_cache_ < WSER_COH_FLOOR &&
      this->pass_subs_ >= 1) {
    // Cohabitation valve (captain design): under memory distress, ONE lwip
    // submission per pass -- our pbuf pressure on the SYS side is spaced
    // out. Display frames are droppable by design; control frames exempt.
    this->ws_drop_++;
    return false;
  }
  ws::encode_frame(this->out_, opcode, payload, len);
  this->pass_subs_++;
  this->flush_tx_();
  return true;   // encoded: the frame is in the belt (flush may mutate out_, so
                 // callers must NEVER infer the verdict from out_.size())
}

bool WebSerial::flush_tx_() {
  auto *client = this->stream_client_ ? this->stream_client_.get() : nullptr;
  if (client == nullptr) {
    this->out_.clear();
    this->out_pos_ = 0;
    this->serving_page_ = false;
    return false;
  }
  // WEDGE BELT (HW-WDT hunt): whatever lies about progress below, this call
  // can never exceed a bounded amount of work -- resume next pass instead.
  size_t belt_bytes = 0;
  int belt_iters = 0;
  for (;;) {
    if (++belt_iters > 64)
      return true;
    while (this->out_pos_ < this->out_.size()) {
      if (belt_bytes > 8192)
        return true;
      const ssize_t w = client->write(this->out_.data() + this->out_pos_, this->out_.size() - this->out_pos_);
      if (w > 0) {
        this->out_pos_ += (size_t) w;
        belt_bytes += (size_t) w;
        continue;
      }
      if (w == 0 || errno == EAGAIN || errno == EWOULDBLOCK)
        return true;  // buffer full: retry next pass (never close on EAGAIN)
      this->drop_client_("write error");
      return false;
    }
    this->out_.clear();
    this->out_pos_ = 0;
    if (this->serving_page_ && this->page_pos_ < this->page_len_) {
      size_t chunk = this->page_len_ - this->page_pos_;
      if (chunk > 512)
        chunk = 512;
      this->out_.resize(chunk);
      progmem_memcpy(this->out_.data(), WSER_PAGE + this->page_pos_, chunk);
      this->page_pos_ += chunk;
      this->out_pos_ = 0;
      continue;
    }
    if (this->serving_page_) {
      this->stream_client_->close();
      this->stream_client_ = nullptr;
      this->serving_page_ = false;
    }
    return true;
  }
}

void WebSerial::drop_client_(const char *why) {
  ESP_LOGD(TAG, "ws client dropped: %s", why);
  if (this->stream_client_ != nullptr) {
    this->stream_client_->close();
    this->stream_client_ = nullptr;
  }
  this->out_.clear();
  this->out_pos_ = 0;
  this->ws_accum_.clear();
  this->serving_page_ = false;
}

// ============================ the serial HUB ============================
uint16_t wser_crc16(const uint8_t *d, size_t n) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int j = 0; j < 8; j++)
      c = (c & 1) ? (uint16_t) ((c >> 1) ^ 0xA001) : (uint16_t) (c >> 1);
  }
  return c;
}

// ---- the runtime seam: every byte any component (or we) puts on / takes off
// the wire arrives here with its direction. Cheap per-byte work only. ----
void WebSerial::on_uart_byte(uart::UARTDirection dir, uint8_t b) {
  uint8_t d = (dir == uart::UART_DIRECTION_RX) ? 1 : 0;
  if (d)
    this->rx_bytes_++;
  else
    this->tx_bytes_++;
  uint32_t nowu = micros();
  // silence closes the previous frame (checked here AND in loop for idle lines)
  if (this->fr_.open && (uint32_t) (nowu - this->last_byte_us_) > (uint32_t) this->gap_ms_ * 1000)
    this->frame_close_();
  this->last_byte_us_ = nowu;
  this->frame_byte_(b, d);
  // RX from the wire enters the switch as the UART port's ingress -> forwarded
  // to every other port in the UART port's VLAN. TX is already on the wire.
  // STAGED, not per byte: at 115200 that would be 11.5k switch passes/s of
  // pure overhead, and 1-byte chunks are below the loop/xvlan fingerprint
  // threshold -- wire-RX-origin traffic would be invisible to detection.
  // The stage flushes on fill or from loop() each pass: zero added latency
  // (net egress drains on the same loop pass either way).
  if (d && this->uart_pi_ >= 0) {
    this->rx_stage_[this->rx_stage_n_++] = b;
    if (this->rx_stage_n_ >= sizeof(this->rx_stage_))
      this->rx_stage_flush_();
  }
  if (this->delim_ != 0 && b == this->delim_) {
    this->fr_.byd_ = true;
    this->frame_close_();
  }
}

void WebSerial::rx_stage_flush_() {
  if (this->rx_stage_n_ == 0 || this->uart_pi_ < 0)
    return;
  size_t n = this->rx_stage_n_;
  this->rx_stage_n_ = 0;  // clear FIRST: switch_ingress_ may re-enter logging
  this->switch_ingress_(this->uart_pi_, this->rx_stage_, n);
}

void WebSerial::frame_byte_(uint8_t b, uint8_t dir) {
  if (!this->fr_.open) {
    this->fr_ = SerFrame{};
    this->fr_.open = true;
    this->fr_.self = this->self_active_;
    this->fr_.us = micros();
  }
  SerFrame &f = this->fr_;
  f.total++;
  if (f.nruns > 0 && f.runs[f.nruns - 1].dir == dir) {
    SerRun &r = f.runs[f.nruns - 1];
    r.total++;
    if (r.n < WSER_SNIP)
      r.snip[r.n++] = b;
  } else if (f.nruns < WSER_RUNS) {
    SerRun &r = f.runs[f.nruns++];
    r.dir = dir;
    r.total = 1;
    r.n = 1;
    r.snip[0] = b;
  }
  if (f.total >= WSER_FRAME_CAP)
    this->frame_close_();
}

void WebSerial::frame_close_() {
  if (!this->fr_.open)
    return;
  this->fr_.dur_us = micros() - this->fr_.us;
  this->observed_++;
  this->flush_frame_();
  this->fr_.open = false;
}

// The wire tap DISPLAY follows the console's VLAN membership: the console is
// a switch port like any other, and a port only sees its own VLAN. Wire
// frames render in the web log only while the console shares the UART
// port's VLAN (and is UP). The tap keeps RUNNING regardless -- counters,
// rates and the FLOODED badge stay physical truth -- only the log lines are
// withheld. TAP OFF remains the independent master kill.
bool WebSerial::tap_visible_() {
  if (this->console_pi_ < 0 || this->uart_pi_ < 0)
    return true;
  Port &c = this->ports_[this->console_pi_];
  return c.up && c.vlan == this->ports_[this->uart_pi_].vlan;
}

void WebSerial::flush_frame_() {
  SerFrame &f = this->fr_;
  if (f.total == 0)
    return;
  if (this->out_.size() > WSER_OUT_SOFT) {
    // refuse the work UPSTREAM: the soft cap would drop this message
    // anyway -- but only AFTER we paid its JSON construction (allocs, hex
    // formatting). Under storm that was hundreds of built-then-discarded
    // strings per minute (measured: WS drops 751 > frames shown 688), on
    // a chip starving for both CPU and heap. Consume the frame, count it,
    // build NOTHING.
    this->ws_drop_++;
    f.total = 0;
    f.nruns = 0;
    f.open = false;
    return;
  }
#ifdef USE_ESP8266
  if (this->largest_block_() < 1536) {  // frame JSON ~300 B + churn: same gate
    this->dropped_++;
    this->drop_heap_++;
    f.total = 0;   // the frame is consumed either way (state stays coherent)
    f.nruns = 0;
    f.open = false;
    return;
  }
#endif
  if (this->tap_off_ || !this->tap_visible_())
    return;
  uint8_t dir = f.nruns ? f.runs[0].dir : 0;
  if (this->tap_filter_dir_ >= 0 && dir != (uint8_t) this->tap_filter_dir_)
    return;
  bool armed = false;
  if (this->arm_dir_ != -2) {
    bool dir_ok = this->arm_dir_ == -1 || dir == (uint8_t) this->arm_dir_;
    bool b_ok = this->arm_byte_ < 0 || (f.nruns && f.runs[0].n && f.runs[0].snip[0] == (uint8_t) this->arm_byte_);
    armed = dir_ok && b_ok;
  }
  uint32_t nowu = micros();
  // direction-fair throttle (the web_onewire lesson, built in from day one):
  // a frame in the OTHER direction bypasses the gap, so request/reply pairs
  // (Modbus poll + answer) always log both sides.
  bool same_dir = (dir == this->last_emit_dir_);
  if (!armed && !this->tap_batch_ && same_dir && nowu - this->last_tap_log_us_ < 20000) {
    this->dropped_++;
    this->drop_thr_++;
    this->flood_ = true;
    return;
  }
  if (this->stream_client_ == nullptr || this->serving_page_)
    return;
  if (this->out_.size() - this->out_pos_ > WSER_LOG_BACKLOG) {
    this->dropped_++;
    this->drop_bkl_++;
    this->flood_ = true;
    return;
  }
  if (this->tap_batch_ && !armed) {
    for (int i = 0; i < this->agg_used_; i++) {
      if (this->agg_[i].dir == dir && this->agg_[i].self == f.self && this->agg_[i].total == f.total) {
        this->agg_[i].count++;
        return;
      }
    }
    if (this->agg_used_ < 10) {
      AggE &e = this->agg_[this->agg_used_++];
      e.dir = dir;
      e.self = f.self;
      e.total = f.total;
      e.count = 1;
      e.us = f.us;
    }
    return;
  }
  std::string j = "{\"t\":\"frame\",\"dir\":" + std::to_string((int) dir) + ",\"total\":" + std::to_string(f.total) +
                  ",\"us\":" + std::to_string(f.us) + ",\"dur\":" + std::to_string(f.dur_us);
  if (f.self)
    j += ",\"self\":true";
  if (f.byd_)
    j += ",\"byDelim\":true";
  if (armed)
    j += ",\"trg\":true";
  if (this->tap_full_) {
    j += ",\"runs\":[";
    for (int i = 0; i < f.nruns; i++) {
      const SerRun &r = f.runs[i];
      if (i)
        j += ",";
      j += "{\"dir\":" + std::to_string((int) r.dir) + ",\"tot\":" + std::to_string(r.total) + ",\"n\":" + std::to_string((int) r.n) +
           ",\"b\":\"" + hex_join(r.snip, r.n) + "\"}";
    }
    j += "]";
  }
  j += "}";
  this->ws_send_text_(j);
  this->last_tap_log_us_ = nowu;
  this->last_emit_dir_ = dir;
  if (armed)
    this->arm_dir_ = -2;
}

void WebSerial::flush_agg_() {
  if (this->agg_used_ == 0 || this->stream_client_ == nullptr || this->serving_page_)
    return;
  if (!this->tap_visible_()) { this->agg_used_ = 0; return; }  // withheld, state still drained
  std::string j = "{\"t\":\"fbatch\",\"txns\":[";
  for (int i = 0; i < this->agg_used_; i++) {
    AggE &e = this->agg_[i];
    if (i)
      j += ",";
    j += "{\"dir\":" + std::to_string((int) e.dir) + ",\"total\":" + std::to_string(e.total) + ",\"count\":" + std::to_string(e.count) +
         ",\"us\":" + std::to_string(e.us) + (e.self ? ",\"self\":true" : "") + "}";
  }
  j += "]}";
  this->ws_send_text_(j);
  this->agg_used_ = 0;
}

// ---- the QoS write path: every hub port writes the wire through here.
// RS485: assert de_pin, write, flush (blocks until shifted out), release. ----
void WebSerial::wire_write_(const uint8_t *d, size_t n, bool self, int origin_pi) {
  if (this->uart_ == nullptr || n == 0)
    return;
  WSER_CRUMB(0x60);            // entered wire_write_
  this->self_active_ = self;
  if (this->de_pin_ != nullptr)
    this->de_pin_->digital_write(true);
  WSER_CRUMB(0x61);            // about to enter uart write (hw FIFO or bit-bang)
  this->uart_->write_array(d, n);   // the debug callback taps these as TX
  WSER_CRUMB(0x62);            // uart write returned (wedge past here = tap/flush side)
  if (this->de_pin_ != nullptr) {
    // RS485 only: DE must stay asserted until the last bit is SHIFTED OUT,
    // so the blocking flush is the contract. Without de_pin it was a pure
    // wire-speed stall on EVERY write (worst on software serial: bit-bang
    // write + flush = ~1 ms/byte of CPU at 19200).
    WSER_CRUMB(0x63);          // inside RS485 flush (only with de_pin)
    this->uart_->flush();
    this->de_pin_->digital_write(false);
  }
  WSER_CRUMB(0x64);            // wire_write_ complete
  this->self_active_ = false;
}

// ---- TCP232: the raw network port (PuTTY / telnet / Eltima direct) ----



// ---- Web Serial bridge: wire RX copies go to the browser as binary 0x02+data;
// browser sends binary 0x01+data (its LOCAL PC port's RX) for the wire. ----


// ---- experimental DMX512 TX: break by baud trick (a 0x00 at 90000 baud is a
// ~100 us low = a valid BREAK + MAB), then 250000 8N2 start code + channels ----
void WebSerial::dmx_tx_(const uint8_t *ch, size_t n) {
  if (this->uart_ == nullptr)
    return;
  uint32_t old_baud = this->uart_->get_baud_rate();
  this->uart_->set_baud_rate(90000);
  this->uart_->load_settings(false);
  uint8_t z = 0;
  this->wire_write_(&z, 1, true);  // the BREAK
  this->uart_->set_baud_rate(250000);
  this->uart_->load_settings(false);
  uint8_t frame[65];
  frame[0] = 0x00;  // start code
  size_t k = n > 64 ? 64 : n;
  memcpy(frame + 1, ch, k);
  this->wire_write_(frame, k + 1, true);
  this->uart_->set_baud_rate(old_baud);
  this->uart_->load_settings(false);
}


// ===================== memory guard =====================
uint32_t WebSerial::largest_block_() {
#ifdef USE_ESP8266
  return ESP.getMaxFreeBlockSize();
#elif defined(USE_ESP32)
  return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
#else
  return this->free_heap_();
#endif
}
uint8_t WebSerial::frag_pct_() {
  uint32_t heap = this->free_heap_();
  if (heap == 0)
    return 0;
  uint32_t largest = this->largest_block_();
  if (largest >= heap)
    return 0;
  return (uint8_t) (100 - (largest * 100 / heap));  // 0 = one contiguous block
}
bool WebSerial::guard_ok_(size_t want) {
  // an allocation is allowed only if the LARGEST block would stay above the
  // platform floor afterwards (heap total lies on a fragmented ESP8266).
  uint32_t largest = this->largest_block_();
  if (largest < want)
    return false;
  return (largest - want) >= this->heap_floor_;
}

// ===================== port table =====================
int WebSerial::find_free_port_() {
  for (int i = 0; i < WSER_MAX_PORTS; i++)
    if (!this->ports_[i].used)
      return i;
  return -1;
}
int WebSerial::port_alloc_(PortType t, uint16_t net_port, uint16_t buf_cap, bool udp) {
  int i = this->find_free_port_();
  if (i < 0) {
    this->ws_send_text_("{\"t\":\"err\",\"msg\":\"port table full\"}");
    return -1;
  }
  // memory guard: net ports need an egress buffer -- refuse if it would break us
  if (buf_cap > 0) {
    if (buf_cap > WSER_PORT_BUF_MAX)
      buf_cap = WSER_PORT_BUF_MAX;
    if (!this->guard_ok_(buf_cap)) {
      this->ws_send_text_("{\"t\":\"err\",\"msg\":\"allocation refused: would drop largest block below the " +
                          std::to_string(this->heap_floor_ / 1024) + " KB safety floor. Lower the buffer or free a port.\"}");
      return -1;
    }
  }
  Port &p = this->ports_[i];
  p = Port{};
  p.used = true;
  p.type = t;
  p.vlan = 1;
  p.up = true;
  p.net_port = net_port;
  p.udp = udp;
  if (buf_cap > 0) {
    p.buf = (uint8_t *) malloc(buf_cap);
    if (p.buf == nullptr) {
      p.used = false;
      this->ws_send_text_("{\"t\":\"err\",\"msg\":\"malloc failed\"}");
      return -1;
    }
    p.buf_cap = buf_cap;
    this->buffers_alloc_ += buf_cap;
  }
  const char *tn = t == PT_UART ? "uart" : t == PT_CONSOLE ? "console" : t == PT_TCP ? "tcp" : t == PT_UDP ? "udp" : "bridge";
  if (net_port)
    snprintf(p.name, sizeof(p.name), "%s :%u", tn, net_port);
  else
    snprintf(p.name, sizeof(p.name), "%s", tn);
  return i;
}
void WebSerial::port_free_(int idx) {
  if (idx < 0 || idx >= WSER_MAX_PORTS || !this->ports_[idx].used)
    return;
  Port &p = this->ports_[idx];
  if (p.loop_peer >= 0 && p.loop_peer < WSER_MAX_PORTS &&
      this->ports_[p.loop_peer].loop_peer == idx)
    this->ports_[p.loop_peer].loop_peer = -1;   // never a dangling wire
  p.loop_peer = -1;
  p.hbrake = 0;
  if (p.buf) {
    this->buffers_alloc_ -= p.buf_cap;
    free(p.buf);
    p.buf = nullptr;
  }
  p.client = nullptr;
  p.listen = nullptr;
#ifdef USE_SOCKET_IMPL_LWIP_TCP
  p.wudp = nullptr;
  p.wpeer = wser::wser_endpoint{};
#endif
  p.used = false;
}
// MIRROR WARNING: this law and port_rate_ok_ below are mirrored line-for-
// line in tests/host/test_shaper.c. Change one, change both (LESSONS rule).
size_t WebSerial::egress_room_(Port &p) {
  if (p.out_cap == 0)
    return SIZE_MAX;              // unlimited: no bucket touched
  uint32_t now = millis();
  if (p.out_tok_ms == 0) { p.out_tok_ms = now; p.out_tok = p.out_cap; p.out_acc = 0; }
  uint32_t dt = now - p.out_tok_ms;   // unsigned delta: millis() wrap-safe (PIEGES 14)
  if (dt) {
    if (dt >= 1000) {
      // Long idle (or the once-per-49.7-days millis wrap landing here): a
      // full 1 s bucket, no more. Also clamps cap*dt away from overflow.
      p.out_tok = p.out_cap;
      p.out_acc = 0;
    } else {
      // Fractional refill: milli-tokens. NOTHING is lost to truncation --
      // the remainder survives in out_acc, so 'out 10' on a 5 ms drain
      // cadence releases 1 byte every 100 ms instead of starving forever.
      uint64_t a = (uint64_t) p.out_cap * dt + p.out_acc;
      uint32_t whole = (uint32_t) (a / 1000);
      p.out_acc = (uint32_t) (a % 1000);
      uint32_t cap = p.out_cap;   // 1 s bucket depth, same as ingress
      p.out_tok = (p.out_tok + whole > cap) ? cap : (p.out_tok + whole);
    }
    p.out_tok_ms = now;           // legitimate now: the remainder is stored
  }
  return p.out_tok;
}
void WebSerial::egress_spend_(Port &p, size_t n) {
  if (p.out_cap == 0)
    return;
  p.out_tok = n >= p.out_tok ? 0 : (uint32_t) (p.out_tok - n);
}

bool WebSerial::port_rate_ok_(Port &p, size_t n) {
  if (p.rate_cap == 0)
    return true;
  uint32_t now = millis();
  if (p.tok_ms == 0) { p.tok_ms = now; p.tok = p.rate_cap; p.rate_acc = 0; }
  uint32_t dt = now - p.tok_ms;       // unsigned delta: millis() wrap-safe (PIEGES 14)
  if (dt > 0) {
    if (dt >= 1000) {
      p.tok = p.rate_cap;             // idle/wrap: one full bucket, clamped
      p.rate_acc = 0;
    } else {
      // Same fractional law as egress: the truncation remainder survives,
      // so a small cap POLICES at its number instead of blocking everything.
      uint64_t a = (uint64_t) p.rate_cap * dt + p.rate_acc;
      uint32_t whole = (uint32_t) (a / 1000);
      p.rate_acc = (uint32_t) (a % 1000);
      uint32_t cap = p.rate_cap;      // 1 s bucket depth
      p.tok = (p.tok + whole > cap) ? cap : (p.tok + whole);
    }
    p.tok_ms = now;
  }
  if (p.tok >= n) { p.tok -= (uint32_t) n; return true; }
  // A chunk bigger than the whole bucket could NEVER pass -- permanent drop,
  // a livelock for that traffic. Policer convention: oversize passes when the
  // bucket is full (the line was idle long enough), and empties it.
  if (n > p.rate_cap && p.tok >= p.rate_cap) { p.tok = 0; return true; }
  return false;
}
void WebSerial::port_enqueue_(int idx, const uint8_t *d, size_t n) {
  Port &p = this->ports_[idx];
  if (p.buf == nullptr)
    return;
  if (p.buf_n + n > p.buf_cap) {   // best-effort: drop oldest
    size_t need = p.buf_n + n - p.buf_cap;
    if (need >= p.buf_n) { p.drop += p.buf_n; p.buf_n = 0; }        // count BEFORE zeroing
    else { memmove(p.buf, p.buf + need, p.buf_n - need); p.buf_n -= (uint16_t) need; p.drop += need; }
  }
  size_t k = n > p.buf_cap ? p.buf_cap : n;
  p.drop += n - k;                 // an oversize chunk's truncated head is dropped too
  memcpy(p.buf + p.buf_n, d + (n - k), k);
  p.buf_n += (uint16_t) k;
}

// ===================== the switch core =====================
// A byte block arriving on src_pi is forwarded to every UP port in the same
// VLAN, except the origin. UART egress writes the wire; net egress buffers;
// bridge egress streams to the browser. This is the Ethernet-hub rule.
void WebSerial::switch_ingress_(int src_pi, const uint8_t *d, size_t n) {
  uint8_t vlan = (src_pi >= 0) ? this->ports_[src_pi].vlan : this->console_vlan_;
  if (src_pi >= 0) {
    Port &sp = this->ports_[src_pi];
    if (!sp.up)
      return;
    // loop detect (passive): is this ingress an echo of our own recent egress?
    // ON only observes -- the stream is NEVER altered. KILL downs the port.
    if ((sp.type == PT_TCP || sp.type == PT_UDP || sp.type == PT_BRIDGE) && this->lp_ingress_(src_pi, d, n))
      return;  // KILL mode: the port just went DOWN, this looped chunk dies with it
    if (!this->port_rate_ok_(sp, n)) {   // storm control
      sp.drop += n;
      this->flood_ = true;
      return;
    }
#ifdef USE_ESP8266
    if (this->radio_floor_ != 0 && this->largest_cache_ != 0 && this->largest_cache_ < this->radio_floor_) {
      // radio heap brake: below the floor, loopy traffic dies at the door
      // so lwip's pbuf allocations never do (wDev_ProcessFiq wedge class)
      sp.hbrake += n;   // every extinguisher signs its catches (conservation ledger)
      this->flood_ = true;
      return;
    }
#endif
    sp.rx += n;
  }
  WSER_CRUMB(0x50);
  this->switched_bytes_ += (uint32_t) n;   // the true work meter: boundary AND internal
  bool wire_hit = false, cons_hit = false;
  for (int i = 0; i < WSER_MAX_PORTS; i++) {
    Port &q = this->ports_[i];
    // hairpin exception: a SELF-WIRED bridge (loop_peer == itself) is the
    // one port allowed to receive its own origin -- that is its definition
    if (!q.used || (i == src_pi && q.loop_peer != i) || !q.up || q.vlan != vlan)
      continue;
    q.tx += n;
    if (q.type == PT_UART) {
      if (q.buf != nullptr)
        this->port_enqueue_(i, d, n);   // drained by the per-pass baud quota
      else
        this->wire_write_(d, n, true, src_pi);   // no-buffer fallback: direct
      wire_hit = true;
    } else if (q.type == PT_CONSOLE) {
      cons_hit = true;
      this->cons_record_((uint8_t) src_pi, d, n);  // RAW stream: the WHOLE vlan, no dedup
    } else if (q.type == PT_BRIDGE) {
      if (i == this->wire_src_ && q.loop_peer != i)
        continue;   // origin rule traverses the wire: NEVER back into the pair
                    // (except the declared hairpin, which IS its own pair)
      this->lp_egress_(q, d, n);   // remember what we sent this port
      if (q.loop_peer >= 0)
        this->port_enqueue_(i, d, n);   // TRUNK: RAM null-modem, drained governed
      else
        this->bridge_push_(i, d, n);
    } else {  // TCP/UDP
      this->lp_egress_(q, d, n);
      this->port_enqueue_(i, d, n);
    }
  }
  // Console egress used to be a BLACK HOLE: counted (tx grew) then discarded
  // (port_enqueue_ on a nullptr buffer). Invisible whenever the console's
  // vlan excludes the uart -- the web log only ever showed the wire tap.
  // Render it -- but only when the tap will NOT already show these bytes:
  // wire-origin ingress is rendered by the tap as RX, and anything that also
  // egressed to the uart shows up as TX self. Rendering those again would
  // double every line of the historical single-view behavior.
  if (cons_hit && !wire_hit && src_pi != this->uart_pi_)
    this->console_push_(src_pi, d, n);
}


// egress to the web console: best-effort, same backlog policy as the bridge
void WebSerial::console_push_(int src_pi, const uint8_t *d, size_t n) {
  if (this->stream_client_ == nullptr || this->serving_page_)
    return;
  if (this->out_.size() - this->out_pos_ > WSER_LOG_BACKLOG) {
    this->ws_drop_++;   // refused upstream (guard predates the audit -- good
    return;             // reflex); now it COUNTS like every other drop
  }
  size_t k = n > 32 ? 32 : n;
  const char *src = (src_pi >= 0 && this->ports_[src_pi].used) ? this->ports_[src_pi].name : "?";
  std::string j = "{\"t\":\"cons\",\"src\":\"" + std::string(src) + "\",\"n\":" + std::to_string(n) +
                  ",\"b\":\"" + hex_join(d, k) + "\"}";
  this->ws_send_text_(j);
}

// ===================== loop detection (PASSIVE echo fingerprinting) =====================
// A serial hub can't run STP (no addresses, no BPDUs) and it must NEVER
// corrupt the stream -- not the wire, not the PC bridge, not TCP/UDP clients.
// So nothing is injected, ever. Instead: the hub FINGERPRINTS what it emits
// to each network port (FNV-1a hash + length + timestamp, a 4-slot ring,
// ~40 B/port). If what comes IN on that same port matches something we sent
// IT within the last 3 s -- three consecutive chunks in a row -- then by
// definition an external path is feeding this port's egress back into its
// ingress: a loop. That is exactly the tcp<->bridge piping case: our bytes
// out the bridge come home through the TCP client. ON = sticky badge + log,
// stream untouched. KILL = the guilty port also goes DOWN. The UART is
// exempt: the TX->RX jumper of checklist B is a deliberate, useful echo.
// Trade-off vs an active probe: an IDLE loop is invisible -- but an idle
// loop is also harmless; the moment traffic circulates, it is caught within
// three round trips. Chunks under 2 bytes are ignored (coincidence guard);
// TCP re-fragmentation breaks a match toward a MISS, never a false alarm.
static uint32_t lp_fnv1a_(const uint8_t *d, size_t n) {
  uint32_t h = 0x811C9DC5u;
  for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 0x01000193u; }
  return h;
}

void WebSerial::lp_egress_(Port &p, const uint8_t *d, size_t n) {
  if ((this->loopdet_ == 0 && this->xvlandet_ == 0) || n < 2)
    return;
  if (p.type == PT_CONSOLE)
    return;  // the web log is display-only: nothing can come back from it
  Port::LpFp &f = p.lp_ring[p.lp_w];
  f.h = lp_fnv1a_(d, n);
  f.len = (uint16_t) (n > 0xFFFF ? 0xFFFF : n);
  f.ms = millis();
  p.lp_w = (uint8_t) ((p.lp_w + 1) & 3);
}

bool WebSerial::lp_ingress_(int pi, const uint8_t *d, size_t n) {
  if (this->loopdet_ == 0 && this->xvlandet_ == 0)
    return false;
  Port &p = this->ports_[pi];
  if (n < 2)
    return false;  // too short to judge; doesn't touch the streaks
  uint32_t h = lp_fnv1a_(d, n);
  uint32_t now = millis();
  // One engine, two verdicts. The fingerprint that matches tells us WHICH
  // port our copy left by, hence which vlan. Same vlan as the ingress port
  // -> the frame came back where it started: recirculation, a LOOP. Other
  // vlan -> it crossed once and dies there: an inter-VLAN BRIDGE (isolation
  // broken). Physics does the classifying, not policy. Vlans are read at
  // scan time (hot PORT VLAN changes purge the port's ring to close the 3 s
  // stale window). Scan order: same-vlan first, cross second; a disabled
  // detector's pass is skipped ENTIRELY -- no count, no consumption -- so a
  // tolerated bridge (XVLANDETECT OFF) can't burn the vouchers a live loop
  // in either vlan still needs.
  int verdict = 0;  // 0 none, 1 loop (same vlan), 2 xvlan (cross)
  for (int pass = 1; pass <= 2 && verdict == 0; pass++) {
    if (pass == 1 && this->loopdet_ == 0)
      continue;
    if (pass == 2 && this->xvlandet_ == 0)
      continue;
    if (pass == 2 && this->wire_ctx_)
      continue;   // a DECLARED crossing (trunk) is exempt; clandestine ones still hunted
    for (int q = 0; q < WSER_MAX_PORTS && verdict == 0; q++) {
      Port &src = this->ports_[q];
      if (!src.used)
        continue;
      bool same = src.vlan == p.vlan;
      if ((pass == 1) != same)
        continue;
      for (int k = 0; k < 4; k++) {
        Port::LpFp &f = src.lp_ring[k];
        if (f.ms != 0 && now - f.ms <= 3000 && f.len == n && f.h == h) {
          f.ms = 0;  // consume: one egress vouches for one ingress
          verdict = pass;
          break;
        }
      }
    }
  }
  // Per-chunk classification: each verdict feeds its own streak and resets
  // the other (interleaved evidence restarts both counts -- conservative).
  if (verdict == 0) {
    p.lp_streak = 0;
    p.xv_streak = 0;
    return false;
  }
  if (verdict == 1) {
    p.xv_streak = 0;
    if (++p.lp_streak < 3)
      return false;
    p.lp_streak = 0;
    bool first = !p.lp;   // EDGE: the badge is sticky -- log the transition,
                          // never the re-verdicts (each W-line costs ~5 ms of
                          // synchronous UART0 at 115200 + a Noise-encrypted
                          // API push: the log storm was the loop-time killer)
    p.lp = true;    // sticky; cleared by PORT UP / PORT RESET / PORT VLAN
    this->loop_pi_ = pi;
    if (this->loopdet_ == 2) {
      p.up = false;
      ESP_LOGW(TAG, "loop detected on port %d -- port DOWN (KILL)", pi);
      this->ws_send_text_("{\"t\":\"err\",\"msg\":\"LOOP on port " + std::to_string(pi) + " -- port DOWN (KILL)\"}");
      this->info_pending_ = true;
      return true;  // this looped chunk dies with the port
    }
    if (first) {
      ESP_LOGW(TAG, "loop detected on port %d", pi);
      this->ws_send_text_("{\"t\":\"err\",\"msg\":\"LOOP detected on port " + std::to_string(pi) + "\"}");
      this->info_pending_ = true;
    }
    return false;
  }
  // verdict == 2: inter-VLAN bridge
  p.lp_streak = 0;
  if (++p.xv_streak < 3)
    return false;
  p.xv_streak = 0;
  bool xfirst = !p.xlp;   // EDGE, same reason as the loop verdict
  p.xlp = true;     // sticky; cleared by PORT UP / PORT RESET / PORT VLAN
  if (this->xvlandet_ == 2) {
    p.up = false;
    ESP_LOGW(TAG, "inter-VLAN bridge into port %d -- port DOWN (KILL)", pi);
    this->ws_send_text_("{\"t\":\"err\",\"msg\":\"XVLAN bridge into port " + std::to_string(pi) + " -- port DOWN (KILL)\"}");
    this->info_pending_ = true;
    return true;
  }
  if (xfirst) {
    ESP_LOGW(TAG, "inter-VLAN bridge into port %d", pi);
    this->ws_send_text_("{\"t\":\"err\",\"msg\":\"XVLAN bridge detected into port " + std::to_string(pi) + "\"}");
    this->info_pending_ = true;
  }
  return false;     // ON: observe only, never alter the stream
}

// ===================== net port service (TCP + UDP) =====================
void WebSerial::port_net_loop_(int idx) {
  Port &p = this->ports_[idx];
  if (!p.used || (p.type != PT_TCP && p.type != PT_UDP))
    return;
#ifdef USE_SOCKET_IMPL_LWIP_TCP
  if (p.udp) {
    // ---- ESP8266 raw-lwip UDP: the wser module owns this path entirely.
    // The BSD block below stays byte-identical for ESP32; on lwip builds
    // it simply never sees a udp port. Same 5s/3-strikes backoff, same
    // peer learning, same txerr/self-recreation semantics as the BSD arm.
    if (p.wudp == nullptr) {
      uint32_t now = millis();
      if (p.tok_ms != 0 && now - p.tok_ms < 5000 && p.tok >= 1)
        return;
      auto *ws = new (std::nothrow) wser::WserUdpSocket();
      if (ws == nullptr || !ws->bind(p.net_port)) {
        delete ws;
        p.tok_ms = now;
        p.tok++;
        if (p.tok >= 3) {
          p.up = false;
          p.tok = 0;
          ESP_LOGW(TAG, "port %d: udp bind on %u keeps failing -- port DOWN", idx, p.net_port);
          this->ws_send_text_("{\"t\":\"err\",\"msg\":\"port " + std::to_string(idx) + ": udp bind on " + std::to_string(p.net_port) + " failed 3x -- port DOWN\"}");
          this->info_pending_ = true;
        }
        return;
      }
      p.wudp.reset(ws);
      p.tok = 0;
      p.tok_ms = 0;  // hand tok/tok_ms back to the rate limiter
    }
    for (int burst = 0; burst < 8; burst++) {
      uint8_t buf[128];
      wser::wser_endpoint from;
      long r = p.wudp->recvfrom(buf, sizeof(buf), &from);
      if (r <= 0)
        break;
      p.have_peer = true;
      p.wpeer = from;
      this->switch_ingress_(idx, buf, (size_t) r);
    }
    if (p.buf_n > 0 && p.have_peer && network::is_connected()) {
      // network gate (field lesson: WiFi outage => EVERY sendto fails hard
      // => txfail hits 5 => recreate => bind succeeds locally => fail =>
      // recreate... a pcb-churn + log storm at several Hz DURING the exact
      // window the chip needs CPU to re-associate. Down network = egress
      // suspended, txfail frozen, silence.)
      size_t want_u = p.buf_n;
      {
        size_t room = this->egress_room_(p);
        if (room < want_u)
          want_u = room;
      }
      if (want_u == 0)
        goto wser_egress_done;   // scoped skip: the rest of the branch still runs
      long w = p.wudp->sendto(p.buf, want_u, p.wpeer);
      if (w > 0) {
        this->egress_spend_(p, (size_t) w);
        memmove(p.buf, p.buf + w, p.buf_n - w);
        p.buf_n -= (uint16_t) w;
        p.txfail = 0;
      } else if (w < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
        p.txerr++;
        if (p.txfail == 0)
          ESP_LOGW(TAG, "port %d: udp sendto failed, errno=%d", idx, errno);
        if (++p.txfail >= 5) {
          ESP_LOGW(TAG, "port %d: recreating the udp socket after %u send failures", idx, (unsigned) p.txfail);
          p.wudp = nullptr;
          p.txfail = 0;
          p.tok = 1;            // arm the creation backoff: recreations are
          p.tok_ms = millis();  // spaced 5 s apart, never a tight churn loop
        }
      }
    }
wser_egress_done:;
    return;
  }
#endif
  if (p.listen == nullptr) {
    // bind-retry backoff: a failing bind (duplicate port, stack not ready)
    // used to recreate a socket EVERY loop pass -- churn, silently, forever.
    // Now: retry at most every 5 s, and after 3 failures the port goes DOWN
    // with one err message so the operator actually learns about it.
    uint32_t now = millis();
    if (p.tok_ms != 0 && now - p.tok_ms < 5000 && p.tok >= 1)
      return;  // (tok/tok_ms double as bind-retry state while listen==nullptr)
    p.listen = socket::socket_ip_loop_monitored(p.udp ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (p.listen == nullptr) {
      // creation failure (unsupported type, OOM): same 5s/3-strikes backoff
      // as bind -- without it this retried and ESP_LOGE'd EVERY loop pass
      p.tok_ms = now;
      p.tok++;
      if (p.tok >= 3) {
        p.up = false;
        p.tok = 0;
        ESP_LOGW(TAG, "port %d: socket creation keeps failing -- port DOWN", idx);
        this->ws_send_text_("{\"t\":\"err\",\"msg\":\"port " + std::to_string(idx) + ": socket creation failed 3x -- port DOWN\"}");
        this->info_pending_ = true;
      }
      return;
    }
    struct sockaddr_storage sa;
    socklen_t sl = socket::set_sockaddr_any(reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa), p.net_port);
    int en = 1;
    p.listen->setsockopt(SOL_SOCKET, SO_REUSEADDR, &en, sizeof(int));
    p.listen->setblocking(false);
    if (p.listen->bind(reinterpret_cast<struct sockaddr *>(&sa), sl) != 0) {
      p.listen = nullptr;
      p.tok_ms = now;
      p.tok++;
      if (p.tok >= 3) {
        p.up = false;
        p.tok = 0;
        ESP_LOGW(TAG, "port %d: bind on %u keeps failing -- port DOWN", idx, p.net_port);
        this->ws_send_text_("{\"t\":\"err\",\"msg\":\"port " + std::to_string(idx) + ": bind on " + std::to_string(p.net_port) + " failed 3x -- port DOWN\"}");
        this->info_pending_ = true;
      }
      return;
    }
    p.tok = 0;
    p.tok_ms = 0;  // hand tok/tok_ms back to the rate limiter
    if (!p.udp)
      p.listen->listen(2);
  }
  if (p.udp) {
    // UDP: one socket both ways; learn the peer from the first datagram.
    // Bounded burst drain: one datagram per loop pass caps ingress at
    // ~50 dgram/s on a 20 ms loop -- far under wire rate. 8 per pass keeps
    // the loop bounded and the pipe fed.
    for (int burst = 0; burst < 8; burst++) {
      uint8_t buf[128];
      struct sockaddr_storage from;
      socklen_t fl = sizeof(from);
      ssize_t r = p.listen->recvfrom(buf, sizeof(buf), reinterpret_cast<struct sockaddr *>(&from), &fl);
      if (r <= 0)
        break;
      p.have_peer = true;
      p.peer_len = (uint8_t) (fl < sizeof(p.peer_addr) ? fl : sizeof(p.peer_addr));
      memcpy(&p.peer_addr, &from, p.peer_len);
      this->switch_ingress_(idx, buf, (size_t) r);
    }
    if (p.buf_n > 0 && p.have_peer && network::is_connected()) {
      // network gate + spaced recreation: same field lesson as the wser arm
      struct sockaddr *pa = reinterpret_cast<struct sockaddr *>(&p.peer_addr);
      size_t want_b = p.buf_n;
      {
        size_t room = this->egress_room_(p);
        if (room < want_b)
          want_b = room;
      }
      if (want_b == 0)
        goto bsd_egress_done;    // scoped skip, same landmine rule
      ssize_t w = p.listen->sendto(p.buf, want_b, 0, pa, p.peer_len);
      if (w > 0) {
        this->egress_spend_(p, (size_t) w);
        memmove(p.buf, p.buf + w, p.buf_n - w);
        p.buf_n -= (uint16_t) w;
        p.txfail = 0;  // a success ends the failure run
      } else if (w < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
        p.txerr++;
        if (p.txfail == 0)  // log the FIRST failure of a run, with its errno
          ESP_LOGW(TAG, "port %d: udp sendto failed, errno=%d", idx, errno);
        if (++p.txfail >= 5) {
          // 5 consecutive hard failures: some stacks latch an ICMP error on
          // the socket (ECONNREFUSED forever after one port-unreachable).
          // Recreate it -- same local port, learned peer kept (it lives in
          // the Port, not the socket). If recreation itself fails, the
          // existing creation backoff takes over (5s/3 strikes -> DOWN).
          ESP_LOGW(TAG, "port %d: recreating the udp socket after %u send failures", idx, (unsigned) p.txfail);
          p.listen = nullptr;
          p.txfail = 0;
          p.tok = 1;            // same spacing as the wser arm: 5 s between
          p.tok_ms = millis();  // recreations, the backoff gate does the rest
        }
      }
    }
bsd_egress_done:;
    return;
  }
  // TCP
  auto s = p.listen->accept_loop_monitored(nullptr, nullptr);
  if (s != nullptr) { s->setblocking(false); p.client = std::move(s); p.buf_n = 0; }
  if (p.client == nullptr)
    return;
  // Bounded burst drain: 64 B once per ~20 ms loop is ~3 KB/s -- LESS than
  // 115200 baud. Up to 8 reads per pass lifts the ceiling to ~25 KB/s while
  // keeping worst-case loop time bounded.
  for (int burst = 0; burst < 8; burst++) {
    uint8_t buf[64];
    ssize_t r = p.client->read(buf, sizeof(buf));
    if (r > 0) {
      this->switch_ingress_(idx, buf, (size_t) r);
      continue;
    }
    if (r == 0) { p.client = nullptr; return; }
    break;  // EWOULDBLOCK or error: stop draining this pass
  }
  if (p.buf_n > 0) {
    size_t want_tcp = p.buf_n;
    {
      size_t room = this->egress_room_(p);
      if (room < want_tcp)
        want_tcp = room;
    }
    if (want_tcp > 0) {  // scoped skip, never an early return (landmine rule)
      ssize_t w = p.client->write(p.buf, want_tcp);
      if (w > 0) { this->egress_spend_(p, (size_t) w); memmove(p.buf, p.buf + w, p.buf_n - w); p.buf_n -= (uint16_t) w; }
    }
  }
}

void WebSerial::bridge_push_(int port_id, const uint8_t *d, size_t n) {
  if (this->stream_client_ == nullptr || this->serving_page_)
    return;
  {
    Port &bp = this->ports_[port_id];
    size_t room = this->egress_room_(bp);
    if (room == 0) {
      bp.drop += n;               // shaped out entirely: counted here
      return;
    }
    if (room < n) {
      bp.drop += n - room;        // partial: stream semantics, tail dropped
      n = room;
    }
    this->egress_spend_(bp, n);
  }
  if (this->out_.size() - this->out_pos_ > WSER_LOG_BACKLOG)
    return;  // best-effort
  uint8_t buf[66];
  buf[0] = 0x02;
  // id unification: BOTH directions speak in bridge SLOT index (0-3), the
  // same space handle_ws_frame_ resolves 0x01 frames with. Emitting the raw
  // switch index here only ever worked because the old single-port JS
  // ignored the byte; a real demux needs one id space.
  int slot = -1;
  for (uint8_t k = 0; k < 4; k++)
    if (this->bridge_pi_[k] == port_id) { slot = k; break; }
  if (slot < 0)
    return;  // a bridge port with no slot has no COM behind it: never default to slot 0
  buf[1] = (uint8_t) slot;
  while (n > 0) {
    size_t k = n > 64 ? 64 : n;
    memcpy(buf + 2, d, k);
    this->ws_send_(ws::OP_BIN, buf, k + 2);
    d += k; n -= k;
  }
}

// ---- commands ----
void WebSerial::handle_command_(const std::string &cmd) {
  const char *p = cmd.c_str();
  if (!skip_to_token(p))
    return;
  if (strncmp(p, "TX", 2) == 0 && (p[2] == ' ' || p[2] == 0)) {
    p += 2;
    uint8_t w[128];
    size_t nw = 0;
    while (skip_to_token(p) && nw < sizeof(w)) {
      uint32_t v = 0;
      if (!wser_read_hexv(p, v))
        break;
      w[nw++] = (uint8_t) v;
    }
    if (nw == 0) {
      this->ws_send_text_("{\"t\":\"err\",\"msg\":\"TX needs hex bytes\"}");
      return;
    }
    if (this->console_pi_ >= 0) {
      // single source of truth: the console PORT's vlan field. CVLAN and the
      // VLAN pill both write it (keeping console_vlan_ as a synced mirror for
      // the UI selector + persistence). The old re-sync here stomped a pill
      // change back to the stale mirror on every console TX.
      this->switch_ingress_(this->console_pi_, w, nw);
      // VLAN-isolation feedback: the log only shows WIRE traffic, so a TX
      // that never reaches the uart is otherwise perfectly silent -- the
      // operator types into the void with zero feedback. Count where this
      // send could actually go and say so when the topology ate it.
      uint8_t cv = this->ports_[this->console_pi_].vlan;
      int others = 0;
      bool wire = false;
      for (int q = 0; q < WSER_MAX_PORTS; q++) {
        Port &pp = this->ports_[q];
        if (!pp.used || q == this->console_pi_ || !pp.up || pp.vlan != cv)
          continue;
        others++;
        if (pp.type == PT_UART)
          wire = true;
      }
      if (others == 0) {
        this->ws_send_text_("{\"t\":\"err\",\"msg\":\"TX went nowhere: the console is alone in VLAN " + std::to_string(cv) + "\"}");
      } else if (!wire) {
        this->ws_send_text_("{\"t\":\"ok\",\"msg\":\"TX to " + std::to_string(others) + " port(s) in VLAN " + std::to_string(cv) + " -- the uart is elsewhere, nothing hit the wire\"}");
      }
    }
    return;
  }
  if (strncmp(p, "DMXTX", 5) == 0) {
    p += 5;
    uint8_t ch[64];
    size_t n = 0;
    while (skip_to_token(p) && n < sizeof(ch)) {
      uint32_t v = 0;
      if (!wser_read_hexv(p, v))
        break;
      ch[n++] = (uint8_t) v;
    }
    if (n == 0) {
      this->ws_send_text_("{\"t\":\"err\",\"msg\":\"DMXTX needs channel bytes\"}");
      return;
    }
    this->dmx_tx_(ch, n);
    this->ws_send_text_("{\"t\":\"ok\",\"msg\":\"DMX frame sent (experimental break timing)\"}");
    return;
  }
  if (strncmp(p, "LINE", 4) == 0) {  // LINE <baud> <8N1|7E2...>
    p += 4;
    uint32_t baud = 0;
    if (skip_to_token(p))
      read_dec(p, baud);
    if (baud < 300 || baud > 2000000) {
      this->ws_send_text_("{\"t\":\"err\",\"msg\":\"baud out of range\"}");
      return;
    }
    char db = '8', par = 'N', sb = '1';
    if (skip_to_token(p)) {
      db = p[0];
      if (p[0] && p[1]) { par = p[1]; if (p[2]) sb = p[2]; }
    }
    this->uart_->set_baud_rate(baud);
    this->uart_->set_data_bits(db == '7' ? 7 : 8);
    this->uart_->set_parity(par == 'E' ? uart::UART_CONFIG_PARITY_EVEN : par == 'O' ? uart::UART_CONFIG_PARITY_ODD : uart::UART_CONFIG_PARITY_NONE);
    this->uart_->set_stop_bits(sb == '2' ? 2 : 1);
    this->uart_->load_settings(false);
    this->ws_send_text_("{\"t\":\"ok\",\"msg\":\"line: " + std::to_string(baud) + " " + db + par + sb + "\"}");
    this->info_pending_ = true;
    return;
  }
  if (strncmp(p, "FRAME", 5) == 0) {  // FRAME <gap_ms> <delim hex | ->
    p += 5;
    uint32_t g = 10;
    if (skip_to_token(p))
      read_dec(p, g);
    if (g < 1) g = 1;
    if (g > 1000) g = 1000;
    this->gap_ms_ = (uint16_t) g;
    this->delim_ = 0;
    if (skip_to_token(p) && *p != '-') {
      uint32_t v = 0;
      if (wser_read_hexv(p, v))
        this->delim_ = (uint8_t) v;
    }
    this->ws_send_text_("{\"t\":\"ok\",\"msg\":\"framing: gap " + std::to_string(this->gap_ms_) + " ms" +
                        (this->delim_ ? " + delimiter" : "") + "\"}");
    return;
  }
  if (strncmp(p, "BRIDGE", 6) == 0) {  // legacy alias of PORT ADD BRIDGE (same checks)
    this->handle_command_("PORT ADD BRIDGE");
    return;
  }
  if (strncmp(p, "PORT", 4) == 0) {
    p += 4;
    if (!skip_to_token(p)) return;
    if (strncmp(p, "ADD", 3) == 0) {
      p += 3;
      if (!skip_to_token(p)) return;
      bool udp = strncmp(p, "UDP", 3) == 0;
      bool bridge = strncmp(p, "BRIDGE", 6) == 0;
      // TRUNK mode: "PORT ADD BRIDGE TRUNK" = a slot-LESS bridge socket for
      // the virtual wire. The bare command keeps its historical contract
      // (Connect flow: slot assigned, t:"bridge" reply) untouched.
      bool trunk = bridge && strstr(p, "TRUNK") != nullptr;
      while (*p && *p != ' ') p++;
      uint32_t np = 0, buf = WSER_PORT_BUF_DEF;
      if (!bridge && skip_to_token(p)) read_dec(p, np);
      if (trunk && skip_to_token(p)) { while (*p && *p != ' ') p++; }  // consume the TRUNK keyword; an optional buffer size follows it
      if (skip_to_token(p)) { uint32_t b; if (read_dec(p, b)) buf = b; }
      if (!bridge) {
        // (UDP on raw-lwip is served by the wser module -- no refusal)
        if (np < 1 || np > 65535) {
          this->ws_send_text_("{\"t\":\"err\",\"msg\":\"net port must be 1-65535\"}");
          return;
        }
        if ((uint16_t) np == this->port_) {
          this->ws_send_text_("{\"t\":\"err\",\"msg\":\"that is the web UI port\"}");
          return;
        }
        for (int q = 0; q < WSER_MAX_PORTS; q++)
          if (this->ports_[q].used && this->ports_[q].net_port == (uint16_t) np &&
              this->ports_[q].udp == udp) {
            this->ws_send_text_("{\"t\":\"err\",\"msg\":\"port " + std::to_string(np) + " already in use by port " + std::to_string(q) + "\"}");
            return;
          }
      }
      int i;
      if (bridge && trunk) {
        i = this->port_alloc_(PT_BRIDGE, 0, (uint16_t) buf, false);  // buf is the New-buffer field (clamped to WSER_PORT_BUF_MAX + heap-guarded inside port_alloc_); bare "TRUNK" falls back to WSER_PORT_BUF_DEF
        if (i >= 0) {
          this->ws_send_text_("{\"t\":\"ok\",\"msg\":\"trunk socket: port " + std::to_string(i) +
                              " added (no COM) -- wire two together with the wire \u21c4 pill\"}");
          this->info_pending_ = true;
        } else {
          this->ws_send_text_("{\"t\":\"err\",\"msg\":\"port table full\"}");
        }
      } else if (bridge) {
        i = this->port_alloc_(PT_BRIDGE, 0, 0, false);
        if (i >= 0) {
          int slot = -1;
          for (int k = 0; k < 4; k++) if (this->bridge_pi_[k] < 0) { this->bridge_pi_[k] = i; slot = k; break; }
          if (slot < 0) { this->port_free_(i); i = -1;
            this->ws_send_text_("{\"t\":\"err\",\"msg\":\"all 4 bridge slots taken\"}");
          } else {
            // structured reply: the JS binds its freshly-opened COM port to this slot
            this->ws_send_text_("{\"t\":\"bridge\",\"pi\":" + std::to_string(i) + ",\"slot\":" + std::to_string(slot) + "}");
          }
        }
      } else {
        i = this->port_alloc_(udp ? PT_UDP : PT_TCP, (uint16_t) np, (uint16_t) buf, udp);
      }
      if (i >= 0 && !bridge)  // bridge already answered with t:"bridge"
        this->ws_send_text_("{\"t\":\"ok\",\"msg\":\"port " + std::to_string(i) + " added\"}");
      else if (i < 0 && !bridge)
        // own memo, trap #33: a silent refusal is debugging debt. The full
        // table refused +TCP without a word and looked like a mystery.
        this->ws_send_text_("{\"t\":\"err\",\"msg\":\"port table full (" + std::to_string(WSER_MAX_PORTS) + "/" + std::to_string(WSER_MAX_PORTS) + ") -- delete a port first\"}");
    } else if (strncmp(p, "DEL", 3) == 0) {
      p += 3; uint32_t id = 0;
      if (skip_to_token(p) && read_dec(p, id) && id < WSER_MAX_PORTS) {
        if ((int) id == this->uart_pi_ || (int) id == this->console_pi_) {
          this->ws_send_text_("{\"t\":\"err\",\"msg\":\"cannot delete a fixed port\"}");
        } else {
          for (int k = 0; k < 4; k++) if (this->bridge_pi_[k] == (int) id) this->bridge_pi_[k] = -1;
          this->port_free_((int) id);
          this->ws_send_text_("{\"t\":\"ok\",\"msg\":\"port deleted\"}");
          // structured notice: the browser closes any COM link bound here --
          // the zombie-link class (pumping into a dead pi) dies with this
          this->ws_send_text_("{\"t\":\"pdel\",\"pi\":" + std::to_string(id) + "}");
        }
      }
    } else if (strncmp(p, "VLAN", 4) == 0) {
      p += 4; uint32_t id = 0, v = 0;
      if (skip_to_token(p) && read_dec(p, id) && skip_to_token(p) && read_dec(p, v) && id < WSER_MAX_PORTS && this->ports_[id].used && v >= 1 && v <= WSER_VLANS) {
        Port &pp = this->ports_[id];
        if (pp.vlan != (uint8_t) v) {
          // a vlan change is a topology change: detection restarts from zero
          // on this port (also closes the 3 s window where ring entries
          // recorded under the old vlan would be classified under the new)
          pp.lp = false; pp.xlp = false; pp.lp_streak = 0; pp.xv_streak = 0;
          for (int k = 0; k < 4; k++) pp.lp_ring[k].ms = 0;
        }
        pp.vlan = (uint8_t) v;
        if ((int) id == this->console_pi_)
          this->console_vlan_ = (uint8_t) v;  // the pill and the selector are two views of one field
      }
    } else if (strncmp(p, "UP", 2) == 0 || strncmp(p, "DOWN", 4) == 0) {
      bool up = p[0] == 'U'; p += up ? 2 : 4; uint32_t id = 0;
      if (skip_to_token(p) && read_dec(p, id) && id < WSER_MAX_PORTS && this->ports_[id].used) {
        this->ports_[id].up = up;
        if (up) { Port &pp = this->ports_[id]; pp.lp = false; pp.xlp = false; pp.lp_streak = 0; pp.xv_streak = 0; }  // fresh start clears both badges
      }
    } else if (strncmp(p, "RATE", 4) == 0) {
      p += 4; uint32_t id = 0, r = 0;
      if (skip_to_token(p) && read_dec(p, id) && skip_to_token(p) && read_dec(p, r) && id < WSER_MAX_PORTS && this->ports_[id].used)
        this->ports_[id].rate_cap = r;
    } else if (strncmp(p, "ADOPT", 5) == 0) {
      // re-link: bind an EXISTING slot-less, un-wired bridge to a free COM
      // slot -- the browser re-attaches an open link after a hub reboot
      // instead of delete-and-recreate.
      p += 5; uint32_t id = 0;
      if (skip_to_token(p) && read_dec(p, id) && id < WSER_MAX_PORTS && this->ports_[id].used) {
        Port &pt = this->ports_[id];
        bool slotted = false;
        for (int k = 0; k < 4; k++)
          if (this->bridge_pi_[k] == (int) id)
            slotted = true;
        if (pt.type != PT_BRIDGE)
          this->ws_send_text_("{\"t\":\"err\",\"msg\":\"adopt: not a bridge port\"}");
        else if (slotted)
          this->ws_send_text_("{\"t\":\"err\",\"msg\":\"adopt: already bound to a COM slot\"}");
        else if (pt.loop_peer >= 0)
          this->ws_send_text_("{\"t\":\"err\",\"msg\":\"adopt: port is wired as a trunk\"}");
        else {
          int slot = -1;
          for (int k = 0; k < 4; k++)
            if (this->bridge_pi_[k] < 0) { this->bridge_pi_[k] = (int) id; slot = k; break; }
          if (slot < 0)
            this->ws_send_text_("{\"t\":\"err\",\"msg\":\"adopt: all 4 bridge slots taken\"}");
          else {
            this->ws_send_text_("{\"t\":\"bridge\",\"pi\":" + std::to_string(id) + ",\"slot\":" + std::to_string(slot) + "}");
            this->info_pending_ = true;
          }
        }
      }
    } else if (strncmp(p, "WIRE", 4) == 0) {
      p += 4; uint32_t a = 0; int32_t b = 0;
      bool okb = false;
      if (skip_to_token(p) && read_dec(p, a) && skip_to_token(p)) {
        if (*p == '-') { p++; uint32_t t = 0; if (read_dec(p, t)) { b = -(int32_t) t; okb = true; } }
        else { uint32_t t = 0; if (read_dec(p, t)) { b = (int32_t) t; okb = true; } }
      }
      if (okb && a < WSER_MAX_PORTS && this->ports_[a].used) {
        Port &pa = this->ports_[a];
        if (b < 0) {
          if (pa.loop_peer >= 0 && pa.loop_peer < WSER_MAX_PORTS)
            this->ports_[pa.loop_peer].loop_peer = -1;
          pa.loop_peer = -1;
          this->save_config_();
          this->ws_send_text_("{\"t\":\"ok\",\"msg\":\"trunk unplugged\"}");
        } else if ((uint32_t) b >= WSER_MAX_PORTS || !this->ports_[b].used) {
          this->ws_send_text_("{\"t\":\"err\",\"msg\":\"wire: invalid peer\"}");
        } else if (pa.type != PT_BRIDGE || this->ports_[b].type != PT_BRIDGE) {
          this->ws_send_text_("{\"t\":\"err\",\"msg\":\"wire: both ends must be bridge ports\"}");
        } else {
          bool slotted = false;
          for (int k = 0; k < 4; k++)
            if (this->bridge_pi_[k] == (int) a || this->bridge_pi_[k] == b)
              slotted = true;
          if (slotted) {
            this->ws_send_text_("{\"t\":\"err\",\"msg\":\"wire: end already bound to a browser COM slot\"}");
          } else if (pa.loop_peer >= 0 || this->ports_[b].loop_peer >= 0) {
            this->ws_send_text_("{\"t\":\"err\",\"msg\":\"wire: an end is already wired\"}");
          } else if ([&]() {  // field bug: legacy Connect-era bridges have NO
            // buffer (buf_cap 0) -- wiring one made a silent black hole
            // (port_enqueue_ drops at line one, tx 0 rx 0, not a word).
            // Retrofit both ends; refuse if the heap cannot afford it.
            for (int e = 0; e < 2; e++) {
              Port &pe = this->ports_[e == 0 ? (int) a : b];
              if (pe.buf == nullptr) {
                if (!this->guard_ok_(256))
                  return true;   // not enough heap: refuse the wire
                pe.buf = (uint8_t *) malloc(256);
                if (pe.buf == nullptr)
                  return true;
                pe.buf_cap = 256;
                pe.buf_n = 0;
                this->buffers_alloc_ += 256;
              }
            }
            return false;
          }()) {
            this->ws_send_text_("{\"t\":\"err\",\"msg\":\"wire: could not give both ends a 256 B buffer (heap floor)\"}");
          } else {
            pa.loop_peer = (int8_t) b;
            this->ports_[b].loop_peer = (int8_t) a;
            this->save_config_();
            if ((uint32_t) b == a)
              this->ws_send_text_("{\"t\":\"ok\",\"msg\":\"loopback: port " + std::to_string(a) + " hairpinned to itself\"}");
            else
              this->ws_send_text_("{\"t\":\"ok\",\"msg\":\"trunk wired: port " + std::to_string(a) + " \u21c4 port " + std::to_string((uint32_t) b) + "\"}");
          }
        }
      }
    } else if (strncmp(p, "ORATE", 5) == 0) {
      p += 5; uint32_t id = 0, r = 0;
      if (skip_to_token(p) && read_dec(p, id) && skip_to_token(p) && read_dec(p, r) && id < WSER_MAX_PORTS && this->ports_[id].used) {
        this->ports_[id].out_cap = r;
        this->ports_[id].out_tok_ms = 0;   // bucket re-arms at next drain
      }
    } else if (strncmp(p, "RESET", 5) == 0) {
      for (int i = 0; i < WSER_MAX_PORTS; i++) { this->ports_[i].tx = this->ports_[i].rx = this->ports_[i].drop = 0; this->ports_[i].lp = false; this->ports_[i].xlp = false; this->ports_[i].txerr = 0; } this->ws_drop_ = 0;
    }
    this->save_config_();
    this->info_pending_ = true;
    return;
  }
  if (strncmp(p, "REBOOT", 6) == 0) {
    this->ws_send_text_("{\"t\":\"ok\",\"msg\":\"rebooting -- ports and trunks are saved; reconnecting in ~10 s\"}");
    // deferred so the ack leaves the WS queue and TCP flushes first
    this->set_timeout("wser_reboot", 200, []() { App.safe_reboot(); });
    return;
  }
if (strncmp(p, "WALK", 4) == 0) {
    // ABLATION LEVER: disable the interrupt-locked umm heap walk entirely.
    // If deaths stop with WALK OFF, causality is proven by removal.
    this->walk_on_ = (strstr(p, "OFF") == nullptr);
    if (!this->walk_on_)
      this->largest_cache_ = 0;   // brake reads 0 = "unknown, stand down"
    this->ws_send_text_(std::string("{\"t\":\"ok\",\"msg\":\"heap walk ") + (this->walk_on_ ? "ON" : "OFF -- brake blinded, ablation test armed") + "\"}");
    return;
  }
    if (strncmp(p, "RADIOFLOOR", 10) == 0) {
    const char *q2 = p + 10; uint32_t v = 0;
    if (skip_to_token(q2) && read_dec(q2, v)) {
      this->radio_floor_ = v;
      if (v == 0)
        this->ws_send_text_("{\"t\":\"err\",\"msg\":\"radio brake OFF -- documented FIQ wedge risk under UDP+WiFi flood; re-arms at reboot\"}");
      else
        this->ws_send_text_("{\"t\":\"ok\",\"msg\":\"radio brake floor set to " + std::to_string(v) + " B\"}");
      this->info_pending_ = true;
    }
    return;
  }
  if (strncmp(p, "LOOPDETECT", 10) == 0) {  // LOOPDETECT ON|OFF|KILL
    p += 10;
    if (skip_to_token(p)) {
      char c0 = (*p >= 'a' && *p <= 'z') ? (char) (*p - 32) : *p;
      char c1 = p[1] ? ((p[1] >= 'a' && p[1] <= 'z') ? (char) (p[1] - 32) : p[1]) : 0;
      if (c0 == 'K')
        this->loopdet_ = 2;
      else if (c0 == 'O' && c1 == 'N')
        this->loopdet_ = 1;
      else if (c0 == 'O' && c1 == 'F')
        this->loopdet_ = 0;
    }
    if (this->loopdet_ == 0) {
      for (int i = 0; i < WSER_MAX_PORTS; i++) {
        Port &pp = this->ports_[i];
        pp.lp = false; pp.lp_streak = 0;
        if (this->xvlandet_ == 0)  // rings are shared: only both-off clears them
          for (int k = 0; k < 4; k++) pp.lp_ring[k].ms = 0;
      }
    }
    this->save_config_();
    this->info_pending_ = true;
    return;
  }
  if (strncmp(p, "XVLANDETECT", 11) == 0) {  // XVLANDETECT ON|OFF|KILL
    p += 11;
    if (skip_to_token(p)) {
      char c0 = (*p >= 'a' && *p <= 'z') ? (char) (*p - 32) : *p;
      char c1 = p[1] ? ((p[1] >= 'a' && p[1] <= 'z') ? (char) (p[1] - 32) : p[1]) : 0;
      if (c0 == 'K')
        this->xvlandet_ = 2;
      else if (c0 == 'O' && c1 == 'N')
        this->xvlandet_ = 1;
      else if (c0 == 'O' && c1 == 'F')
        this->xvlandet_ = 0;
    }
    if (this->xvlandet_ == 0) {
      for (int i = 0; i < WSER_MAX_PORTS; i++) {
        Port &pp = this->ports_[i];
        pp.xlp = false; pp.xv_streak = 0;
        if (this->loopdet_ == 0)
          for (int k = 0; k < 4; k++) pp.lp_ring[k].ms = 0;
      }
    }
    this->save_config_();
    this->info_pending_ = true;
    return;
  }
  if (strncmp(p, "FLOOR", 5) == 0) {
    p += 5; uint32_t v = 0;
    if (skip_to_token(p) && read_dec(p, v) && v >= 1024) this->heap_floor_ = v;
    this->info_pending_ = true;
    return;
  }
  if (strncmp(p, "CVLAN", 5) == 0) {  // console talks on VLAN
    p += 5; uint32_t v = 0;
    if (skip_to_token(p) && read_dec(p, v) && v >= 1 && v <= WSER_VLANS) {
      this->console_vlan_ = (uint8_t) v;
      if (this->console_pi_ >= 0)
        this->ports_[this->console_pi_].vlan = (uint8_t) v;  // the authoritative field
    }
    this->save_config_();
    this->info_pending_ = true;
    return;
  }
  if (strncmp(p, "TAP", 3) == 0) {
    p += 3;
    char mode = 'F';
    if (skip_to_token(p)) {
      mode = (*p >= 'a' && *p <= 'z') ? (char) (*p - 32) : *p;
      while (*p && *p != ' ')
        p++;
    }
    this->flush_agg_();
    if (mode == 'B') { this->tap_batch_ = true; this->tap_off_ = false; }
    else if (mode == 'L') { this->tap_batch_ = false; this->tap_off_ = false; }
    else if (mode == 'O') { this->tap_off_ = true; }
    else {
      this->tap_off_ = false;
      this->tap_full_ = (mode != 'S');
      uint32_t d;
      this->tap_filter_dir_ = (skip_to_token(p) && read_dec(p, d)) ? (int) d : -1;
    }
    this->info_pending_ = true;
    return;
  }
  if (strncmp(p, "ARM", 3) == 0) {
    p += 3;
    this->arm_dir_ = -1;
    this->arm_byte_ = -1;
    if (skip_to_token(p)) {
      if (*p == '*') p++;
      else { uint32_t v = 0; if (read_dec(p, v)) this->arm_dir_ = (int) v; }
      if (skip_to_token(p)) {
        uint32_t c = 0;
        if (wser_read_hexv(p, c))
          this->arm_byte_ = (int) c;
      }
    }
    this->info_pending_ = true;
    return;
  }
  this->ws_send_text_("{\"t\":\"err\",\"msg\":\"unknown command\"}");
}

void WebSerial::port_wire_drain_(int idx) {
  Port &a = this->ports_[idx];
  if (a.loop_peer < 0 || a.buf == nullptr || a.buf_n == 0)
    return;
  int pb = a.loop_peer;
  if (pb < 0 || pb >= WSER_MAX_PORTS || !this->ports_[pb].used || this->ports_[pb].type != PT_BRIDGE ||
      this->ports_[pb].loop_peer != idx) {
    ESP_LOGW(TAG, "port %d: virtual wire peer invalid, unplugging", idx);
    a.loop_peer = -1;   // paranoid: never a dangling wire
    return;
  }
  if (!a.up)
    return;
  size_t room = this->egress_room_(a);   // the 'out' pill IS the wire's baud
  size_t k = a.buf_n;
  if (k > room)
    k = room;
  if (k > this->drain_quota_)
    k = this->drain_quota_;   // AIMD ceiling
  if (k == 0)
    return;
  uint32_t chrono_d0 = micros();
  this->egress_spend_(a, k);
  a.tx += k;   // wire egress counted like any egress
  this->wire_ctx_ = true;          // declared crossing: XVLAN-exempt
  this->wire_src_ = (int8_t) idx;  // origin rule traverses the wire
  this->switch_ingress_(pb, a.buf, k);
  this->wire_ctx_ = false;
  this->wire_src_ = -1;
  memmove(a.buf, a.buf + k, a.buf_n - k);
  a.buf_n -= (uint16_t) k;
  {
    uint32_t ddur = micros() - chrono_d0;
    if (ddur > this->max_drain_us_) this->max_drain_us_ = ddur;
  }
}

void WebSerial::bstat_tick_() {
  // 4 Hz buffer-fill telemetry. Crash-4 doctrine: NO std::string churn --
  // snprintf into a stack buffer, gated by client presence, backlog soft
  // cap, and (8266) the heap floor. Silent when nobody is watching.
  if (this->stream_client_ == nullptr || this->serving_page_)
    return;
  uint32_t now = millis();
  if (this->bstat_ms_ != 0 && now - this->bstat_ms_ < 250)
    return;
  this->bstat_ms_ = now;
  if (this->out_.size() > WSER_OUT_SOFT)
    return;
#ifdef USE_ESP8266
  if (this->largest_cache_ != 0 && this->largest_cache_ < 3072)
    return;
#endif
  char b[224];
  int o = snprintf(b, sizeof(b), "{\"t\":\"bs\",\"o\":[");
  for (int i = 0; i < WSER_MAX_PORTS; i++) {
    Port &p = this->ports_[i];
    int pct = (p.used && p.buf != nullptr && p.buf_cap) ? (int) ((uint32_t) p.buf_n * 100 / p.buf_cap) : -1;
    o += snprintf(b + o, sizeof(b) - o, "%s%d", i ? "," : "", pct);
  }
  o += snprintf(b + o, sizeof(b) - o, "],\"i\":[");
  for (int i = 0; i < WSER_MAX_PORTS; i++) {
    Port &p = this->ports_[i];
    int v = -1;                       // n/a (console, bridge, idle slots)
    if (p.used && p.type == PT_UART && this->uart_ != nullptr) {
      size_t cap = this->uart_->get_rx_buffer_size();
      if (cap)
        v = (int) ((uint32_t) this->uart_->available() * 100 / cap);
      if (v > 100)
        v = 100;
    } else if (p.used && p.type == PT_TCP) {
      v = -2;                         // flow-controlled by design (no local buffer)
    }
#ifdef USE_SOCKET_IMPL_LWIP_TCP
    else if (p.used && p.type == PT_UDP && p.wudp) {
      v = (int) ((uint32_t) p.wudp->pending() * 100 / 3);
    }
#endif
    o += snprintf(b + o, sizeof(b) - o, "%s%d", i ? "," : "", v);
  }
  o += snprintf(b + o, sizeof(b) - o, "]}");
  if (o > 0 && o < (int) sizeof(b))
    this->ws_send_(ws::OP_TEXT, reinterpret_cast<const uint8_t *>(b), (size_t) o);
}

// ALLOCQUIET number appender: formats into a stack scratch, appends chars --
// zero heap activity when the destination has capacity (reserved at setup).
static void app_n(std::string &s, long long v) {
  char b[14]; int i = 14; bool neg = v < 0;
  unsigned long long u = neg ? (unsigned long long) (-v) : (unsigned long long) v;
  if (u == 0) { s += '0'; return; }
  while (u) { b[--i] = (char) ('0' + (u % 10)); u /= 10; }
  if (neg) s += '-';
  s.append(b + i, 14 - i);
}

void WebSerial::ws_send_text_(const char *text) {
  this->ws_send_(ws::OP_TEXT, reinterpret_cast<const uint8_t *>(text), strlen(text));
}

// TRUTHCONS ring helpers -- byte ring with wrap, variable records.
static inline uint16_t cr_free_(uint16_t w, uint16_t r) { return (uint16_t) ((r - w - 1) & 1023); }
void WebSerial::cons_record_(uint8_t src, const uint8_t *d, size_t n) {
  if (this->stream_client_ == nullptr || this->serving_page_)
    return;   // no observer, no contract: nothing recorded, nothing 'lost'
  // a pending gap is confessed FIRST, as its own record, before any data
  if (this->cons_gap_ > 0 && cr_free_(this->cr_w_, this->cr_r_) >= 6) {
    uint32_t g = this->cons_gap_; this->cons_gap_ = 0;
    uint8_t hdr[6] = {0xFF, 4, (uint8_t) g, (uint8_t) (g >> 8), (uint8_t) (g >> 16), (uint8_t) (g >> 24)};
    for (int k = 0; k < 6; k++) { this->cons_ring_[this->cr_w_] = hdr[k]; this->cr_w_ = (uint16_t) ((this->cr_w_ + 1) & 1023); }
  }
  while (n > 0) {
    size_t c = n > 64 ? 64 : n;
    if (cr_free_(this->cr_w_, this->cr_r_) < c + 2) {
      this->cons_gap_ += (uint32_t) n;
      if (this->console_pi_ >= 0) this->ports_[this->console_pi_].drop += (uint32_t) n;
      return;
    }
    this->cons_ring_[this->cr_w_] = src; this->cr_w_ = (uint16_t) ((this->cr_w_ + 1) & 1023);
    this->cons_ring_[this->cr_w_] = (uint8_t) c; this->cr_w_ = (uint16_t) ((this->cr_w_ + 1) & 1023);
    for (size_t k = 0; k < c; k++) { this->cons_ring_[this->cr_w_] = d[k]; this->cr_w_ = (uint16_t) ((this->cr_w_ + 1) & 1023); }
    d += c; n -= c;
  }
}
void WebSerial::cons_drain_() {
  if (this->stream_client_ == nullptr || this->serving_page_)
    { this->cr_r_ = this->cr_w_; this->cons_gap_ = 0; return; }
  if (this->cons_gap_ > 0) {
    // The confession record: [0x03][0xFF][LE32 lost] -- the frontend turns
    // it into an inline [lost N B] marker and feeds the truth banner.
    uint32_t g = this->cons_gap_;
    uint8_t gb[6] = {0x03, 0xFF, (uint8_t) g, (uint8_t) (g >> 8), (uint8_t) (g >> 16), (uint8_t) (g >> 24)};
    if (!this->ws_send_(ws::OP_BIN, gb, 6))
      return;   // valve refused: the debt stands, confess next pass
    this->cons_gap_ = 0;   // field bug: judging by out_.size() misread a
                           // synchronous flush as refusal -- the same gap
                           // re-confessed every pass (the [lost N] wall)
  }
  int budget = 4;   // bounded per pass: <= 4 records (~264 B worst)
  while (budget-- > 0 && this->cr_r_ != this->cr_w_) {
    uint8_t src = this->cons_ring_[this->cr_r_];
    uint8_t len = this->cons_ring_[(uint16_t) ((this->cr_r_ + 1) & 1023)];
    uint8_t buf[66];
    buf[0] = 0x03; buf[1] = src;
    uint16_t rr = (uint16_t) ((this->cr_r_ + 2) & 1023);
    for (int k = 0; k < len; k++) { buf[2 + k] = this->cons_ring_[rr]; rr = (uint16_t) ((rr + 1) & 1023); }
    if (!this->ws_send_(ws::OP_BIN, buf, (size_t) len + 2))
      return;   // valve refused (soft/coh/heap): record stays, retry next pass
    this->cr_r_ = rr;
  }
}

void WebSerial::bb_push_() {
  if (!this->bb_pending_ || this->stream_client_ == nullptr || this->serving_page_)
    return;
  this->bb_pending_ = false;   // one report per boot, to the first client
  // Mirror on the LOGGER too: the setup-time ESP_LOGE is invisible to the
  // OTA viewer (it prints before wifi/API exist). HERE the API is up, so
  // the report reaches BOTH channels. esphome's own formatter (proven for
  // years) -- NOT a raw snprintf (Piege 43).
  ESP_LOGE(TAG, "BLACK BOX replay: phase 0x%02X after %u passes -- mxpass %u mxwin %u mxwalk %u us -- last evt %u at pass %u",
           (unsigned) this->bb_phase_, (unsigned) this->bb_passes_, (unsigned) this->bb_mxp_,
           (unsigned) this->bb_mxw_, (unsigned) this->bb_mxh_, (unsigned) this->bb_evt_, (unsigned) this->bb_evtp_);
  // FIELD CRASH #6 (decoded OTA backtrace: Alignment exccause=9 inside
  // _svfprintf_r/__ssputs_r): this used to be the ONLY runtime snprintf in
  // the file -- newlib-nano word-walking a long flash-resident format is
  // the classic 8266 trap the house std::string style exists to avoid. And
  // since every crash re-arms bb_pending_, the messenger became a SELF-
  // PERPETUATING crash loop: boot, first client, format the report, die.
  // Back to house style: cold path, plain concatenation, no formatter.
  std::string &b = this->info_buf_;   // ALLOCQUIET: reuse the reserved buffer
  b.clear();
  b += "{\"t\":\"sys\",\"msg\":\"BLACK BOX (previous boot crashed): phase 0x";
  const char *hx = "0123456789ABCDEF";
  b += hx[(this->bb_phase_ >> 4) & 15];
  b += hx[this->bb_phase_ & 15];
  b += " after "; app_n(b, this->bb_passes_); b += " passes -- mxpass "; app_n(b, this->bb_mxp_);
  b += " us, mxwin "; app_n(b, this->bb_mxw_); b += " us, mxwalk "; app_n(b, this->bb_mxh_);
  b += " us -- last evt "; app_n(b, this->bb_evt_); b += " at pass "; app_n(b, this->bb_evtp_); b += "\"}";
  this->coh_exempt_ = true;
  this->ws_send_text_(b);
  this->coh_exempt_ = false;
}
void WebSerial::send_info_() {
  // Black-box delivery point: NOT at client attach (the page-serving GET
  // also attaches, and a frame queued between the HTTP headers and the HTML
  // corrupts AND truncates the page -- field-found). Here the WS is
  // provably up; bb_push_ self-guards and fires once.
  this->bb_push_();
  WSER_EVT(2);
  // STACKFIX for info (field: the connect/die loop at a full 8266 table):
  // the WS gates test out_ BEFORE queueing, so an ~3 KB info landing on an
  // out_ already garnished just under SOFT (sys lines, cons) parks it above
  // HARD and the NEXT send drops the client -- forever, since ports persist.
  // Rule: an info only departs on a near-empty channel; otherwise it is
  // DEFERRED via info_pending_ (used for its designed purpose: an info is
  // owed) and retried next pass. No valve is touched.
  // Starved-from-birth fix (field: skeleton page, beacon never fired): a
  // boot that has NEVER pushed an info is starved too -- after 5 s uptime.
  bool starved = (this->info_ok_ms_ == 0) ? (millis() > 5000)
                                          : ((uint32_t) (millis() - this->info_ok_ms_) > 3000);
  size_t backlog = this->out_.size() - this->out_pos_;
  if (backlog > 512) {
    // QOSLANE: after 3 s of famine the CONTROL plane may use the SOFT..HARD
    // gap -- send iff backlog + worst frame still clears HARD with margin.
    // The valve stays sovereign; the lane lives INSIDE it.
    if (!(starved && backlog + 3300 < (size_t) WSER_OUT_HARD - 128)) {
      this->info_pending_ = true;
      if (starved && (uint32_t) (millis() - this->info_starve_ms_) > 5000) {
        this->info_starve_ms_ = millis();
        ESP_LOGW(TAG, "info starved by BACKLOG: out %u/%u pending, free %u largest %u",
                 (unsigned) backlog, (unsigned) this->out_.size(),
                 (unsigned) this->free_heap_(), (unsigned) this->largest_block_());
      }
      return;
    }
  }
#ifdef USE_ESP8266
  // Field crash #4 (OOM 1921 B in cont ctx) and field crash #5 (flood +
  // bridge, reboot at evt 2): this function builds the fixed head through
  // dozens of operator+ temporaries, then j.reserve() asks ONE contiguous
  // block of j.size() + 232*WSER_MAX_PORTS + 64 (~4.7 KB). The old gate
  // (3072) sat BELOW that ask -- a storm-laminated heap with largest in
  // (3072..~4700) passed the gate and died inside reserve (no exceptions on
  // the 8266: bad alloc = abort = hardware reset). The gate now covers the
  // reserve bill plus build churn. Skip the push, never crash -- the UI
  // resyncs on the next one and the age stamp discloses the silence.
  uint32_t lb = this->largest_block_();
  if (lb < (232 * WSER_MAX_PORTS + 64 + 2048)) {
    this->info_pending_ = true;   // NEVER a silent starve (field: skeleton page)
    if (starved && (uint32_t) (millis() - this->info_starve_ms_) > 5000) {
      this->info_starve_ms_ = millis();
      // The distress beacon: tiny, house-style, always fits -- the panel
      // freezes BY DESIGN under heap distress, and says so with the number.
      ESP_LOGW(TAG, "info starved by HEAP: largest %u < 5824, free %u, out backlog %u",
               (unsigned) lb, (unsigned) this->free_heap_(), (unsigned) backlog);
      std::string &d = this->info_buf_;   // ALLOCQUIET: same reserved buffer
      d.clear();
      d += "{\"t\":\"sys\",\"msg\":\"info starved: largest ";
      app_n(d, lb);
      d += " B under the 5824 gate -- panel frozen by design, heap is the story\"}";
      this->coh_exempt_ = true;
      this->ws_send_text_(d);
      this->coh_exempt_ = false;
    }
    return;
  }
#endif
  WSER_CRUMB(0x20);   // send_info_: gates passed, building
  uint32_t mhz;
#ifdef USE_ESP8266
  mhz = ESP.getCpuFreqMHz();          // runtime truth: 80 or 160 (system_update_cpu_freq)
#elif defined(USE_ESP32)
  mhz = (uint32_t) (esp_clk_cpu_freq() / 1000000);  // runtime truth, arduino AND idf
#else
  mhz = 0;                            // unknown platform: disclosed as '-'
#endif
  std::string &j = this->info_buf_;   // ALLOCQUIET: one reservation, forever
  j.clear();
  j += "{\"t\":\"info\",\"obs\":" ; app_n(j, this->observed_); j += ",\"dropped\":" ; app_n(j, this->dropped_); j += ",\"dth\":" ; app_n(j, this->drop_thr_); j += ",\"dhp\":" ; app_n(j, this->drop_heap_); j += ",\"dbk\":" ; app_n(j, this->drop_bkl_); j += ",\"mhz\":" ; app_n(j, mhz); j += ",\"txr\":" ; app_n(j, this->tx_rate_); j += ",\"rxr\":" ; app_n(j, this->rx_rate_); j += ",\"heap\":" ; app_n(j, this->free_heap_()); j += ",\"largest\":" ; app_n(j, this->largest_block_()); j += ",\"frag\":" ; app_n(j, (int) this->frag_pct_()); j += ",\"bufs\":" ; app_n(j, this->buffers_alloc_); j += ",\"floor\":" ; app_n(j, this->heap_floor_); j += ",\"bufmax\":" ; app_n(j, WSER_PORT_BUF_MAX); j += ",\"loop\":" ; app_n(j, this->loop_ema_us_); j += ",\"up\":" ; app_n(j, millis() / 1000); j += ",\"baud\":" ; app_n(j, this->uart_ ? this->uart_->get_baud_rate() : 0); j += ",\"dbits\":" ; app_n(j, this->uart_ ? (int) this->uart_->get_data_bits() : 8); j += ",\"sbits\":" ; app_n(j, this->uart_ ? (int) this->uart_->get_stop_bits() : 1); j += ",\"par\":\""; j += (this->uart_ == nullptr ? "N" : this->uart_->get_parity() == uart::UART_CONFIG_PARITY_EVEN ? "E" : this->uart_->get_parity() == uart::UART_CONFIG_PARITY_ODD ? "O" : "N"); j += "\"" ",\"gap\":" ; app_n(j, this->gap_ms_); j += ",\"delim\":" ; app_n(j, (int) this->delim_); j += ",\"cvlan\":" ; app_n(j, (int) this->console_vlan_); j += ",\"owner\":"; j += (this->owner_ ? "true" : "false"); j += ",\"tapoff\":" ; j += (this->tap_off_ ? "true" : "false"); j += ",\"tapfull\":" ; j += (this->tap_full_ ? "true" : "false"); j += ",\"tapbatch\":" ; j += (this->tap_batch_ ? "true" : "false"); j += ",\"armed\":" ; j += (this->arm_dir_ != -2 ? "true" : "false"); j += ",\"flood\":" ; j += (this->flood_ ? "true" : "false"); j += ",\"loopdet\":" ; app_n(j, (int) this->loopdet_); j += ",\"xvlandet\":" ; app_n(j, (int) this->xvlandet_); j += ",\"tapvis\":"; j += (this->tap_visible_() ? "true" : "false"); j += ",\"wsdrop\":" ; app_n(j, this->ws_drop_); j += ",\"uhw\":" ; j += this->uart_hw_ ? "true" : "false"; j += ",\"uname\":\"" ; j += this->uart_name_; j += "\",\"urx\":" ; app_n(j, this->uart_ ? this->uart_->get_rx_buffer_size() : 0); j += ",\"swr\":" ; app_n(j, this->switched_rate_); j += ",\"dq\":" ; app_n(j, this->drain_quota_); j += ",\"dqd\":" ; app_n(j, (int) this->quota_dir_); j += ",\"thr\":" ; app_n(j, this->throttle_events_); j += ",\"thc\":" ; app_n(j, this->throttle_cause_); j += ",\"mxp\":" ; app_n(j, this->max_pass_us_); j += ",\"mxw\":" ; app_n(j, this->max_win_us_); j += ",\"mxd\":" ; app_n(j, this->max_drain_us_); j += ",\"mxh\":" ; app_n(j, this->max_walk_us_); j += ",\"rfloor\":" ; app_n(j, this->radio_floor_); j += ",\"peak\":" ; app_n(j, this->peak_rate_); j += ",\"eload\":" ; app_n(j, this->eload_pct_); j += ",\"udpok\":true";
#ifdef USE_ESP8266
  if (this->largest_block_() < j.size() + 232 * (size_t) WSER_MAX_PORTS + 64 + 256) {
    this->info_pending_ = true;
    return;  // the heap moved during the head build: same rule, skip not crash
  }
#endif
  j.reserve(j.size() + 232 * (size_t) WSER_MAX_PORTS + 64);  // one alloc for the append phase
  // PGINFO: ports are paged (ids 0-7 / 8-15 on alternating pushes) whenever
  // any id >= 8 exists -- i.e. on the ESP32-class table (WSER_MAX_PORTS 16;
  // the 8266 table is 8 and never pages). Each page is AUTHORITATIVE for
  // its range (absent id = deleted). This halves the largest frame and
  // future-proofs bigger tables; the 8266 connect/die loop itself was INFO
  // STACKING, fixed by the near-empty-channel rule above. Mirrored in the
  // page JS (updInfo merge) and in tests/host/test_ui.js.
  bool paged = false;
  for (int sp = 8; sp < WSER_MAX_PORTS; sp++)
    if (this->ports_[sp].used) { paged = true; break; }
  if (paged) {
    j += ",\"pg\":"; app_n(j, (int) this->info_pg_); j += ",\"pgs\":2";
  }
  j += ",\"ports\":[";
  bool first = true;
  for (int i = 0; i < WSER_MAX_PORTS; i++) {
    Port &p = this->ports_[i];
    if (!p.used) continue;
    if (paged && (i >> 3) != (int) this->info_pg_) continue;
    // isolated = UP with no other UP port in its vlan: everything it says,
    // and everything said to it, dies at the switch. Legal topology (that is
    // what isolation IS) -- but the operator must SEE it, on every port, not
    // discover it by staring at silent counters.
    bool iso = p.up;
    if (iso)
      for (int q = 0; q < WSER_MAX_PORTS; q++)
        if (q != i && this->ports_[q].used && this->ports_[q].up && this->ports_[q].vlan == p.vlan) { iso = false; break; }
    if (!first) j += ",";
    first = false;
    const char *tn = p.type == PT_UART ? "uart" : p.type == PT_CONSOLE ? "console" : p.type == PT_TCP ? "tcp" : p.type == PT_UDP ? "udp" : "bridge";
    bool conn = (p.type == PT_TCP && p.client != nullptr) || (p.type == PT_UDP && p.have_peer);
    j += "{\"id\":" ; app_n(j, i); j += ",\"type\":\"" ; j += tn; j += "\",\"vlan\":" ; app_n(j, (int) p.vlan); j += ",\"up\":" ; j += (p.up ? "true" : "false"); j += ",\"np\":" ; app_n(j, p.net_port); j += ",\"rate\":" ; app_n(j, p.rate_cap); j += ",\"orate\":" ; app_n(j, p.out_cap); j += ",\"wire\":" ; app_n(j, (int) p.loop_peer); j += ",\"hbrake\":" ; app_n(j, p.hbrake); j += ",\"lr\":" ; app_n(j, p.rate_bps); j += ",\"tx\":" ; app_n(j, p.tx); j += ",\"rx\":" ; app_n(j, p.rx); j += ",\"drop\":" ; app_n(j, p.drop); j += ",\"conn\":" ; j += (conn ? "true" : "false"); j += ",\"lp\":" ; j += (p.lp ? "true" : "false"); j += ",\"xlp\":" ; j += (p.xlp ? "true" : "false"); j += ",\"iso\":" ; j += (iso ? "true" : "false"); j += ",\"bufcap\":" ; app_n(j, p.buf_cap); j += ",\"txerr\":" ; app_n(j, p.txerr); j += ",\"fixed\":" ; j += ((i == this->uart_pi_ || i == this->console_pi_) ? "true" : "false"); j += "}";
  }
  j += "]}";
  this->flood_ = false;
  if (j.size() + 8 > WSER_OUT_HARD) {
    this->info_pending_ = true;
    return;   // absolute belt: never hand the valve a frame it must die on
  }
  WSER_CRUMB(0x21);   // send_info_: built, entering ws_send/flush
  this->coh_exempt_ = true;
  this->ws_send_text_(j);
  this->coh_exempt_ = false;
  WSER_CRUMB(0x22);   // send_info_: push returned
  this->info_ok_ms_ = millis();
  if (paged) this->info_pg_ ^= 1;
}

uint32_t WebSerial::free_heap_() {
#ifdef USE_ESP8266
  return ESP.getFreeHeap();
#elif defined(USE_ESP32)
  return esp_get_free_heap_size();
#else
  return 0;
#endif
}


void WebSerial::save_config_() {
  SavedCfg c{};
  c.magic = 0x5B;  // v5: + virtual trunk wire (loop_peer)
  c.console_vlan = this->console_vlan_;
  c.loopdet = this->loopdet_;
  c.xvlandet = this->xvlandet_;
  c.nports = 0;
  for (int i = 0; i < WSER_MAX_PORTS; i++) {
    Port &p = this->ports_[i];
    if (!p.used || p.type == PT_UART || p.type == PT_CONSOLE || p.type == PT_BRIDGE)
      continue;  // only dynamic net ports are restorable
    if (p.type == PT_TCP && this->init_tcp_port_ != 0 && p.net_port == this->init_tcp_port_)
      continue;  // the YAML tcp_port: is re-created by setup() every boot --
                 // saving it too made it come back TWICE (once from YAML,
                 // once from prefs) the boot after the first config save
    SavedPort &s = c.ports[c.nports++];
    s.type = p.type; s.vlan = p.vlan; s.up = p.up ? 1 : 0;
    s.net_port = p.net_port; s.buf_cap = p.buf_cap; s.rate = p.rate_cap; s.orate = p.out_cap; s.wire = p.loop_peer;
  }
  this->pref_.save(&c);
}
void WebSerial::load_config_() {
  SavedCfg c{};
  if (!this->pref_.load(&c) || c.magic != 0x5B)
    return;  // (older prefs just fall back to defaults, once)
  this->console_vlan_ = c.console_vlan == 0 ? 1 : c.console_vlan;
  if (this->console_pi_ >= 0)
    this->ports_[this->console_pi_].vlan = this->console_vlan_;
  this->loopdet_ = c.loopdet > 2 ? 1 : c.loopdet;
  this->xvlandet_ = c.xvlandet > 2 ? 1 : c.xvlandet;
  for (int i = 0; i < c.nports && i < WSER_MAX_PORTS; i++) {
    SavedPort &s = c.ports[i];
    // dedupe on restore: a saved port identical (type + net port) to one that
    // already exists -- the YAML port, or stale entries from older prefs --
    // must NOT be created a second time. This also self-heals prefs written
    // before the save-side fix above.
    bool dup = false;
    for (int q = 0; q < WSER_MAX_PORTS && !dup; q++) {
      Port &ex = this->ports_[q];
      if (ex.used && (uint8_t) ex.type == s.type && ex.net_port == s.net_port)
        dup = true;
    }
    if (dup)
      continue;
    int pi = this->port_alloc_((PortType) s.type, s.net_port, s.buf_cap, s.type == PT_UDP);
    if (pi >= 0) {
      this->ports_[pi].vlan = s.vlan == 0 ? 1 : s.vlan;
      this->ports_[pi].up = s.up != 0;
      this->ports_[pi].rate_cap = s.rate;
      this->ports_[pi].out_cap = s.orate;
      this->ports_[pi].loop_peer = s.wire;   // validated in a second pass below
    }
  }
  // trunk validation pass: both ends must exist, be bridges, and point at
  // each other -- anything else is unplugged (one log), never a corrupted
  // state resurrected.
  for (int a = 0; a < WSER_MAX_PORTS; a++) {
    Port &pa = this->ports_[a];
    if (!pa.used || pa.loop_peer < 0)
      continue;
    int b = pa.loop_peer;
    bool ok = b < WSER_MAX_PORTS && this->ports_[b].used &&   // b == a is the hairpin
              pa.type == PT_BRIDGE && this->ports_[b].type == PT_BRIDGE &&
              this->ports_[b].loop_peer == a;
    if (!ok) {
      ESP_LOGW(TAG, "saved trunk on port %d invalid, unplugged", a);
      pa.loop_peer = -1;
    }
  }
}

void WebSerial::setup() {
  // ALLOCQUIET: capacity for any legal frame under the belts -- out_ never
  // grows (vector realloc = a CONT allocator visit) after setup.
  this->out_.reserve(WSER_OUT_HARD + 512);
  this->info_buf_.reserve(3456);   // fixed head + one 8-port page, worst case
  this->ws_accum_.reserve(256);
  if (this->de_pin_ != nullptr) {
    this->de_pin_->setup();
    this->de_pin_->digital_write(false);  // receive by default (RS485)
  }
  // the two fixed ports: the physical UART and the web console
  // the uart port gets an egress buffer like every network port: the switch
  // ENQUEUES, a baud-derived quota drains per pass. Field lesson: direct
  // synchronous fanout to a 19200 software serial burned ~390 ms of CPU per
  // second under storm (0.52 ms blocking bit-bang PER BYTE) -- the loop
  // period was the WIRE, not the work.
  this->uart_pi_ = this->port_alloc_(PT_UART, 0, 256, false);
  this->console_pi_ = this->port_alloc_(PT_CONSOLE, 0, 0, false);
  if (this->uart_ != nullptr) {
    this->uart_->add_debug_callback([this](uart::UARTDirection dir, uint8_t b) { this->on_uart_byte(dir, b); });
  }
  // YAML tcp_port: -> a pre-provisioned TCP port at boot
  if (this->init_tcp_port_ != 0)
    this->port_alloc_(PT_TCP, this->init_tcp_port_, WSER_PORT_BUF_DEF, false);
  // restore the saved switch config (dynamic ports + VLANs)
  this->pref_ = global_preferences->make_preference<SavedCfg>(fnv1_hash("web_serial_cfg_" + std::to_string(this->port_)));
  this->load_config_();
  ESP_LOGCONFIG(TAG, "web_serial switch on %u, %d ports", this->port_, WSER_MAX_PORTS);
#ifdef USE_ESP8266
  if (s_crumb.magic == 0x57435232UL) {
    ESP_LOGE(TAG, "BLACK BOX: phase 0x%02X after %u passes | mxpass=%uus mxwin=%uus mxwalk=%uus | last_evt=%u at pass %u",
             (unsigned) s_crumb.phase, (unsigned) s_crumb.passes, (unsigned) s_crumb.mxp,
             (unsigned) s_crumb.mxw, (unsigned) s_crumb.mxh, (unsigned) s_crumb.last_evt,
             (unsigned) s_crumb.last_evt_pass);
    // Copy the report for the web: the first client to connect gets it as a
    // sys line (the serial log is not always watched; the page is).
    this->bb_pending_ = true;
    this->bb_valid_ = true;
    this->bb_phase_ = s_crumb.phase; this->bb_passes_ = s_crumb.passes;
    this->bb_mxp_ = s_crumb.mxp; this->bb_mxw_ = s_crumb.mxw; this->bb_mxh_ = s_crumb.mxh;
    this->bb_evt_ = s_crumb.last_evt; this->bb_evtp_ = s_crumb.last_evt_pass;
    s_crumb.mxp = s_crumb.mxw = s_crumb.mxh = 0;
    s_crumb.last_evt = 0; s_crumb.last_evt_pass = 0;
    s_crumb.magic = 0;  // one report per crash
  } else {
    // fresh flash / clean power-up: .noinit is random -- zero everything the
    // report reads, or the FIRST real crash prints garbage microseconds
    // (field-found: mxpass '18 minutes').
    s_crumb.phase = 0;
    s_crumb.mxp = s_crumb.mxw = s_crumb.mxh = 0;
    s_crumb.last_evt = 0; s_crumb.last_evt_pass = 0;
  }
  s_crumb.passes = 0;
  if (!this->uart_hw_)
    ESP_LOGW(TAG, "UART is SOFTWARE bit-bang on this build -- see the config dump for the hardware recipe");
#endif
}
void WebSerial::dump_config() {
  ESP_LOGCONFIG(TAG, "web_serial hub: web %u, tcp232 %u, %s mode, gap %u ms", this->port_, this->init_tcp_port_,
                this->owner_ ? "owner" : "tap", this->gap_ms_);
  ESP_LOGCONFIG(TAG, "  loop detect: %s", this->loopdet_ == 2 ? "KILL" : this->loopdet_ == 1 ? "ON" : "OFF");
  ESP_LOGCONFIG(TAG, "  xvlan detect: %s", this->xvlandet_ == 2 ? "KILL" : this->xvlandet_ == 1 ? "ON" : "OFF");
  ESP_LOGCONFIG(TAG, "  UART TRANSPORT: %s (tx=%d rx=%d, %u baud, rx_buffer %u B)", this->uart_name_,
                this->uart_tx_, this->uart_rx_, this->uart_ ? this->uart_->get_baud_rate() : 0,
                this->uart_ ? (unsigned) this->uart_->get_rx_buffer_size() : 0);
#ifdef USE_ESP8266
  if (!this->uart_hw_) {
    ESP_LOGW(TAG, "  CONSEQUENCE: software TX is cycle-exact bit-bang; under WiFi load it can");
    ESP_LOGW(TAG, "  wedge the chip (HWDT, black-box phase 0x60). Egress is SHIELDED to ~2 ms/pass.");
    ESP_LOGW(TAG, "  For hardware: tx GPIO2 + NO rx = UART1 | tx1/rx3 = UART0 (move logger) | tx15/rx13 = swap");
  } else {
    ESP_LOGCONFIG(TAG, "  CONSEQUENCE: silicon FIFO, non-blocking TX -- egress saturates the wire");
  }
#else
  ESP_LOGCONFIG(TAG, "  CONSEQUENCE: full hardware throughput, no artificial limits");
#endif
}

void WebSerial::loop() {
  if (!this->server_started_)
    this->start_server_();
  if (this->server_ == nullptr)
    return;
  WSER_CRUMB(0x01);
  this->pass_subs_ = 0;
  this->accept_client_();
  if (this->out_pos_ < this->out_.size())
    this->flush_tx_();
  if (this->stream_client_ != nullptr || this->pending_client_ != nullptr)
    WSER_CRUMB(0x02);
    this->read_client_();
  if (this->pending_client_ != nullptr && millis() - this->pending_since_ > 3000) {
    this->pending_client_->close();
    this->pending_client_ = nullptr;
    this->request_len_ = 0;
    this->nl_ = 0;
  }
  // owner mode: the hub reads the wire itself (feeds the same callback pipeline)
  if (this->owner_ && this->uart_ != nullptr) {
    size_t av = this->uart_->available();
    while (av > 0) {
      uint8_t buf[32];
      size_t k = av > sizeof(buf) ? sizeof(buf) : av;
      if (!this->uart_->read_array(buf, k))
        break;
      av -= k;
    }
  }
  // an idle line closes the open frame
  if (this->fr_.open && (uint32_t) (micros() - this->last_byte_us_) > (uint32_t) this->gap_ms_ * 1000)
    this->frame_close_();
  WSER_CRUMB(0x03);
  this->rx_stage_flush_();  // staged wire RX enters the switch before egress drains
  if (this->info_pending_ && this->stream_client_ != nullptr && !this->serving_page_) {
    this->info_pending_ = false;
    this->send_info_();   // depth ~zero: the 4 KB cont stack is all ours here
  } else if (this->info_pending_ && this->stream_client_ == nullptr) {
    this->info_pending_ = false;   // nobody listening: drop the request
  }
  this->bstat_tick_();
  this->cons_drain_();
#ifndef USE_ESP8266
  {
    bool want = (this->tx_rate_ + this->rx_rate_) > 0;
    if (!want) {
      for (int i = 0; i < WSER_MAX_PORTS && !want; i++)
        want = this->ports_[i].used && this->ports_[i].buf_n > 0;
    }
    if (want != this->hf_on_) {
      this->hf_on_ = want;
      if (want)
        this->high_freq_.start();
      else
        this->high_freq_.stop();
    }
  }
#endif
  {
    uint32_t nowm = millis();
    if (this->largest_cache_ms_ == 0 || nowm - this->largest_cache_ms_ >= 250) {
  {
    uint32_t w0 = micros();
      if (this->walk_on_) this->largest_cache_ = this->largest_block_();
    uint32_t wd = micros() - w0;
    if (wd > this->max_walk_us_) this->max_walk_us_ = wd;
    WSER_MAXC(mxh, wd);
    if (wd > 150000) { WSER_CRUMB(0xEE); }   // heap walk exceeded 150 ms!
  }
      this->largest_cache_ms_ = nowm;
    }
  }
  // ---- duty pool refill (governor v2) ----
  {
    uint32_t nowp = micros();
    if (this->pool_last_us_ != 0) {
      uint32_t wall = nowp - this->pool_last_us_;
      if (wall > 1000000)
        wall = 1000000;  // clamp aberrations (flash writes, scans)
      this->pool_us_ += wall * WSER_DUTY_PCT / 100;
      if (this->pool_us_ > WSER_POOL_CAP_US)
        this->pool_us_ = WSER_POOL_CAP_US;
    }
    this->pool_last_us_ = nowp;
  }
  if (this->pool_us_ < 2000) {
    this->duty_starved_ = true;
  }
  if (this->pool_us_ < 2000)
    return;   // pool empty: skip the whole heavy phase, the radio eats first
  uint32_t phase_t0 = micros();
  uint32_t chrono_pass0 = phase_t0;
  {
    uint32_t nowp2 = micros();
    if (this->phase_last_us_ != 0 && (uint32_t) (nowp2 - this->phase_last_us_) < 1000) {
      // heavy phase runs at most every 1 ms: at 16 ms loops this never
      // fires; at microsecond loops it is the anti-sawtooth metronome
      goto phase_skipped;
    }
    this->phase_last_us_ = nowp2;
  }
  // uart egress drain: at most ~8 ms of wire per pass, whatever the baud
  // (19200 -> 16 B, 115200 -> 92 B). The wire's own rate stays the true
  // throughput limit; only the per-pass BLOCKING is bounded now.
  if (this->uart_pi_ >= 0) {
    Port &upt = this->ports_[this->uart_pi_];
    if (upt.used && upt.buf != nullptr && upt.buf_n > 0 && this->uart_ != nullptr) {
      uint32_t baud = this->uart_->get_baud_rate();
      // BLACK BOX verdict (phase 0x60): the wedge lives inside software-
      // serial TX under FIQ pressure -- cycle-exact bit-bang preempted
      // mid-bit. Shrink the exposure window: ~2 ms of wire per pass
      // (4 bytes at 19200). Hardware UART1 (drop rx_pin, TX-only GPIO2)
      // removes the class entirely -- this is defense while you migrate.
      size_t quota;
      if (this->uart_hw_) {
        // hardware FIFO: feed what the wire drains per pass (baud/10 B/s x
        // loop period x1.5 headroom) -- SATURATES the wire at any baud,
        // near-zero blocking. The ESP32 gives everything it has.
        // ELAPSED-time feed: bytes = wire rate x time since the last drain.
        // The old ema-based formula with a 64-byte FLOOR flooded the FIFO
        // at microsecond loop periods (64 B x 6800 passes/s into a 22.7
        // kB/s wire = blocking sawtooth). Elapsed-based feeding saturates
        // the wire at ANY loop frequency with near-zero blocking; under
        // 16 bytes accumulated, skip and let it build.
        uint32_t nowd = micros();
        uint32_t dtu = this->uart_drain_last_us_ ? (uint32_t) (nowd - this->uart_drain_last_us_) : 20000;
        if (dtu > 200000)
          dtu = 200000;
        uint64_t owed = (uint64_t) baud * dtu / 10000000ULL;
        if (owed < 16)
          quota = 0;   // not worth a write yet: accumulate
        else {
          quota = owed > 256 ? 256 : (size_t) owed;
          this->uart_drain_last_us_ = nowd;
        }
      } else {
        quota = baud ? (size_t) (baud / 5000) : 4;   // software: ~2 ms shield
        if (quota < 4)
          quota = 4;
      }
      size_t room = this->egress_room_(upt);
      if (room < quota)
        quota = room;             // shaped below the wire: the 9600-on-a-19200 demo
      size_t k = upt.buf_n < quota ? upt.buf_n : quota;
      if (k == 0)
        goto uart_done;
      WSER_CRUMB(0x06);
      this->wire_write_(upt.buf, k, true, -1);
      this->egress_spend_(upt, k);
      memmove(upt.buf, upt.buf + k, upt.buf_n - k);
      upt.buf_n -= (uint16_t) k;
    }
  }
uart_done:;
  {
    // governed port service: budget checked BETWEEN ports (never inside an
    // operation), cursor rotates so overload starves nobody in particular
    uint32_t t0 = micros();
    for (int served = 0; served < WSER_MAX_PORTS; served++) {
      int i = (this->net_cursor_ + served) % WSER_MAX_PORTS;
      if (this->ports_[i].used && (this->ports_[i].type == PT_TCP || this->ports_[i].type == PT_UDP)) {
        WSER_CRUMB(0x70 + (uint32_t) i);
        this->port_net_loop_(i);
      } else if (this->ports_[i].used && this->ports_[i].type == PT_BRIDGE && this->ports_[i].loop_peer >= 0) {
        WSER_CRUMB(0x70 + (uint32_t) i);
        this->port_wire_drain_(i);   // TRUNK drain: same fairness, same budget
      }
      if ((uint32_t) (micros() - t0) > WSER_LOOP_BUDGET_US ||
          (uint32_t) (micros() - phase_t0) > this->pool_us_) {
        this->net_cursor_ = (uint8_t) ((i + 1) % WSER_MAX_PORTS);  // resume HERE next pass
        break;
      }
      if (served == WSER_MAX_PORTS - 1)
        this->net_cursor_ = 0;  // full tour completed under budget: reset
    }
  }
  {
    uint32_t spent = (uint32_t) (micros() - phase_t0);
    this->eload_spent_us_ += spent;   // P3: engine-load window
    this->pool_us_ = spent >= this->pool_us_ ? 0 : this->pool_us_ - spent;
  }
phase_skipped:;
#ifdef USE_ESP8266
  s_crumb.passes++;
#endif
  {
    uint32_t pdur = micros() - chrono_pass0;
    if (pdur > this->max_pass_us_) this->max_pass_us_ = pdur;
    WSER_MAXC(mxp, pdur);
  }
  WSER_CRUMB(0x00);  // pass completed: idle between loops
  uint32_t nowu = micros();
  if (this->last_loop_us_ != 0) {
    uint32_t dt = nowu - this->last_loop_us_;
    // clamp raised 100ms -> 1s: at 100ms BOTH bench chips pegged the EMA
    // against the ceiling and read "~99 ms" -- identical-looking, actually
    // unknown. A gauge that saturates in the exact regime you built it
    // for is a liar; 1 s covers any real period while still absorbing
    // one-off aberrations (flash writes, WiFi scans).
    if (dt > 1000000)
      dt = 1000000;
    this->loop_ema_us_ = this->loop_ema_us_ ? (this->loop_ema_us_ * 7 + dt) / 8 : dt;
  }
  this->last_loop_us_ = nowu;
  if (millis() - this->last_rate_ms_ >= 1000) {
    WSER_CRUMB(0x30);   // entered the 1-second window block
    uint32_t chrono_w0 = micros();
    uint32_t dtm = millis() - this->last_rate_ms_;
    if (dtm == 0)
      dtm = 1;
    this->obs_rate_ = (this->observed_ - this->last_observed_) * 1000 / dtm;
    this->tx_rate_ = (this->tx_bytes_ - this->last_tx_b_) * 1000 / dtm;
    {
      uint32_t tot = this->tx_rate_ + this->rx_rate_;
      if (tot > this->peak_rate_)
        this->peak_rate_ = tot;   // session high-water mark
      uint32_t pct = this->eload_spent_us_ / (dtm * 10);   // spent/elapsed %
      this->eload_pct_ = pct > 100 ? 100 : (uint8_t) pct;
      this->eload_spent_us_ = 0;
      this->switched_rate_ = (this->switched_bytes_ - this->sw_last_) * 1000 / dtm;
      this->sw_last_ = this->switched_bytes_;
      for (int wp = 0; wp < WSER_MAX_PORTS; wp++) {
        Port &pw = this->ports_[wp];
        if (!pw.used)
          continue;
        uint32_t cur = pw.tx + pw.rx;
        pw.rate_bps = (cur - pw.rl_last) * 1000 / dtm;
        pw.rl_last = cur;
      }
      {
        uint32_t fl = this->radio_floor_ ? this->radio_floor_ : WSER_RADIO_FLOOR;
        bool red_heap = this->largest_cache_ != 0 && this->largest_cache_ < fl * 3 / 2;
        bool red_duty = this->duty_starved_;
        bool red_back = this->out_.size() > WSER_OUT_SOFT / 2;
        if (red_heap || red_duty || red_back) {
          uint32_t nq = this->drain_quota_ / 2;
#ifdef USE_ESP8266
          this->drain_quota_ = nq < 256 ? 256 : nq;
#else
          this->drain_quota_ = nq < 64 ? 64 : nq;
#endif
          this->quota_dir_ = -1;
          this->throttle_events_++;
          this->throttle_cause_ = red_heap ? 1 : (red_duty ? 2 : 3);
        } else if (this->drain_quota_ < 2048) {
          this->drain_quota_ += 32;
          if (this->drain_quota_ > 2048) this->drain_quota_ = 2048;
          this->quota_dir_ = 1;
        } else { this->quota_dir_ = 0; }
        this->duty_starved_ = false;
      }
      {
        uint32_t wdur = micros() - chrono_w0;
        if (wdur > this->max_win_us_) this->max_win_us_ = wdur;
        WSER_MAXC(mxw, wdur);
      }
      WSER_CRUMB(0x31);   // window math + governor done
    }
    this->rx_rate_ = (this->rx_bytes_ - this->last_rx_b_) * 1000 / dtm;
    this->last_observed_ = this->observed_;
    this->last_tx_b_ = this->tx_bytes_;
    this->last_rx_b_ = this->rx_bytes_;
    this->last_rate_ms_ = millis();
    WSER_CRUMB(0x32);   // entering flush_agg_ (1 Hz WS batch sender)
    this->flush_agg_();
    WSER_CRUMB(0x33);   // flush_agg_ returned
  }
  if (this->stream_client_ != nullptr && !this->serving_page_ && millis() - this->last_info_ > ((this->largest_cache_ != 0 && this->largest_cache_ < WSER_COH_FLOOR) ? 3500 : 2000)) {
    this->last_info_ = millis();
    this->info_pending_ = true;
  }
  // BBSHOTS: unconditional timed replay of the crash report on the logger.
  // Three shots survive API viewer flaps; no page, no client, no gate.
  if (this->bb_valid_ && this->bb_shots_ < 3) {
    uint32_t due = 15000 + 10000 * (uint32_t) this->bb_shots_;
    if (millis() > due) {
      this->bb_shots_++;
      ESP_LOGE(TAG, "BLACK BOX replay %u/3: phase 0x%02X after %u passes -- mxpass %u mxwin %u mxwalk %u us -- last evt %u at pass %u",
               (unsigned) this->bb_shots_, (unsigned) this->bb_phase_, (unsigned) this->bb_passes_,
               (unsigned) this->bb_mxp_, (unsigned) this->bb_mxw_, (unsigned) this->bb_mxh_,
               (unsigned) this->bb_evt_, (unsigned) this->bb_evtp_);
    }
  }
  WSER_CRUMB(0x34);   // loop() truly done -- a wedge past here is OUTSIDE web_serial
}

}  // namespace web_serial
}  // namespace esphome
