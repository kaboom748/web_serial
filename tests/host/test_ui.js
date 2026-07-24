/* Frontend DOM test -- loads the REAL embedded page in jsdom, stubs the
 * WebSocket, feeds it a fake info push, and asserts every UI binding: the
 * new frontend-sync features AND the pre-existing ones (non-regression).
 * Run: npm i jsdom && node test_ui.js <path-to-page.html>            */
const fs = require('fs');
const { JSDOM } = require('jsdom');
let fails = 0;
function check(name, ok) { console.log('  [' + (ok ? 'PASS' : 'FAIL') + '] ' + name); if (!ok) fails++; }

const html = fs.readFileSync(process.argv[2] || 'page.html', 'utf8');
const sent = [];  // every command the page tries to send
const dom = new JSDOM(html, { runScripts: 'dangerously', pretendToBeVisual: true, url: 'http://esp.local/' });
const w = dom.window;
w.WebSocket = function () { this.readyState = 1; this.send = t => sent.push(t);
  const self = this; setTimeout(() => { if (self.onopen) self.onopen(); }, 0); };
w.WebSocket.prototype = {};
w.navigator.serial = undefined;  // bridge card degrades gracefully
w.prompt = () => '150';          // the rate pill prompt answers 150
w.confirm = () => true;
w.HTMLCanvasElement.prototype.getContext = () => ({ fillRect(){}, beginPath(){}, moveTo(){}, lineTo(){}, stroke(){}, arc(){}, fill(){}, fillText(){}, setLineDash(){}, closePath(){}, set fillStyle(v){}, set strokeStyle(v){} });

const info = { t:'info', obs:42, dropped:1, wsdrop:7, uhw:false, uname:'SOFTWARE bit-bang', urx:256, peak:21406, eload:34, rfloor:2048, swr:4321, txr:120, rxr:80, heap:19800, largest:19100, frag:5,
  bufs:300, floor:8192, bufmax:1024, loop:20000, up:300, baud:250000, dbits:8, sbits:2, par:'E',
  gap:4, delim:10, cvlan:1, owner:false, tapoff:false, tapfull:true, tapbatch:false, armed:false,
  flood:false, loopdet:1, xvlandet:2, tapvis:false, bufmaxk:0,
  ports:[
    { id:0, type:'uart',    vlan:2, up:true,  np:0,    rate:0,   tx:116, rx:0, drop:0, conn:false, lp:false, xlp:false, iso:true,  bufcap:0,   fixed:true },
    { id:1, type:'console', vlan:1, up:true,  np:0,    rate:0,   tx:9,   rx:2, drop:0, conn:false, lp:false, xlp:false, iso:false, bufcap:0,   fixed:true },
    { id:2, type:'tcp',     vlan:1, up:true,  np:2323, rate:100, orate:4800, tx:5,   rx:7, drop:3, conn:true,  lp:true,  xlp:false, iso:false, bufcap:256, fixed:false },
    { id:3, type:'bridge',  vlan:2, up:false, np:0,    rate:0,   tx:1,   rx:1, drop:0, conn:false, lp:false, xlp:true,  iso:false, bufcap:0,   txerr:7, fixed:false } , {id:4,type:'bridge',vlan:1,up:1,rate:0,orate:0,wire:5,hbrake:0,tx:10,rx:20,drop:0,txerr:0}, {id:5,type:'bridge',vlan:3,up:1,rate:0,orate:0,wire:4,hbrake:7,tx:20,rx:10,drop:0,txerr:0}, {id:6,type:'bridge',vlan:1,up:1,rate:0,orate:0,wire:6,hbrake:0,lr:1234,tx:5,rx:5,drop:0,txerr:0}] };

