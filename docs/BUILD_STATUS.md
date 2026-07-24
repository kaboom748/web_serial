# web_serial -- BUILD STATUS

## ETAT COURANT (2026-07-23, fin de session 4 -- l'apres-guerre de l'allocateur)
- Build: STABLE+INFOSKEL (ALLOCQUIET + soupape de cohabitation + pg brace +
  parse armor). Plateformes verifiees terrain : 8266 @ 80 ET 160 MHz,
  ESP32-IDF @ 240 MHz.
- LA BETE HWDT EST CLOSE : collision umm/lwip en contexte SYS (Piege 50),
  instruite par 6 crashs, 2 piles decodees, 3 experiences discriminantes.
  Preuve terrain : config tueuse + tempete complete, uptime 382 s+ (la bete
  tuait en 27-65 s), panneau vivant, modele a 1 %, conservation exacte.
- Suites : 8/8 vertes, 159 asserts UI, squelette JSON valide par json.loads
  sur les DEUX branches plateforme, miroirs arithmetiques (porte 5824,
  anti-empilement 512+3200<4096, voie de controle, app_n, etage 6144>5120).
- Soupapes canoniques INTACTES + l'oignon documente : 7680 bleu-gouverneur /
  6144 cohabitation / 5120 frein radio / 1536 garde tap.
- RELEASE v1.0.0 officialisee : README refondu (statut field-hardened,
  install github://, Views + terminal + index docs), guide navigateur
  PUBLIER-WEB.md, CI 4 jobs (host 8 suites + matrice 3 cibles),
  exemples 3 plateformes, layout docs/ pour le depot.
- Docs a jour : PIEGES v2.3 (50-56), CONFIG_ESP8266_TERRAIN.md,
  README (oignon), GUIDE (instruments), LESSONS (session 4).

## RESTE A FAIRE
- CERTIFICAT FINAL : soak de nuit dans la config ATTACHEE (page + tempete)
  -- les anciens soaks immunises tournaient detaches. Matin sans ligne
  BLACK BOX = enterrement officiel. Vigie : plateau du largest.
- Panneau Views : TERMINE (V1+V2+V3 livres, 8 cartes). Extensions au
  grenier : delta A/B, spectre, modbus, budget de passe (seul backend).
- Extensions optionnelles : delta A/B, spectre d'octets, transactions
  Modbus, budget de passe (seul backend : 3-4 chronos).
- Hors Views : F1/F4 backend (attente FIFO plafonnee + purge tampon LINE) ;
  hygiene switch (repaint optimiste, buf_n=0 sur DOWN hairpin, frein radio
  restreint aux loopy).
- Arbitrages capitaine : persistance LINE (magic 0x5C ?) ; profondeur seau
  shaper (1 s vs 250 ms) ; power_save_mode par defaut du banc (reco: none).

---

## JOURNAL CHRONOLOGIQUE (verbatim, ordre d'ecriture)

## RELEASE100 -- v1.0.0, l'officialisation
- README refondu : le WARNING ALPHA (devenu faux) remplace par le statut
  field-hardened chiffre ; badge CI ; install external_components
  github:// ; sections neuves (les trois onglets dont Views 8 cartes, le
  terminal-verite et son contrat, l'index docs/) ; section tests 8 suites
  / 177 asserts / run_all.sh.
- Publication navigateur : PUBLIER-WEB.md convertie depuis le modele
  oled_stream du capitaine (etat des lieux, piege du dossier parent,
  .gitignore + .github/workflows/ci.yml a la main, LICENSE via le
  selecteur GPLv3, About/topics, release v1.0.0 avec notes completes).
- CI : host-tests (run_all.sh -- extraction PROGMEM + 6 C + decoder +
  UI jsdom) + matrice esp8266/esp32-arduino/esp32-idf (secrets generes
  au vol). Exemple IDF cree ; en-tete du yaml 8266 corrige (disait ESP32).
- GUIDE : section LOGGING (les trois robinets du plan d'observation,
  l'heritage assume, la grille calme/tempete, l'independance du terminal).
- Livrables : web_serial_github_v1.0.0.zip (layout depot, docs/ regroupe
  les 5 memos dont PIEGES) + le zip de bord habituel rafraichi.

## GATEHOLE -- the green gate itself had a hole (found by shipping through it)
- The VLAN_COL decl was COMMA-CHAINED (var VLAN_COL=[...],vlanLost=0;):
  the removal regex left an orphan ',vlanLost=0;' -- SyntaxError, page
  dead, ZERO asserts executed... and the gate read 'FAIL==0' as green and
  SHIPPED a broken page. Two permanent rules: (1) never regex-remove a
  var declaration (comma-chaining) -- exact-string surgery only; (2) the
  gate requires node --check PASS *and* a PASS floor (>=176), never just
  zero failures. Repair: 'var vlanLost=0;'. Piege 54 family compounds.

## PARTYLINE -- the captain's doctrine: bytes are bytes
- Field verdict on the double echo: the LEGACY reader-loop local display
  (rawFeed(c.slot) at the 0x01 send site) showed INTENTION pre-verdict;
  the 0x03 mirror shows post-verdict truth. The legacy line is removed --
  the vlan mirror is the terminal's ONLY data source. Round-trip cost for
  browser-born bytes: ~2x serial rate on a WS link with 100x headroom.
- Colors: ONE neutral tint for all data (com/bridge/uart/tcp -- a party
  line, an RS485 bus on a scope). Color is reserved for INSTRUMENTS:
  lost=red, receipt=blue, local echo=gray. VLAN_COL palette removed;
  RAW_COL survives for the header link labels only. The wire keeps the
  src byte (0xFF confession + future use); only the paint died.
- Panel retitled 'vlan terminal'. Scope rule now uniform: a COM whose
  bridge lives in another vlan is out of display scope (move the console
  to see it -- the tap's rule). 3 new contracts, 177 UI asserts, board
  green-gated (and the gate caught a red mid-flight: no ship happened).

## CONSTRUTH -- the truth machinery lied about the size of its lie (both bugs field-caught)
- Field (esp32, PuTTY paste -> uart -> console): a WALL of identical
  [lost 48958 B], banner at 630 MB for a 25 KB paste, console drop 49072
  (~2x offered). Two distinct bugs, one capture:
- BUG 1, the false refusal: cons_drain_ judged ws_send_ by out_.size()
  equality -- but a synchronous flush (fast local client) returns the
  buffer to its prior size, so SUCCESS read as refusal: the same gap
  re-confessed every pass, the read cursor frozen, the ring saturated,
  everything after overflowed. FIX: ws_send_ now RETURNS the verdict
  (bool: encoded / valve-dropped; 5 drop paths -> false); the drain
  consumes the debt and advances records on true only. Piege 56.
- BUG 2, the twin producers: the pre-built ship had its OWN keel
  (cons_record_, wired in-loop) that the post-compaction grep missed --
  my forged cons_ring_write_ (post-loop) offered every byte a SECOND
  time: drop ~= 2x(offered-stored), the field number to the byte. FIX:
  the original twin survives (adjacent to the drain, cr_free_ helper);
  mine is removed, decl and all. Packaged invariant: ONE ring writer.
- Mirror extended (construth drain): refusal keeps debt+records; success
  confesses ONCE; no debt -> no confession; episodes distinct, never
  cumulative. All 8 suites green, 174 UI asserts.

## CONSRAW -- the truthful vlan terminal (route B): the keel installed
- Discovery: a pre-built ship in the hold -- frontend COMPLETE (0x03
  dispatch, vlanFeed + 0xFF '[lost N B]' marker + vlanLost, rawTruth
  banner, full txRcpt receipt wired into Send AND transfers, source
  palette 100+src) and backend drain cons_drain_ with valve backpressure
  ('valve said no: record stays') -- but NOBODY WROTE THE RING and the
  gap record was never emitted. The pipeline drained an empty sea.
- Keel installed: cons_ring_write_ (records [src][len<=64][bytes], WHOLE-
  record atomicity, overflow -> cons_gap_ + the console PORT's drop line),
  called at the delivery site on cons_hit UNCONDITIONALLY (the raw stream
  has no tap to duplicate -- the log's dedup rule stays the log's);
  in-ring confession [0xFF][4][LE32] written FIRST among new records
  (mirror-faithful), drain-head emitter kept as the no-follow-up fallback.
- The mirror itself had a LATENT INDEX BUG: its assertion looked for the
  confession at the READ cursor; it lands at the WRITE head -- first among
  the NEW records, exactly where the hole happened. Semantics were right,
  the index was wrong; fixed with a write-head anchor (wpre).
- Contract live: G1 delivery receipt, G2 inline confessed loss + port-line
  count, G3 truth banner. Sovereign valves untouched; transit is lossless
  by backpressure; loss exists only at ring overflow and always confesses.
  174 UI asserts, all 8 suites green. Field counter-proof pending: type ->
  receipt; storm -> [lost N] + banner + console drop; transfer -> verdict.

## VIEWSDONE -- the panel closes its book (V1+V2+V3, frontend pure)
- V1: vwModel now applies the GENERAL law D = swr / Sigma-I across ALL
  vlans (field-validated to the byte); the dominant vlan is kept for the
  M / link / bufcap display; the badge suffix upgrades from 'multi-vlan:
  approx' to 'multi-vlan exact (Sigma I=N)'. Single-vlan behavior byte-
  identical (sumI==I) -- all historical asserts untouched.
- V2: a cost line in LIVE MODEL -- 'cost X cyc/B on the engine core
  (M MHz) -- service D/D_pred R'. The esp32 duty-broken frames (gap 77-
  92%) now carry their explanation as a number (service 0.08-0.23).
- V3: the ENDURANCE card -- uptime, throttle/ws-drops/tap-drops PER HOUR
  (health reads in rates, not lifetime counters), the black-box line
  RETAINED from the sys stream ('clean boot' otherwise), and the largest/
  frag vigil institutionalized.
- 168 UI asserts, all 8 suites green, zero backend bytes: the stability
  certificate (700 s field) remains valid while the paint dried.

## AMBERFIRST -- the discriminant played out; the third tick color field-captured
- The 'flat governor' report resolved: console was DOWN (captain's own
  catch) -- tap hidden, WS silent, nothing to govern. Console UP + the
  esp32 M=12 monster -> the chart dove ON CAMERA: plateau 2048 -> cliff
  to floor -> recovery at 928, throttle 2 (last: duty), engine 96%.
- MILESTONE: first AMBER (duty-starved) collapse ever captured. The
  cause-color channel is now fully field-verified: red/backlog (many),
  blue/heap (radio-save session), amber/duty (this frame). Governor,
  feed and renderer all exonerated; GOVRAILS legible in the wild.
- Model badge again honestly naming 'budget-broken serving' (gap 77%) at
  the duty-bound regime -- V2's service-ratio line is the confirmed next
  step per the captain.

## GOVRAILS -- the esp32 plateau perception fix + the 16-port field triumph
- Field (esp32, 16 ports, 4096 B buffers, M=15 storm, uptime 700 s+):
  the AIMD chart LOOKED glitched -- quota pinned at 2048 all window
  (throttle 0, esp32 never collapses here) drew a thin line near the top
  over an unlabeled void. The scale was ALREADY fixed-domain and correct;
  the bug was perceptual. Fix: labeled dashed rails at 2048 and 64 --
  the plateau now reads as 'pinned at max, healthy'. Scale extracted to
  vwGovY, mirrored in tests (2048->12 px, 64->136 px). 161 UI asserts.
- Same captures, for the record: FIRST 16-node PGINFO topology rendered
  (b3..b15 ring), funnel/causes coherent, model badge honestly reporting
  'breach: budget-broken serving' at gap 92% -- engine 81-92%, loop 60-68
  ms, the esp32 duty-broken regime that V2's service-ratio line will
  contextualize. Captain confirms next step: graph finalization V1-V3.

## ESP32FIX -- WSER_COH_FLOOR anchored inside the 8266 ifdef (Piege 48, self-inflicted)
- Field: esp32 build failed -- the cohabitation floor was inserted next to
  the 8266 caps line, INSIDE '#ifdef USE_ESP8266'. The esp32 branch never
  saw it. Relocated after the #endif, universal 6144 (effectively inert on
  esp32's heap; the courtesy is 8266-scale by design).
- Honest gap noted: the host suites do not COMPILE the cpp -- platform-
  scoped declarations are invisible to them; the field pio build is the
  scope reviewer. Piege 48 bites its own author.

## FIELD-STABLE -- the closure captures (uptime 382 s in the killer config + full storm)
- Captain's bench: page attached, uart UP (4.77M tx), console, udp :2324,
  a trunked bridge pair AND three hairpins in loopback storm, 1024 B
  buffers, Switched 11 451 B/s sustained. Uptime 382 s = 6-14 lifetimes
  of the old beast. NO crash, NO black-box line.
- Every subsystem proved in one frame: info flowing (updated 1 s -- the
  pg fix), model gap 1% with the bottleneck correctly naming the pinned
  quota (64 B), conservation EXACT at a new regime (91 424 = 8 x 11 428;
  eviction 79 996 = 7 x 11 428), governor chart showing full arcs
  (floor crawl -> 2048 recovery -> re-collapse with red backlog ticks),
  WS drops 21 557 = the cohabitation valve + soft cap speaking as
  designed (display expendable, uptime not), heap 18.4k/largest 13.0k/
  frag 30% stable under 4.3k of port buffers.
- Watch notes: mxpass 67 ms one-off (page-serve mid-storm; the flush
  belt bounds it), largest plateau in long soak still the standing vigil.
- Remaining for the FULL certificate: the overnight soak in THIS exact
  attached configuration (the old soaks ran detached -- that was the
  immunity). Then Views V1-V3 to close the panel.

## INFOSKEL -- the unbraced-if orphan + the silent swallower (post-STABLE)
- Field on STABLE: NO CRASH (the collision fix holds!) but skeleton panel.
  Root 1: the ALLOCQUIET transform split the one-expression 'if (paged)
  j += pg-fields;' into THREE statements -- only the first stayed under
  the if. On the 8266 (never paged) the ',\"pg\":' literal was skipped but
  app_n(0) and ',\"pgs\":2' always ran: '...udpok\":true0,...' -- invalid
  JSON at char 508, found by a skeleton reconstructor (literals verbatim,
  0 for numbers) piped into json.loads. Fix: braces. Both branches now
  validate.
- Root 2 (why it was INVISIBLE): the page's onmessage parse had a SILENT
  catch -- malformed frames vanished without console or log. Armor: the
  catch now logs 'BAD FRAME from hub: <err> -- <first 80 chars>'. A bad
  frame can never again masquerade as a dead hub.
- Build-hygiene note (third occurrence today): a heredoc assert died
  mid-script TWICE leaving edits unwritten while looking applied; caught
  each time by the failing test / validator before packaging. The suites
  keep earning their keep.
- 159 UI asserts, all suites green, JSON skeleton validated on both
  platform branches.

## STABLE -- ALLOCQUIET + the captain's cohabitation valve (the collision closed)
- Diagnosis sealed by three experiments + two decoded HWDT stacks: SYS-
  context lwip->umm_malloc colliding with our CONT-side allocator use;
  bare device stable, any TCP/WS client of ours -> wedge in 27-65 s.
- A. ALLOCQUIET: send_info_ + bb rebuilt on ONE reserved buffer
  (info_buf_, 3456 B at setup) with app_n (stack-scratch digits) -- 53
  std::to_string temporaries -> 0; two residual std::string wrappers
  stripped; ws_send_text_(const char*) overload (no temp); out_ reserved
  to WSER_OUT_HARD+512 (vector never reallocates post-setup). Main-
  context allocator occupancy per push: ~50 ops -> ~0.
- B. Cohabitation valve (captain design, hard default): WSER_COH_FLOOR
  6144 (tiers ABOVE the 5120 radio brake -- earlier, gentler). Under
  largest < floor: ONE lwip submission per pass (display frames drop as
  designed, ws_drop counts them; control frames info/bb exempt via
  coh_exempt_), info cadence stretched 2 s -> 3.5 s (age stamp
  discloses). pass_subs_ reset each pass at crumb 0x01.
- C absorbed by A: with a zero-alloc build there is no alloc window left
  to stagger; submission spacing IS the B cap.
- Mirrors: app_n digits (0/42/-1/4294967295) + floor-above-brake in
  test_guard. All suites green, 158 UI asserts. No valve touched: caps,
  AIMD, guards, radio brake byte-identical.
- Bench protocol for the captain: (1) THE killer scenario -- page open,
  idle, 10 min (used to die in 27-65 s); (2) viewer attached, 10 min;
  (3) the storm tortures. Expected: stable; under distress ws_drop may
  climb (the valve speaking) and info age may read ~3.5 s.

## WEDGEHUNT-2 -- 160 MHz exonerated; the Level1Int/UART-storm fork
- Field: 80 MHz test = 'non c pas mieux'. The wedge is CLOCK-INDEPENDENT:
  timing-race theories die, resource/interrupt-storm theories strengthen.
- Standing evidence: phase 0x34 (inter-pass), Level1Int (CPU drowned at
  int level 1 for 8+ s), ROM PC, our loop healthy to the last instant, no
  DNS user in the yaml (those BT frames = scan noise), happens with or
  without a page, ~27-65 s post-boot, began when the physical bench went
  live (uart UP persisted; the 8 h crash-free soaks ran uart DOWN).
- Prime suspect: UART RX interrupt storm on GPIO13 (level-1 ISR) -- a held
  BREAK, a floating/low RX, or a mis-bauded talker can fire per-symbol
  error interrupts continuously. The component-level ISR runs regardless
  of web_serial's logical up/down.
- Two-fork plan handed to the captain: (F1) enable his own pre-staged
  -DDEBUG_ESP_HWDT build flag -> the next HWDT dumps the REAL wedged-
  context stack instead of a heuristic scan (the definitive Level1Int
  instrument); (F2) zero-flash physical test -- unplug the bench wire
  from GPIO13/15 (or power off the remote talker), reproduce: survival
  = line-condition storm, and the fix moves to bias/termination/baud.
  Plus the correlation question: were the crash-free overnight soaks run
  with the wire physically connected?

## BBSHOTS -- the report fires unconditionally (and the trigger reframed)
- Field: serial-flashed BBRELAY, 'toute plante, pas acces' -- crashes now
  happen WITHOUT any page load. This reframes the beast: the post-connect
  correlation was a mirage (the captain naturally reloads ~15 s after each
  flash); the real trigger is TIME-BASED, ~27-50 s post-boot (crash #5:
  1607 passes = 27 s; yesterday: +48 s). BBRELAY cannot be an aggravator
  pre-connect: its code is gated on a web client and never ran.
- Structural flaw closed: BOTH report channels were gated on the web page
  (the page itself, and the relay AT the page). New: bb_valid_ set at the
  setup snapshot; loop() fires THREE unconditional logger shots at 15/25/
  35 s post-boot ('BLACK BOX replay N/3: phase 0x..'), surviving API
  viewer flaps. No page, no client, no gate -- the phase byte cannot hide.
- flush_agg_ audited: bounded (agg_used_ iterations, early-outs), house-
  style strings -- no wedge inside it; 0x32/0x33 marks will confirm.
- Captain's doc arrived EMPTY on this side -- asked for an inline repaste.
  Procedure: serial-flash this build, open ONLY the OTA log viewer, let it
  sit through one crash cycle, paste the replay line. Watch safe mode
  (10 x ~45 s boots trips a 300 s lockout).

## BBRELAY -- the OTA blind spot closed (and the wedge hunt status)
- Field: the WEDGEHUNT build reproduced the beast (~33 s post-connect,
  same HWDT Level1Int / ROM PC / cont_check) but the captain's OTA paste
  had NO black-box line -- structural: our setup-time ESP_LOGE prints
  BEFORE wifi/API exist, the OTA viewer can never see it. The data is in
  .noinit and bb_push_ already delivered it to the WEB page this session.
- Fix: bb_push_ now MIRRORS the report on the logger (ESP_LOGE 'BLACK BOX
  replay: ...') at first-info time, when the API is up -- both channels
  carry the verdict forever after. esphome's proven formatter, not a raw
  snprintf (Piege 43 respected).
- Signature note for the hunt: Level1Int = the CPU wedged at interrupt
  level / with ints masked for 8+ s (even the SW WDT starved); ROM PC
  0x40000F68 + cont_check = the classic SYS/idle-loop spin. If the phase
  comes back 0x34, the wedge is OUTSIDE web_serial and the suspects
  become framework/lwip/wifi under our connection load.
- Awaiting from the captain: the BLACK BOX sys line from the WEB page log
  (this session already holds it) -- the phase byte is the address.

## WEDGEHUNT -- the HW-WDT beast: probes said heap is INNOCENT; crumbs densified
- Captain's OTA log, decisive: connect probe fired 'free 28680 largest
  27128' -- the heap/starvation theory is DEAD. The real beast: Hardware
  WDT - Level1Int, PC in ROM, BT0 cont_check, ~15-27 s after each WS
  connect, crash-per-connect loop (two boots 40 s apart). This RETRO-
  UNIFIES crash #5: its sane mxpass (221 us) was the FROZEN pass that
  never finished to record itself.
- Phase forensics: both reports say phase 0x00 + evt 2. send_info_ runs in
  the 0x03 zone (pending consumer), so it is exonerated-by-phase; the true
  0x00 territory = the loop TAIL (EMA, the 1-second window: rates, AIMD
  governor, flush_agg_ 1 Hz WS batch sender, scheduler flag) OR outside
  loop() entirely (framework/API -- cont_check in BT supports it; the API
  Noise flapping correlates in both logs).
- This build: crumb DENSIFICATION so the next report names the station --
  0x30 window entry, 0x31 governor done, 0x32/0x33 around flush_agg_,
  0x34 loop truly done (a wedge past 0x34 = OUTSIDE web_serial), 0x20-0x22
  inside send_info_ (gates passed / entering push / push returned). NEW
  IDLE PHASE IS 0x34 (was 0x00). Plus WEDGE BELTS in flush_tx_: <= 64
  outer iterations and <= 8192 B written per call, resume next pass.
- SAFE MODE caution to the captain: boots are 40 s < the 60 s success
  threshold; 10 attempts lock the chip in safe mode 300 s. Space out the
  reproductions. One repro + the BLACK BOX line = the wedge's address.

## STARVEPROBE -- starved-from-birth fix + logger-side ground truth
- Field: 'ca gele' persists on the QOSLANE build, beacon silent. Own bug:
  starved required info_ok_ms_ != 0 -- a boot that NEVER pushed once was
  never 'starved', so the beacon could not fire in the exact field case.
  Fixed: never-pushed counts as starved after 5 s uptime.
- Strategy shift: the probes now speak on the ESPHOME LOGGER (the channel
  proven alive in the captain's OTA capture), WS-independent: (a) throttled
  ESP_LOGW on BOTH block sites with the full numbers (backlog, out size,
  free, largest, which gate); (b) the ws-connect line now carries free +
  largest (boot-time ground truth). Whatever the WS state, the OTA log
  will now NAME the blocker and its numbers.
- Open question to the captain with the next OTA log: the exact freeze
  phenomenology (skeleton forever? values then stop? browser tab dead?).
  All suites green, 158 UI asserts.

## QOSLANE -- the silent starvation band + a control plane inside the valves
- Field (post-STACKDEFER flash): page serves, WS says connected, ZERO info
  ever -- skeleton panel, dashes everywhere; the esphome API dies in a
  reconnect loop (Noise needs kB per connect: the classic LOW-HEAP witness).
- Root: raising the 8266 largest gate 3072 -> 5824 (OOMWINDOW) created a
  SILENT STARVATION BAND: the gate's early return set no info_pending_, so
  a boot whose largest camps in (3072..5824) retries every 2 s and starves
  forever without a word. The scheduler stamps last_info_ BEFORE calling,
  so nothing ever escalated. The captain's question ('pas moyen de rendre
  un QoS en tempete ?') named the missing design.
- Fix, three contracts: (1) NO early return in send_info_ is silent -- all
  re-arm info_pending_; (2) after 3 s of famine the CONTROL plane may use
  the SOFT..HARD gap (send iff backlog + worst frame < HARD-128 -- the lane
  lives INSIDE the valve, mirrored in test_guard: lane backlog <= 668);
  (3) under heap distress a throttled sys DISTRESS BEACON ships the largest
  number every 5 s: the frozen panel confesses and MEASURES.
- The beacon doubles as the outage probe: flash -> either the panel flows
  (transient), or the log prints 'info starved: largest N B' and N names
  the next investigation (why is the heap low on this boot). Open: the heap
  eater itself. All suites green, 158 UI asserts.

# BUILD STATUS -- v0.2 (beta candidate: full audit pass)

Host-verifiable work green: 7 suites (crc16, framing, guard, switch, loopdet,
decoder.js, ui) incl. a jsdom DOM suite (28 checks) that loads the REAL
embedded page, stubs the WebSocket and asserts every UI binding old and new
(run: npm i jsdom && node test_ui.js, feeding the html extracted from the
raw string) (crc16, framing, guard,
switch, loopdet, decoder.js), 0 non-ASCII, braces balanced, embedded page JS passes
`node --check`, full `g++ -std=gnu++20 -fsyntax-only` PASS against ESPHome
2026.7.1 headers (0 errors; `load_settings` guard is target-only, fine on
ESP8266/ESP32), wser_crc16 + ws Sha1/accept_key verified against canonical
Modbus and RFC 6455 vectors. Decls mirrored from web_spi.h, runtime seam
(no graft). Hardware checklist:

## A. Build + basics
| step | ESP32 | ESP8266 |
|---|---|---|
| `esphome run` builds | [ ] | [ ] |
| page loads, WS connects, sparklines tick | [ ] | [ ] |

## B. Loopback (jumper TX->RX)
- [ ] Send "hello" (CR+LF): one TX frame AND one RX frame, fair throttle shows both
- [ ] Hex send 01 03 00 00 00 02 C4 0B -> decoder says Modbus RTU, CRC16 ok
- [ ] LINE 9600 8N1 applies live; loopback still round-trips
- [ ] FRAME 4 - (Modbus preset) splits bursts at 4 ms

## C. TCP232
- [ ] PuTTY raw to <ip>:2323: typing appears as TX frames; loopback echoes back
- [ ] Web console TX visible in PuTTY (the hub dispatches everywhere)
- [ ] Second TCP client replaces the first cleanly

## D. PC serial bridge (Chrome + flag)
- [ ] Connect a USB-serial adapter wired to the ESP pins
- [ ] PC-port typing reaches the wire (seen as TX self frames)
- [ ] Wire RX arrives on the PC port (echo test both ways)

## E. Real protocols (as available)
- [ ] Modbus RTU device polled (tap mode if a modbus component owns reads):
      request+reply BOTH log (direction fairness), CRC ok, exceptions flagged
- [ ] RS485 transceiver: de_pin scoped around TX (scope it), replies received
- [ ] DMX console/fixture RX: frames decoded, channel bars move
- [ ] DMX TX experimental: a simple fixture responds (dimmer level)
- [ ] GPS NMEA: sentences named, checksum ok

## F. Loop + XVLAN detect (passive, vlan-scoped)
- [ ] Same-vlan cross-port loop (TCP client piped into a PC bridge): LOOP badge
      on each ingress port within 3 round trips in ON mode, stream unaffected
- [ ] Capture a TCP client + a PC COM port for 60 s: ZERO bytes present that
      the hub did not legitimately forward (nothing injected, all modes)
- [ ] Deliberate inter-vlan bridge with XVLANDETECT OFF: total silence, VLAN
      map unaffected, and a simultaneous same-vlan loop still gets caught
- [ ] Same bridge, XVLANDETECT ON: XVLAN badge on the bridged-into port; KILL
      puts it DOWN
- [ ] Circular loop across two vlans (two external links): LOOP on the
      re-entry ports, never XVLAN, when bridging is tolerated
- [ ] Hot PORT VLAN change mid-traffic: no badge for at least 3 s after
- [ ] Console vlan: set via the VLAN pill, then type from the console -- the
      pill and the "Console talks on VLAN" selector both HOLD the new vlan
      (regression: the mirror used to stomp the pill back on every console TX)
- [ ] Loopback jumper on the UART (checklist B) never trips anything
- [ ] Modbus request/reply through a TCP port: never flagged (replies differ)
- [ ] EXPECTED MISS (not a bug): a >64 B block looping through the bridge is
      NOT flagged (re-chunked in flight, exact-match by design); the same loop
      with a short chunk (CR+LF) IS flagged within 3 passes

## FA. Audit regressions (v0.2 hardening)
- [ ] TCP232 sustained ingress at 115200: no backlog growth (burst drain, 8 reads/pass)
- [ ] Wire RX at 115200 into a TCP client: CPU headroom OK (staged chunks, not per-byte)
- [ ] Loop through the WIRE path (wire RX piped back via a net port): now detected
      (staged RX chunks are >=2 B, so they fingerprint)
- [ ] Second browser tab connects instantly, first shows "disconnected -- retrying"
      (stale-client replacement; a page load in flight is never evicted)
- [ ] PORT ADD TCP 0 / duplicate port / the web UI port: refused with a message
- [ ] Deliberately conflicting bind: 3 retries over ~15 s then port DOWN + one err
- [ ] PORT RATE 100 then a 512 B burst: passes when idle, drops while saturated,
      drop counter matches the bytes actually lost
- [ ] Bridge COM disconnect then immediate reconnect of the SAME adapter: works
      (reader cancelled before close -- the port really closes now)
- [ ] Hello banner says web_serial, not Web SPI
- [ ] Rate pill per port: shows the live cap (or infinity), click prompts and
      sends PORT RATE; Reset counters button zeroes stats + badges
- [ ] Reload the page mid-session after a DMX preset: Line settings fields
      show the LIVE 250000 8N2 gap 4, not the 8N1 defaults
- [ ] SYSTEM shows Mode owner/tap-only; LOGGING shows the Wire tap hidden
      state row whenever the console is out of the uart's VLAN
- [ ] Port row hover shows the egress buffer size for net ports
- [ ] Change any vlan, reboot the device: the YAML tcp port exists ONCE
      (regression: the startup-config used to re-create it as a duplicate,
      which then bind-conflicted itself DOWN)
- [ ] Old polluted prefs: a saved duplicate of an existing port is skipped
      at restore (self-healing load)
- [ ] VLAN map lists the console exactly once
- [ ] ANY port alone in its VLAN (uart, tcp, udp, bridge, console): grey
      "alone in VLAN N" badge on its row within one info push (~2 s), and the
      VLAN map box shows "isolated"; badge clears as soon as a second UP port
      joins the vlan (add, move, or PORT UP)
- [ ] Console NOT in the uart's VLAN, tcp->uart traffic flowing: NO tap lines
      in the log, a one-time "wire tap hidden" notice appears, Frames/rates
      keep counting; moving the console back to the uart's VLAN restores the
      tap with a "visible again" notice
- [ ] Console + tcp in one VLAN without the uart: bytes typed in the TCP
      client appear in the web log as "-> console . from tcp :2323" lines
      (regression: console egress was counted then silently discarded)
- [ ] Same but WITH the uart in the vlan: each byte appears ONCE (tap only,
      no double render from the console delivery path)
- [ ] Console alone in its VLAN, type + Send: an explicit "TX went nowhere"
      message appears (no more silent void); console + tcp in one VLAN without
      the uart: "nothing hit the wire" note appears, the tcp client still
      receives the bytes
- [ ] Reboot the hub with a COM bridged: the browser drops the stale binding
      with a "hub restarted" message instead of pointing it at a recycled id

## FB2. UDP platform truth
- [ ] ESP32: + UDP full VCOM round-trip BOTH ways (PuTTY sees console TX),
      txerr stays at 0 (regression: misaligned peer sockaddr made every
      sendto fail EINVAL -- ingress worked, egress never)
- [ ] ESP8266: + UDP creates a WORKING port via the wser module -- full
      VCOM UDP round-trip BOTH ways (PuTTY sees console TX), txerr stays 0
- [ ] ESP8266: two UDP ports simultaneously (multi-instance udp_pcb)
- [ ] ESP8266: PORT DEL on a live UDP port then re-ADD on the same number
      (teardown releases the pcb -- no EADDRINUSE ghost)
- [ ] Any platform: a port whose socket creation fails 3x goes DOWN with one
      message (no per-loop retry storm)
- [ ] UDP egress failure observability: with a peer that ICMP-rejects, the
      port row shows a climbing txerr, ONE errno line in the device log per
      failure run, and the socket self-recreates after 5 in a row (then the
      VCOM round-trip recovers if the cause was a latched socket error)

## FB. Multi-port PC bridge
- [ ] Connect 2+ local COM ports (Chrome): each gets its own bridge port and
      slot; bytes demux to the right physical port both ways
- [ ] Disconnect one: its hub port is deleted (PORT DEL), the other unaffected
- [ ] Unplug a USB adapter mid-session: auto-cleanup, no orphaned hub port
- [ ] 5th connect refused cleanly (4 slots max)

## G. Soak
- [ ] Storm survival (regression of the field OOM): sustained loop storm at
      full Detail for 10+ min -- the hub NEVER reboots; WS drops climbs
      (soft cap), worst case the browser reconnects (hard cap); loop time
      stays bounded
- [ ] 24 h soak: MIN free heap drift < 2 KB (the ONLY test that catches
      fragmentation -- LESSONS.md P2); log heap every 10 min
- [ ] 24 h at 115200 sustained: heap flat, flooded badge honest, no reboot

Date/board/notes: ____________________________________________

## SHAPERFIX -- fractional token buckets (ingress + egress)
- Bug: refill `cap*dt/1000` truncated AND advanced the timestamp -- any rate
  below ~1000/pass_period B/s starved to ZERO (`out 10` = dead trunk; field
  capture: tx frozen 250k behind peers, drops 250k ahead, no $ link rate).
- Fix: milli-token accumulators `out_acc`/`rate_acc` in Port (RAM only, NO
  SavedCfg change, magic stays 0x5B). dt>=1000 clamps to ONE bucket (idle,
  overflow, and the 49.7-day millis wrap in one branch).
- Valves untouched: AIMD drain_quota_ still caps the wire drain, guard/soft/
  hard caps and radio brake not touched. Platform-independent (8266+ESP32).
- Proof: tests/host/test_shaper.c (14 asserts) mirrors both laws line-for-
  line (cross-ref comments both sides); the old law demonstrably delivers
  0 B on the regression scenario. UDP bind-backoff aliasing of tok/tok_ms
  verified safe (hands back tok=0/tok_ms=0 -> clean re-arm clears acc).

## VIEWSTAB -- the Views tab (analytics, client-side only)
- Third pane (General/UART/Views): 6 cards -- TOPOLOGY (deterministic vlan
  ring, hairpin self-arcs, wire chords, LOOP/XVLAN rings, click = blast
  radius), GOVERNOR (quota AIMD history + throttle-cause ticks), LIVE MODEL
  (D_meas=swr/I vs D_pred=min(buf,quota)*1e6/loop, gap badge = continuous
  regression test, what-if sliders), FLOW & CONSERVATION (I*M*D, eviction
  (M-1)/M), INTER-ARRIVALS (gap histogram from logs[]), MEMORY MAP (largest
  vs the three floors).
- Zero backend change, zero library (PROGMEM page must work offline): all
  charts are hand-rolled canvas like spark(). Accumulators capped at 150
  samples; panels repaint only while the tab is visible; age stamp discloses
  stale info (WS drops can eat pushes under storm). Page 66.7k -> 80.9k.
- Model math mirrored in tests/host/test_ui.js (cross-ref comments both
  sides). 127 UI asserts. Caught during build: the pane first landed OUTSIDE
  .main (the siblings/parent assertion paid for itself again).

## VIEWSTAB R1-R3 -- field-feedback refinements
- R1: the what-if refreshes from the live bench on every repaint (it was
  frozen at boot defaults -- field capture showed 51 200 vs a 15 kB/s bench).
- R2: INTER-ARRIVALS empty state now says WHY when the uart is DOWN (the
  tap only sees the wire; cons deliveries carry no frame timing).
- R3: MEMORY MAP scale anchored on the session heap max (floor 40960):
  markers stay put while the fill moves, and the anchor (not a constant)
  keeps the gauge honest on ESP32's 160k heap. Scale disclosed in the note.
- Field validation before these: third never-calibrated regime (buf 256,
  quota 1400, 5 hairpins) reconciled at 0.07-0.1 pct; Switched exact.
  132 UI asserts.

## F10FIX -- the Baud field mine defused
- Bug: updInfo wrote the live baud into .placeholder while .value stayed at
  the hardcoded 115200 -- clicking Apply to change PARITY silently sent
  LINE 115200, an involuntary speed change (the worst of the speed-change
  flaw audit, F10).
- Fix: s-baud joins the syncIf family like the other four LINE fields --
  live value, focus-guarded (a field being edited is never stomped; after
  leaving it, it returns to the truth). One line.
- Behaviour change disclosed: an abandoned draft no longer survives
  forever; the field always shows the wire's truth unless actively edited
  -- consistent with gap/delim/data/par/stop. 134 UI asserts.

## F9F6 -- speed-change hardening (frontend, valves untouched)
- F9 (dump vs stale baud): pacing lives on lastInfo.baud, and WS backlog
  can starve the info push -- a baud drop we have not heard of would race
  the wire and evict the transfer. The sender now HOLDS while telemetry is
  older than 3 s (disclosed on the progress row) and, on a live LINE
  change, repaces and logs 'transfer repaced to N baud'.
- F6 (gap blind to baud): the framing gap is fixed ms while a char lasts
  10/baud s. Governance call: the tap's config stays sovereign -- no
  silent backend override; instead an amber disclosure appears next to the
  gap field the moment gap < 2 chars, math included ('gap 10 ms < 2 chars
  (66.7 ms) at 300 baud -- the tap will SPLIT real frames').
- 141 UI asserts; the F9 tests run inside the real dump harness (hold,
  disclosure, resume, live repace).

## BADGE2 -- oscillation-aware model verdict
- Field case: raising the console (M 6->7) hammered the WS path, the AIMD
  went into violent oscillation (throttle 2->41, quota snapshot 160 rising)
  and the badge cried 'breach: bug or duty-broken (esp32)' -- right verdict,
  wrong label: the mechanism is platform-agnostic and the CAUSE was the
  oscillation (mean quota well under the snapshot the law reads).
- Fix: the badge now counts recent throttle ticks (last 12 samples of
  vwHist.ev). Gap >= 10 pct with >= 3 hits -> amber 'governor oscillating
  (N hits): the quota snapshot overstates the mean'; with a quiet governor
  -> red 'breach: bug, or budget-broken serving'. The accounting legs
  (deliveries/eviction identities) stayed exact throughout the field case.
- 143 UI asserts (oscillating vs quiet-governor breach both covered).

## BADGE3 -- direction-aware oscillation wording (field-found)
- First flash of BADGE2 fired correctly (amber, 6 hits, no false breach)
  but the field showed the snapshot can land at EITHER end of the sawtooth:
  taken at the bottom (quota 64, falling) the snapshot UNDERSTATES the mean
  (D_meas 5609 > D_pred 3605), the opposite of the first case. The wording
  now follows the sign of the gap: 'understates'/'overstates'.
- Accounting identities stayed exact through both field cases (deliveries
  7 x I x D and eviction 6 x injection, to the byte). 145 UI asserts.

## VIEWSBE -- the views' backend phase (cold path only, valves untouched)
- mhz field in info: runtime CPU clock. ESP8266 = ESP.getCpuFreqMHz() (80 OR
  160 on the same binary -- the blind spot closed); ESP32 arduino =
  getCpuFrequencyMhz(); ESP32 IDF = 0 (unknown, disclosed; wire esp_clk on
  request). SYSTEM shows a CPU row.
- Tap-drop causes: drop_thr_/drop_heap_/drop_bkl_ split the Dropped
  aggregate at the three gate sites (20 ms throttle, heap guard, WS
  backlog). MONITOR shows the split; Views gains card 7, TAP FUNNEL --
  offered = shown + causes, survival rate, bands. The funnel states the
  doctrine: the wire flow is never touched, only the reporting is.
- Black box -> web: the .noinit crumb report is snapshotted at boot BEFORE
  its reset and pushed ONCE as a sys line to the first web client
  (bb_push_ at both attach sites, snprintf, cold path). The morning-after
  verdict no longer requires a serial console.
- Page 80.9k -> 85.7k. 151 UI asserts. Open question to the captain: is
  the ESP32 built with arduino or esp-idf? (IDF -> mhz shows '-')

## MHZIDF -- runtime CPU clock on ESP32 IDF (both frameworks, one branch)
- Field: the captain's ESP32 tests run on esp-idf -- the CPU row showed the
  disclosed '-' exactly as designed. Now wired: esp_clk_cpu_freq()/1e6 via
  <esp_private/esp_clk.h>, which exists under BOTH arduino-esp32 (idf 4.4
  bundled) and native idf 4/5 -- one USE_ESP32 branch replaces the
  arduino-only split. 8266 path unchanged (ESP.getCpuFreqMHz()).
- PLAN B if a given idf version hides the private header: swap the include
  for the Kconfig macro CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ (compile-time, but
  truthful in esphome which fixes the clock). Tell me the compile error and
  I switch.
- Field haul from the ESP32 captures: the badge's RED branch validated
  (quiet governor, gap 60 pct -> 'budget-broken serving' -- and the card now
  MEASURES the esp32 service ratio: D_meas/D_pred = 0.40); the R3 gauge
  anchored at 0..187k on the big heap (the exact reason for session-max);
  the funnel reads thr-only, heap 0, bkl 0. 152 UI asserts.

## BBHOTFIX -- black-box delivery corrupted the served page (field-found)
- Mechanism: one of the two client-attach sites is the plain-GET PAGE-SERVE
  path; bb_push_ ran after out_.assign(HTTP headers) but BEFORE
  serving_page_=true, so ws_send_'s guard let a WS frame slip between the
  headers and the HTML. Symptom: the report in clear at the top of the page
  AND the page truncated by the frame's length (Content-Length was for the
  page alone) -> dead app, dashes everywhere.
- Fix: (1) both attach-site calls removed; (2) delivery moved to the top of
  send_info_ -- info only flows on a healthy, fully-upgraded WS; (3)
  bb_push_ now also guards serving_page_ (belt and braces).
- Second field find, pre-existing: on a VIRGIN flash the .noinit crumb is
  random and the mx/evt fields were never initialized on magic mismatch --
  the first real crash then reports garbage (mxpass '18 minutes'). The
  mismatch path now zeroes phase/mx*/evt.
- The crash itself (previous boot: evt 2 = info-push checkpoint, pass 3152,
  ~54 s after flash, phase 0x00 = outside instrumented sub-phases) is REAL
  and unexplained; its mx numbers were garbage so no verdict. If it recurs
  on the fixed build, the next report carries CLEAN maxima. Watch item.
- Evt map for reading reports: 1 = page served, 2 = info push, 3 = page
  serve start. 152 UI asserts, all suites green.

## VWFLOOD -- valve audit + Views feed-path coalescing (field report: flood freeze)
- Captain's report: 8266, +1 bridge then flood -> the AIMD chart does not
  dive as before and the interface freezes. VALVE AUDIT FIRST, byte-checked:
  WSER_OUT_SOFT/HARD 2048/4096 (8266) and 8192/16384 (esp32) intact; AIMD
  law intact (collapse /2 floor 64 at l.3528, growth +=32 cap 2048); heap
  guard 1536 intact (l.1714); radio brake intact (l.2064). The recent
  backend insertions (bb at send_info_ head, crumb hygiene) are clean and
  cold-path. THE VALVES WERE NOT TOUCHED.
- The one structural NEW load-path work since VIEWSTAB: with the Views tab
  open, EVERY info repainted all 7 panels. Fixed: the feed path is now
  coalesced to <= 4 Hz (vwKick, 250 ms window + trailing repaint); tab
  switches still render immediately. Both windows unit-tested.
- Build note: the first edit attempt had a python SyntaxError and applied
  NOTHING -- caught by the failing coalescer assertion before packaging.
  The tests earn their keep again.
- Open investigation (discriminator protocol handed to the captain): (a)
  reproduce the flood with the General tab active -- if no freeze, the
  frontend render path was the culprit and this fix closes it; (b) flood
  with the page closed 60 s, reopen, check whether throttle jumped (did the
  quota dive unobserved? then the backend is healthy); (c) WHICH bridge:
  the +Bridge button (slot with no COM behind it = every delivery becomes a
  0x02 binary frame to the browser, a firehose) vs Connect Web Serial; and
  is the ESP still answering (ping / serial log) during the 'freeze' --
  ESP dead vs browser dead vs link poisoned is the trichotomy to cut.
  154 UI asserts, all suites green.

## OOMWINDOW -- the flood crash: an arithmetic death window (field crash #5)
- Captain's repro: 8266, +bridge then flood -> the ESP REBOOTS (dynamic
  ports wiped, counters zeroed). First crumb (garbage era) already pointed
  at evt 2 = the info-push checkpoint.
- Root cause found by reading, confirmed by arithmetic: the send_info_
  head gate required largest >= 3072, but three lines later j.reserve()
  asks ONE contiguous block of j.size()+232*16+64 (~4.7 KB). A storm-
  laminated heap with largest inside (3072..~4700) passed the gate and
  died inside reserve -- no exceptions on the 8266, bad alloc = abort =
  hardware reset. The flood + slot-bridge firehose is exactly the churn
  that parks largest in that window; the recent +70 B of info fields
  widened the ask.
- Fix (strengthens a SKIP-guard -- the safe direction, no valve touched):
  head gate raised to 232*WSER_MAX_PORTS+64+2048 = 5824, with the full
  story in the comment; plus a pre-reserve re-check with the exact bill
  (the heap can move during the head build). Skip the push, never crash;
  the age stamp discloses the silence.
- test_guard.c now mirrors the arithmetic (gate >= reserve ask + 1024):
  grow the per-port budget or MAX_PORTS and the suite trips loudly.
- ASK OPEN to the captain: the BLACK BOX line from the crash he just had
  (filter sys in the log) -- clean numbers now; evt 2 expected if this was
  the beast. All 8 suites green, 154 UI asserts.

## PGINFO -- paged ports info (the connect/die loop at 14+ ports)
- Field: captain filled the port table (13 bridges + 3 fixed = 16); page
  entered a connect/die loop ('disconnected -- retrying' forever), and the
  saved ports resurrect the poison on every reboot. Reading ws_send_:
  the caps compare out_ BEFORE queueing, so a 16-port info (~5.4 KB worst
  case) always passes, PARKS out_ above WSER_OUT_HARD (4096), and the NEXT
  send drops the client. The valve is right; the payload was wrong-sized.
- Fix: ports are paged (ids 0-7 / 8-15 on alternating pushes) whenever any
  id >= 8 exists; each page is AUTHORITATIVE for its range (absent id =
  deleted). Frontend merges by id into a stable view (updInfo head).
  Absolute belt: an info frame that would approach HARD is skipped, never
  sent. Unpaged behavior at <= 8 ports is byte-identical (compat).
- Recovery path for the bricked hub: flash this build -- paged infos
  (~2.4-3.2 KB) fit, the page loads, delete the extra bridges.
- Open: the 43 s reboot in the same capture is not yet attributed (was
  OOMWINDOW flashed at that point? next BLACK BOX line will say). 158 UI
  asserts, all suites green.

## ERRATUM + STACKDEFER -- the captain caught a constant error
- Captain: 'c 7 ports max en esp8266' -- correct. WSER_MAX_PORTS is 8 on
  the 8266 (ids 0-7, so 5 dynamic next to uart/console/tcp) and 16 on the
  ESP32. My PGINFO analysis used the ESP32 count against the 8266 caps:
  WRONG. Corrected forensics: an 8266 info (~2.6-3.2 KB worst) never
  breaches HARD alone -- the connect/die loop was INFO STACKING: out_
  garnished just under SOFT by sys lines ('port table full' x8...), the
  gate tests BEFORE queueing, the info lands, out_ parks at ~5.1 KB > HARD,
  the next send trips drop_client_. Persistence resurrects it every boot.
- Real fix (STACKDEFER): an info departs only on a near-empty channel
  (backlog <= 512), else deferred via info_pending_ (its designed purpose)
  and retried next pass. Arithmetic mirrored loudly in test_guard.c:
  512 + worst-8266-info < HARD. PGINFO stays (ESP32 headroom + bigger
  tables) with its comment rewritten honestly; paging never fires on the
  8266 by construction.
- Crash #5 attribution softened accordingly: with MAX=8 the reserve ask is
  ~1.9 KB (old gate covered it); the fatal alloc was more likely the out_
  encode/vector growth or the operator+ churn under lamination -- ALL of
  which the raised 5824 gate now fronts. The fix stands; the named line in
  the earlier story is corrected. All suites green, 158 UI asserts.

## CRASH6 -- the messenger was the serial killer (decoded OTA backtrace)
- Captain delivered the decisive instrument: the esphome OTA log with a
  DECODED trace. Exception - Alignment (exccause=9), PC in __ssputs_r via
  _svfprintf_r = inside a printf-family formatter; BT12-15 show the
  ws::encode_frame -> socket::write chain on the same stack. Timeline:
  client connects 13:16:49, first info, bb_push_ fires (a report was
  pending), snprintf formats the long black-box line -> alignment fault.
- The diabolical loop: every crash re-arms bb_pending_, so boot -> first
  client -> format the report -> die -> re-arm. The black box itself was
  the self-perpetuating killer ('encore plante, meme dans cette page').
- Why 12:55 delivered fine but 13:16 died: LINK-LAYOUT LOTTERY. The long
  literal run (~43 chars) takes __ssputs_r's word-copy path (nano-
  vfprintf.c:182, exactly the PC line); whether that word walk faults
  depends on the literal's placement parity, which changed with the
  13:15:55 rebuild. bs's snprintfs survive on the byte path (short runs)
  -- noted as candidates ONLY if a future trace ever names them.
- Fix: bb_push_ returns to house style -- plain std::string concatenation,
  zero formatter, cold path (the reason the whole file is written that way
  on this platform). Crash #5 (evt 2, clean mx, alloc-window class) remains
  distinct and remains gated at 5824. Two beasts, two mechanisms, both
  down. All suites green, 158 UI asserts.