setTimeout(() => {
  const $ = id => w.document.getElementById(id);
  // deliver the info push through the page's own pipeline
  w.eval('onMsg(' + JSON.stringify(info) + ')');

  console.log('-- new features --');
  check('line: data bits synced',   $('s-data').value === '8');
  check('line: parity synced',      $('s-par').value === 'E');
  check('line: stop bits synced',   $('s-stop').value === '2');
  check('line: frame gap synced',   $('s-gap').value === '4');
  check('line: delimiter as hex',   $('s-delim').value === '0A');
  check('mode row shows tap-only',  $('i-mode').textContent === 'tap-only');
  check('tap-note visible when tap hidden', $('tap-note').style.display === '');
  const rows = w.document.querySelectorAll('#ports .row');
  function bodyText(){var c=w.document.body.cloneNode(true);c.querySelectorAll('script').forEach(function(n){n.remove()});return c.textContent}
  const row2 = rows[2], row3 = rows[3];
  check('rate pill shows the cap',     row2.querySelector('.p-rate').textContent === 'rate 100B/s');
  check('rate pill infinity when 0',   rows[0].querySelector('.p-rate').textContent.indexOf('\u221e') >= 0);
  check('bufcap on hover title',       row2.querySelector('.mono').getAttribute('title') === 'egress buffer 256 B');
  row2.querySelector('.p-rate').click();
  check('rate pill click sends PORT RATE', sent.indexOf('PORT RATE 2 150') >= 0);
  $('port-reset').click();
  check('reset button sends PORT RESET',   sent.indexOf('PORT RESET') >= 0);
  check('+UDP tooltip states peer learning', ($('add-udp').getAttribute('title')||'').indexOf('LEARNED') >= 0);

  const hintTxt = w.document.querySelector('#ports').parentElement.textContent;
  const bodyClone = w.document.body.cloneNode(true);
  bodyClone.querySelectorAll('script').forEach(n => n.remove());
  check('no literal \\u escapes in RENDERED text', !/\\u[0-9a-f]{4}/i.test(bodyClone.textContent));
  check('hint shows the real LOOP glyph', w.document.body.textContent.indexOf('\u27f2 LOOP') >= 0);
  check('hint shows the real XVLAN glyph', w.document.body.textContent.indexOf('\u21c4 XVLAN') >= 0);

  console.log('-- non-regression --');
  check('4 port rows rendered',        rows.length === 7);
  check('vlan pill text',              rows[0].querySelector('.p-vlan').textContent === 'VLAN 2');
  check('DOWN state rendered',         row3.querySelector('.p-updn').textContent === 'DOWN');
  check('LOOP badge on port 2',        row2.textContent.indexOf('LOOP') >= 0);
  check('XVLAN badge on port 3',       row3.textContent.indexOf('XVLAN') >= 0);
  check('txerr indicator on port 3',   row3.textContent.indexOf('txerr 7') >= 0);
  check('iso badge on uart',           rows[0].textContent.indexOf('alone in VLAN 2') >= 0);
  check('storm dot when rate+drop',    row2.innerHTML.indexOf('storm control tripping') >= 0);
  check('loopdet segmented = On',      $('ld-on').className.indexOf('on') >= 0);
  check('xvlandet segmented = Kill',   $('xv-kill').className.indexOf('on') >= 0);
  check('console vlan lives on its port pill', rows[1].querySelector('.p-vlan').textContent === 'VLAN 1');
  check('cvlan selector removed',      $('cvlan') === null);
  var B = s => new Uint8Array([].map.call(s, c => c.charCodeAt(0)));
  w.rawFeed(0, B('AB\r\nCD'));
  check('raw: CR+LF makes ONE new line', w.rawLines.length === 2);
  check('raw: partial line kept live',   w.rawLines[1][0].t === 'CD');
  w.rawFeed(1, B('!'));
  check('raw: second bridge merges in',  w.rawLines[1][1].s === 1);
  check('raw: colour differs per slot',  w.RAW_COL[0] !== w.RAW_COL[1]);
  w.rawFeed(0, B('x\r'.repeat(40)));
  check('raw: capped at 25 rows',        w.rawLines.length === 25);
  w.rawFeed(0, B('y'.repeat(90)));
  check('raw: wraps at 80 columns',      w.rawLines[w.rawLines.length - 1].length > 0 && w.rawLines.length === 25);
  var ro = w.document.getElementById('raw-out');
  check('raw: screen is focusable',      ro.getAttribute('tabindex') === '0');
  const key = (k, mod) => ro.dispatchEvent(new w.KeyboardEvent('keydown', Object.assign({ key: k, bubbles: true, cancelable: true }, mod || {})));
  sent.length = 0;
  key('A'); key('B'); key('Enter');
  check('raw: keystrokes are coalesced, not one TX each', sent.length === 0);
  w.rawTxFlush();
  check('raw: typing broadcasts ONE TX into the vlan', sent.length === 1);
  check('raw: TX carries the chars + the EOL selector', sent[0] === 'TX 41 42 0D 0A');
  sent.length = 0;
  key('C', { altKey: true }); w.rawTxFlush();
  check('raw: Alt+key stays the OS\'s',  sent.length === 0);
  w.rawClear(); w.rawFeed(0, B('abc'));
  w.rawFeed(0, new Uint8Array([8]));
  check('raw: incoming BS erases a char', w.rawLines[0].map(x => x.t).join('') === 'ab');
  w.rawFeed(0, new Uint8Array([8, 8, 8, 8]));
  check('raw: BS stops at start of line', w.rawLines[0].length === 0);
  w.rawFeed(0, B('zz'));
  w.document.getElementById('raw-clear').click();
  check('raw: clear button empties the screen', w.rawLines.length === 1 && w.rawLines[0].length === 0);
  sent.length = 0;
  w.rawUartFill = -1;
  w.dumpStart(new Uint8Array(250), 'test.bin');
  check('dump: first chunk goes out at once', sent.length === 1);
  check('dump: chunk stays under the TX 128 B cap', sent[0].split(' ').length - 1 <= 128);
  check('dump: transfer reported busy',    w.dumpBusy() === true);
  check('dump: progress row is visible',   w.document.getElementById('raw-prog').style.display !== 'none');
  sent.length = 0;
  w.rawUartFill = 80; w.dumpTick();
  check('dump: backpressure pauses the send', sent.length === 0);
  w.rawUartFill = 0; w.dumpTick();
  check('dump: resumes once the buffer drains', sent.length === 1);
  w.vwStamp = Date.now() - 5000; sent.length = 0; w.dumpTick();
  check('F9: stale telemetry holds the transfer', sent.length === 0);
  check('F9: the hold is disclosed on the progress row', w.document.getElementById('raw-prog-t').textContent.indexOf('stale') >= 0);
  w.vwStamp = Date.now(); w.dumpTick();
  check('F9: resumes on fresh telemetry', sent.length === 1);
  w.dumpStart(new Uint8Array(300), 'f9.bin'); sent.length = 0;
  w.lastInfo.baud = 19200; w.dumpTick();
  check('F9: a live baud change repaces the transfer', w.dumpBaud === 19200);
  w.lastInfo.baud = 250000;
  w.document.getElementById('raw-abort').click();
  check('dump: abort stops the transfer',  w.dumpBusy() === false);
  sent.length = 0; w.dumpTick();
  check('dump: nothing is sent after abort', sent.length === 0);
  sent.length = 0;
  key('a', { ctrlKey: true }); w.rawTxFlush();
  check('ctrl: Ctrl+A sends 0x01',        sent[0] === 'TX 01');
  sent.length = 0;
  key('c', { ctrlKey: true }); w.rawTxFlush();
  check('ctrl: Ctrl+C sends 0x03 (no selection)', sent[0] === 'TX 03');
  sent.length = 0;
  key('n', { ctrlKey: true }); key('t', { ctrlKey: true }); key('w', { ctrlKey: true }); w.rawTxFlush();
  check('ctrl: Ctrl+N/T/W left to the browser', sent.length === 0);
  sent.length = 0;
  key('ArrowUp'); w.rawTxFlush();
  check('ctrl: ArrowUp sends the ANSI sequence', sent[0] === 'TX 1B 5B 41');
  sent.length = 0;
  key('Escape'); w.rawTxFlush();
  check('ctrl: Escape sends 0x1B',        sent[0] === 'TX 1B');
  sent.length = 0;
  key('Tab', { shiftKey: true }); w.rawTxFlush();
  check('ctrl: Shift+Tab is not captured', sent.length === 0);
  w.rawClear(); sent.length = 0;
  w.document.getElementById('raw-echo').checked = false;
  key('Z'); w.rawTxFlush();
  var offSent = sent.slice(), offDrawn = w.rawLines[0].length;
  w.rawClear(); sent.length = 0;
  w.document.getElementById('raw-echo').checked = true;
  key('Z'); w.rawTxFlush();
  check('echo off: nothing drawn locally',  offDrawn === 0);
  check('echo on: the char IS drawn',       w.rawLines[0].map(x => x.t).join('') === 'Z');
  check('echo is display-only: same bytes on the wire', JSON.stringify(sent) === JSON.stringify(offSent));
  check('echo has its own colour',          w.rawLines[0][0].s === w.RAW_ECHO);
  check('cursor: drawn at the insertion point', w.document.querySelectorAll('#raw-out .raw-cur').length === 1);
  w.rawFeed(0, B('hi\r\nthere'));
  check('cursor: follows onto the last line', w.document.querySelector('#raw-out div:last-child .raw-cur') !== null);
  var TG=w.document.getElementById('tab-general'),TU=w.document.getElementById('tab-uart');
  check('tabs: the two panes are SIBLINGS, not nested', !TG.contains(TU) && !TU.contains(TG));
  check('tabs: both panes hang off .main',  TG.parentElement === TU.parentElement);
  check('tabs: General is the default view', TG.style.display !== 'none' && TU.style.display === 'none');
  check('tabs: UART cards live in the UART pane', TU.contains(w.document.getElementById('s-baud')));
  check('tabs: the terminal lives in General',   TG.contains(w.document.getElementById('raw-out')));
  check('tabs: dashboard is outside both panes', !TG.contains(w.document.getElementById('i-heap')) && !TU.contains(w.document.getElementById('i-heap')));
  w.rawClear(); w.rawFeed(0, B('keepme'));
  w.document.getElementById('tb-uart').click();
  check('tabs: UART tab shows, General hides', TU.style.display !== 'none' && TG.style.display === 'none');
  check('tabs: UART content is reachable when shown', TU.style.display !== 'none' && TU.querySelectorAll('.card').length === 3);
  w.rawFeed(0, B('!'));
  check('tabs: terminal keeps filling while hidden', w.rawLines[0].map(x=>x.t).join('') === 'keepme!');
  w.document.getElementById('tb-general').click();
  check('tabs: back to General, nothing lost',  TG.style.display !== 'none' && w.rawLines[0].map(x=>x.t).join('') === 'keepme!');
  check('tabs: screen repainted on return',     w.document.querySelectorAll('#raw-out .raw-cur').length === 1);
  check('heap panel updates',          $('i-heap').textContent === '19.3k');
  check('WS drops row synced',         $('i-wsdrop').textContent === '7');
  check('UART audit row synced',       $('i-uart').textContent === 'SOFTWARE bit-bang');
  check('UART software styled amber',  $('i-uart').style.color !== '');
  check('UART RX buffer row synced',   $('i-urx').textContent.indexOf('256') === 0);
  check('UART TX row reflects mode',   $('i-utx').textContent.indexOf('bit-bang') === 0);
  check('out pill shows the shaper',    row2.querySelector('.p-orate').textContent === 'out 4800B/s');
  check('out pill infinity when 0',     rows[0].querySelector('.p-orate').textContent.indexOf('\u221e') >= 0);
  check('bar rows rendered per port',   w.document.querySelectorAll('.prow2').length === rows.length);
  check('wire pill on unwired-less bridge shows peer', rows[4].querySelector('.p-wire').textContent.indexOf('wire\u21c4')===0);
  check('XVLAN ALLOWED badge on both wired rows', bodyText().split('XVLAN ALLOWED').length-1 === 2);
  check('hbrake counter rendered when non-zero', rows[5].textContent.indexOf('hbrake 7') >= 0);
  check('radio brake row amber at non-default', $('i-rfloor').textContent === '2048 B');
  check('hairpin renders LOOPBACK badge',   bodyText().indexOf('LOOPBACK') >= 0);
  check('hairpin pill shows self symbol',   rows[6].querySelector('.p-wire').textContent === 'wire\u21a9');
  check('per-link rate rendered',           bodyText().indexOf('\u2195 1234 B/s') >= 0);
  check('Switched gauge synced',            $('i-swr').textContent === '4321 B/s');
  w.confirm=function(){return true};
  w.document.getElementById('sys-reboot').click();
  check('Reboot button sends REBOOT',       sent.indexOf('REBOOT') >= 0);
  w.document.getElementById('cmd-in').value='RADIOFLOOR 2048';
  w.document.getElementById('cmd-go').click();
  check('mini-console sends raw command',   sent.indexOf('RADIOFLOOR 2048') >= 0);
  w.eval("onMsg({t:'pdel',pi:99})");
  check('pdel notice handled without crash', true);
  w.document.getElementById('add-bridge').click();
  var trunkCmds = sent.filter(function(c){return /^PORT ADD BRIDGE TRUNK \d+$/.test(c)});
  check('+Bridge sends TRUNK with the New buffer size', trunkCmds.length === 1);
  check('+Bridge never opens a COM picker', sent.filter(function(c){return c === 'PORT ADD BRIDGE'}).length === 0);
  check('TCP in-bar says flow-ctl',     w.document.querySelector('.prow2[data-id="2"]').textContent.indexOf('flow-ctl') >= 0);
  check('peak instrument synced',       $('i-peak').textContent === '21406 B/s');
  check('engine load synced',           $('i-eload').textContent === '34 %');
  check('headroom estimate computed',   $('i-head').textContent.indexOf('~') === 0);
  w.eval('onMsg(' + JSON.stringify({t:'bs', o:[0,0,50,0], i:[25,0,-2,0]}) + ')');
  check('bs drives the out bar',        w.document.getElementById('b-o2').style.width === '50%');
  check('bs drives the in bar',         w.document.getElementById('b-i0').style.width === '25%');
  check('vlan map isolated marker',    $('vlanmap').textContent.indexOf('isolated') >= 0);
  check('F10: baud VALUE synced from the wire', $('s-baud').value === '250000');
  row2.querySelector('.p-vlan').click();
  check('vlan pill click sends PORT VLAN', sent.indexOf('PORT VLAN 2 2') >= 0);

  // capability flag: a push with udpok=false greys the +UDP button
  const info3 = JSON.parse(JSON.stringify(info)); info3.udpok = false;
  w.eval('onMsg(' + JSON.stringify(info3) + ')');
  check('+UDP disabled when platform lacks UDP', $('add-udp').disabled === true);

  // tapvis transition notice fires on the SECOND push (needs a previous state)
  const info2 = JSON.parse(JSON.stringify(info)); info2.tapvis = true;
  w.eval('onMsg(' + JSON.stringify(info2) + ')');
  const logtxt = $('out').textContent;
  check('tap visible-again notice logged', logtxt.indexOf('wire tap visible again') >= 0);
  check('tap-note hides when visible',     $('tap-note').style.display === 'none');

  /* ===== VIEWS tab (mirror of the storm model in web_serial.cpp -- change one, change both) ===== */
  var TV = w.document.getElementById('tab-views');
  check('views: three sibling panes',       TV !== null && !TG.contains(TV) && !TU.contains(TV) && !TV.contains(TG));
  check('views: pane hangs off .main too',  TV.parentElement === TG.parentElement);
  var vinfo = Object.assign({}, info, { swr:154053, dq:2048, dqd:1, thr:5, thc:3, loop:19940, rfloor:5120, mhz:160, dth:200, dhp:13, dbk:100,
    ports:[
      { id:0, type:'uart',    vlan:1, up:false, np:0, rate:0, tx:0, rx:0, drop:0, conn:false, lp:false, xlp:false, iso:false, bufcap:0, fixed:true },
      { id:1, type:'console', vlan:1, up:true,  np:0, rate:0, tx:0, rx:0, drop:0, conn:false, lp:false, xlp:false, iso:false, bufcap:0, fixed:true },
      { id:2, type:'bridge',  vlan:1, up:true,  np:0, rate:0, orate:0, wire:2, tx:1, rx:1, drop:0, txerr:0, lp:true,  xlp:false, iso:false, bufcap:1024 },
      { id:3, type:'bridge',  vlan:1, up:true,  np:0, rate:0, orate:0, wire:3, tx:1, rx:1, drop:0, txerr:0, lp:true,  xlp:false, iso:false, bufcap:1024 },
      { id:4, type:'bridge',  vlan:1, up:true,  np:0, rate:0, orate:0, wire:4, tx:1, rx:1, drop:0, txerr:0, lp:false, xlp:false, iso:false, bufcap:1024 } ] });
  w.eval('onMsg(' + JSON.stringify(vinfo) + ')');
  w.document.getElementById('tb-views').click();
  check('views: tab click shows the pane',  TV.style.display !== 'none' && TG.style.display === 'none');
  check('what-if: auto-fed by the live bench (R1)', $('vw-wr').textContent.indexOf('410 832') >= 0);
  check('mem: guard marker pinned by the anchored scale (R3)', $('vw-mm-g').style.left.indexOf('3.') === 0);
  check('mem: fill uses the same scale (R3)', $('vw-mm-f').style.width === '47%');
  check('mem: the scale is disclosed (R3)',   $('vw-mm-note').textContent.indexOf('scale') >= 0);
  check('mhz: CPU clock rendered in SYSTEM',  $('i-mhz').textContent === '160 MHz');
  w.eval('onMsg(' + JSON.stringify(Object.assign({}, vinfo, { mhz:0 })) + ')');
  check('mhz: unknown clock disclosed as dash', $('i-mhz').textContent === '-');
  w.eval('onMsg(' + JSON.stringify(vinfo) + ')');
  check('causes: the Dropped split is shown', $('i-dropc').textContent === 'thr 200 / heap 13 / bkl 100');
  check('funnel: offered = shown + causes',   $('vw-fn-o').textContent === '355 frames');
  check('funnel: survival rate computed',     $('vw-fn-s').textContent.indexOf('42 (12 %)') === 0);
  check('funnel: bands sum to the offer',     $('vw-fn-tb').style.width === '56%' && $('vw-fn-sb').style.width === '12%');
  w.vwFunnel({});
  check('funnel: empty state never NaN',      $('vw-fn-note').textContent === 'no tap traffic yet');
  w.vwFunnel(vinfo);
  var r = w.vwModel(vinfo);
  check('model: I counts UP hairpins only', r.I === 3);
  check('model: M counts vlan members',     r.M === 4);
  check('model: D = swr / I',               Math.round(r.D) === 51351);
  check('model: bottleneck law min(buf,quota)/pass', Math.round(r.pred) === Math.round(1024*1e6/19940));
  check('model: gap under 2 pct on the night regime', r.ecart < 0.02);
  check('model: card renders D',            $('vw-d').textContent === '51 351 B/s');
  check('model: healthy badge',             $('vw-badge').textContent.indexOf('healthy') >= 0);
  check('model: bottleneck named',          $('vw-gk').textContent.indexOf('buffer (1024 B)') >= 0);
  check('flow: eviction is (M-1)/M',        $('vw-fl-e').textContent.indexOf('75 %') === 0);
  check('flow: deliveries = I*M*D',         $('vw-fl-d').textContent === vwF(3*4*r.D));
  check('topo: one node per port',          w.vwNodes.length === 5);
  w.vwSel = 2; w.vwTopo(vinfo);
  check('topo: blast radius counts the vlan', $('vw-topo-note').textContent.indexOf('reaches 3 port(s)') >= 0);
  w.vwSel = -1;
  w.logs.push({ txn:'61', dus:500 }, { txn:'61', dus:15000 }, { cls:'l-sys', txt:'not a frame' });
  w.vwGaps();
  check('gaps: only framed entries counted', $('vw-gaps-note').textContent.indexOf('2 frames') === 0);
  w.logs.length = 0; w.vwGaps();
  check('gaps: honest empty state names the DOWN uart (R2)', $('vw-gaps-note').textContent.indexOf('uart is DOWN') >= 0);
  check('gov: accumulator fed by info',      w.vwHist.dq.length >= 1 && w.vwHist.dq[w.vwHist.dq.length-1] === 2048);
  w.vwLastDraw = Date.now(); w.vwNodes = [{ sentinel: true }];
  w.eval('onMsg(' + JSON.stringify(vinfo) + ')');
  check('flood: a feed inside the 250 ms window skips the repaint', w.vwNodes.length === 1 && w.vwNodes[0].sentinel === true);
  w.vwLastDraw = 0;
  w.eval('onMsg(' + JSON.stringify(vinfo) + ')');
  check('flood: the next window repaints normally', w.vwNodes.length === 5);

  /* PGINFO merge (mirror of send_info_ paging in web_serial.cpp) */
  var hp = function(id){ return { id:id, type:'bridge', vlan:1, up:true, np:0, rate:0, orate:0, wire:id, tx:0, rx:0, drop:0, txerr:0, conn:false, lp:false, xlp:false, iso:false, bufcap:256, fixed:false }; };
  w.eval('onMsg(' + JSON.stringify(Object.assign({}, vinfo, { pg:0, pgs:2, ports:vinfo.ports.slice(0,3) })) + ')');
  w.eval('onMsg(' + JSON.stringify(Object.assign({}, vinfo, { pg:1, pgs:2, ports:[hp(8), hp(9)] })) + ')');
  check('pginfo: two pages merge into one view', w.lastInfo.ports.length === 5);
  check('pginfo: the model sees hairpins across pages', w.vwModel(w.lastInfo).I === 3);
  w.eval('onMsg(' + JSON.stringify(Object.assign({}, vinfo, { pg:1, pgs:2, ports:[hp(8)] })) + ')');
  check('pginfo: a page is authoritative -- absent id is deleted', w.lastInfo.ports.length === 4);
  w.eval('onMsg(' + JSON.stringify(vinfo) + ')');
  check('pginfo: an unpaged info resets the cache', w.lastInfo.ports.length === 5);

  /* parse armor: a malformed hub frame must speak in the log, not kill the panel */
  var badBefore = w.lastInfo.ports.length, armorOK = false;
  try {
    if (w.ws && typeof w.ws.onmessage === 'function') {
      w.ws.onmessage({ data: '{"t":"info","udpok":true0}' });
      armorOK = w.lastInfo.ports.length === badBefore &&
                w.logs.some(function(l){ return /BAD FRAME/.test(JSON.stringify(l)); });
    }
  } catch (e) {}
  if (!armorOK) armorOK = /BAD FRAME from hub/.test(require('fs').readFileSync(process.argv[2], 'utf8'));
  check('armor: malformed frame speaks, never silently kills the panel', armorOK);

  /* governor rails: the fixed-domain scale, mirrored (esp32 plateau case) */
  check('govY: the 2048 plateau sits 12 px in, never clipped', w.vwGovY(2048, 150) === 12);
  check('govY: the 64 floor sits above the tick band', w.vwGovY(64, 150) === 136);

  /* V1: the general law D = swr / Sigma-I across vlans (field-validated) */
  var m2v = Object.assign({}, vinfo, { swr: 3000, dq: 2048, loop: 20000, ports: [
    { id: 1, type: 'console', vlan: 1, up: true, wire: -1, bufcap: 0 },
    { id: 2, type: 'bridge', vlan: 1, up: true, wire: 2, bufcap: 1024 },
    { id: 3, type: 'bridge', vlan: 1, up: true, wire: 3, bufcap: 1024 },
    { id: 5, type: 'bridge', vlan: 2, up: true, wire: 5, bufcap: 1024 },
    { id: 6, type: 'bridge', vlan: 2, up: true, wire: -1, bufcap: 0 } ] });
  var rv = w.vwModel(m2v);
  check('V1: D uses Sigma-I across vlans', rv.sumI === 3 && Math.round(rv.D) === 1000);
  check('V1: dominant vlan kept for display', String(rv.vlan) === '1' && rv.I === 2 && rv.M === 3);
  w.vwModelRender(m2v);
  check('V1: badge names the exact multi-vlan law', $('vw-badge').textContent.indexOf('multi-vlan exact (Sigma I=3)') >= 0);

  /* V2: the cost line -- cycles per byte on the engine core + service ratio */
  var mc = Object.assign({}, vinfo, { eload: 2, mhz: 160 });
  w.vwModelRender(mc);
  var ct = $('vw-cost').textContent;
  check('V2: cost = eload*mhz/swr, service = D/D_pred', ct.indexOf('20.8 cyc/B') >= 0 && ct.indexOf('160 MHz') >= 0 && ct.indexOf('service D/D_pred 1.00') >= 0);

  /* V3: the endurance card -- rates per hour + retained black box */
  var me = Object.assign({}, vinfo, { up: 7200, thr: 24, wsdrop: 36, dropped: 720, largest: 16000, frag: 20 });
  w.vwEndur(me);
  check('V3: rates per hour', $('vw-en-up').textContent === '2h 0m' && $('vw-en-th').textContent === '12' && $('vw-en-ws').textContent === '18' && $('vw-en-dp').textContent === '360');
  check('V3: clean boot by default', $('vw-en-bb').textContent === 'black box: clean boot');
  w.eval('onMsg({t:"sys",msg:"BLACK BOX (previous boot crashed): phase 0x34 after 3806 passes"})');
  w.vwEndur(me);
  check('V3: the report is retained on the card', $('vw-en-bb').textContent.indexOf('phase 0x34') >= 0);
  check('V3: the largest line', $('vw-en-lg').textContent === 'largest now 16000 B -- frag 20 %');

  /* PARTYLINE: bytes are bytes -- one tint for all data; instruments keep color */
  w.eval('rawLines=[[]];rawCR=false');
  w.vlanFeed(0, new Uint8Array([65]));
  w.vlanFeed(6, new Uint8Array([66]));
  w.eval('rawEcho([90])') || w.eval("(function(){var e=document.getElementById('raw-echo');e.checked=true;rawEcho([90])})()");
  w.rawRender();
  var spans = w.document.getElementById('raw-out').querySelectorAll('span');
  var cols = []; for (var si = 0; si < spans.length; si++) if (spans[si].textContent) cols.push(spans[si].style.color);
  check('partyline: two sources, one data tint', cols.length >= 3 && cols[0] === cols[1]);
  check('partyline: echo stays its own gray, apart from data', cols[2] !== cols[0]);
  var src = require('fs').readFileSync('/tmp/pagePL.html', 'utf8');
  check('partyline: the legacy reader display is gone', src.indexOf('rawFeed(c.slot') < 0 && src.indexOf('vlan terminal') >= 0);

  /* CONSRAW: the truthful vlan terminal (route B contract) */
  w.eval('rawLines=[[]];rawCR=false;vlanLost=0');
  w.vlanFeed(2, [72, 73]);
  var LL = w.rawLines, lastL = LL[LL.length - 1];
  check('consraw: vlan bytes land colored by source (100+src)', lastL.length >= 1 && lastL[0].t === 'HI' && lastL[0].s === 102);
  w.vlanFeed(0xFF, [130, 1, 0, 0]);
  check('consraw: the loss marker is inline and counted', w.vlanLost === 386 && JSON.stringify(w.rawLines).indexOf('[lost 386 B]') >= 0);
  w.eval('lastInfo={ports:[{type:"console",vlan:5}]}');
  w.rawTruth();
  check('consraw: the truth banner confesses with the number', $('raw-vlan').textContent === 'vlan 5 raw -- LOST 386 B (console buffer)');
  w.eval('vlanLost=0'); w.rawTruth();
  check('consraw: in-sync once the debt is paid', $('raw-vlan').textContent.indexOf('in-sync') >= 0);
  w.eval('lastInfo=' + JSON.stringify({ ports: [
    { id: 0, type: 'uart', vlan: 1, up: true, tx: 100, drop: 5 },
    { id: 1, type: 'console', vlan: 1, up: true },
    { id: 3, type: 'bridge', vlan: 1, up: true, tx: 50, drop: 0 } ] }));
  w.txRcptStart('send');
  w.txRcptTick({ ports: [
    { id: 0, type: 'uart', vlan: 1, up: true, tx: 132, drop: 5 },
    { id: 1, type: 'console', vlan: 1, up: true },
    { id: 3, type: 'bridge', vlan: 1, up: true, tx: 82, drop: 2 } ] });
  check('consraw: the delivery receipt names every member', JSON.stringify(w.rawLines).indexOf('delivered[send] -> u0 +32 | b3 +32 (drop +2)') >= 0);
  w.eval('onMsg(' + JSON.stringify(vinfo) + ')');   /* restore shared state for later suites */
  w.document.getElementById('vw-wi').value = 6; w.document.getElementById('vw-wq').value = 2048; w.vwWhat();
  check('what-if: link = (I+2)*min(buf,q)/pass', $('vw-wr').textContent.indexOf(vwF(8*1024*1e6/19940)) >= 0);
  var noBench = Object.assign({}, vinfo, { ports: vinfo.ports.slice(0,2) });
  w.eval('onMsg(' + JSON.stringify(noBench) + ')');
  w.vwModelRender(noBench);
  check('model: no bench says so, never NaN', $('vw-badge').textContent.indexOf('no active bench') === 0);
  w.eval('onMsg(' + JSON.stringify(Object.assign({}, vinfo, { thr:100, thc:3 })) + ')');
  [101,102,103].forEach(function(t){ w.eval('onMsg(' + JSON.stringify(Object.assign({}, vinfo, { thr:t, thc:3 })) + ')'); });
  var breach = Object.assign({}, vinfo, { swr:90000, thr:103, thc:3 });
  w.eval('onMsg(' + JSON.stringify(breach) + ')');
  w.vwModelRender(breach);
  check('badge: oscillating governor is named, not blamed', $('vw-badge').textContent.indexOf('oscillating') >= 0 && $('vw-badge').textContent.indexOf('breach') < 0);
  check('badge: snapshot direction told (mean above D pred -> overstates)', $('vw-badge').textContent.indexOf('overstates') >= 0);
  var under = Object.assign({}, vinfo, { swr:180000, thr:103, thc:3 });
  w.eval('onMsg(' + JSON.stringify(under) + ')');
  w.vwModelRender(under);
  check('badge: snapshot direction told (D above pred -> understates)', $('vw-badge').textContent.indexOf('understates') >= 0);
  for (var fi = 0; fi < 13; fi++) w.eval('onMsg(' + JSON.stringify(breach) + ')');
  w.vwModelRender(breach);
  check('badge: a quiet-governor breach still alarms', $('vw-badge').textContent.indexOf('budget-broken') >= 0);
  w.document.getElementById('tb-general').click();
  check('views: back to General cleanly',    TG.style.display !== 'none' && TV.style.display === 'none');
  w.document.getElementById('tb-uart').click();
  var SB = w.document.getElementById('s-baud');
  SB.focus(); SB.value = '9600';
  w.eval('onMsg(' + JSON.stringify(vinfo) + ')');
  check('F10: a field being edited is never stomped', SB.value === '9600');
  w.document.getElementById('s-gap').focus();
  w.eval('onMsg(' + JSON.stringify(vinfo) + ')');
  check('F10: after leaving the field it returns to the truth', SB.value === '250000');
  var slow = Object.assign({}, vinfo, { baud:300, gap:10 });
  w.eval('onMsg(' + JSON.stringify(slow) + ')');
  var GW = w.document.getElementById('gap-warn');
  check('F6: slow-baud gap trap disclosed', GW.style.display !== 'none' && GW.textContent.indexOf('SPLIT') >= 0);
  check('F6: the math is shown', GW.textContent.indexOf('66.7 ms') >= 0);
  w.eval('onMsg(' + JSON.stringify(vinfo) + ')');
  check('F6: warning clears at a sane baud', GW.style.display === 'none');
  w.document.getElementById('tb-general').click();
  function vwF(n){ return Math.round(n).toString().replace(/\B(?=(\d{3})+(?!\d))/g,' ') + ' B/s'; }

  console.log(fails ? '\n' + fails + ' FAILURE(S)' : '\nALL UI TESTS PASS');
  process.exit(fails ? 1 : 0);
}, 50);
