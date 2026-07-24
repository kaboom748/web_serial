# web_serial -- hardware truths (distilled from the web_spi/web_i2c memo)

The full memo (PIEGES_MATERIEL_ESP8266_ESP32) is the reference; this file
keeps the subset that BITES THIS COMPONENT, with our compliance state.
Rule zero: the ESP8266 is a cooperative single-core machine with ~40 KB of
free heap and ~4 KB of stack. Anything that blocks, allocates on the hot
path, or grows unbounded kills it. The ESP32 forgives -- not everywhere.

## Enforced in this codebase (audited)
- P2 fragmentation: fixed-size buffers everywhere on the data path
  (rx_stage_, port egress bufs allocated once at PORT ADD, transport
  rings); std::string only for short-lived messages/info. ws_accum_
  reserved at accept (no realloc churn). The ONLY fragmentation test that
  works is the 24 h soak watching min-heap drift -- see BUILD_STATUS G.
- P3 stack: no local buffer > 256 B anywhere (8266 cont stack ~4 KB; a
  deep-path overflow crashes FAR from the culprit). Audited; the one
  512 B offender (WS drain) was cut to 256.
- P5 blocking: zero delay() calls; everything is state machines + the loop.
- P6 boot: nothing heavy in setup() (socket binds only). If a heavy-write
  feature ever lands (file dump, big flash write), gate it behind a boot
  grace like web_spi's B-4.
- P14 micros() wraps at ~71 min ON BOTH CHIPS: every delta is computed in
  unsigned 32-bit -- C++ uint32_t subtraction, JS `(a-b)>>>0`. Both sides
  audited compliant. Never "fix" these into signed math.
- P21 PROGMEM: the 37 KB page lives in flash (PROGMEM) and is served in
  512 B slices via progmem_memcpy. NEVER load it whole into RAM (it is
  ~75% of the 8266 heap) and never byte-loop over PROGMEM directly
  (LoadStoreError). ASCII-only literal, checked every build.
- API splits (heap fns, socket impls) behind platform guards, in ONE
  place each; both ifdef branches compiled every build (fake-define pass).

## Standing rules for FUTURE work on this component
- ESP-NOW (planned): recv callbacks run outside the loop context -- only
  ever fill a ring there (the rx_stage_ pattern), never touch the switch.
  No IRAM_ATTR needed unless a true ISR appears; if one does, remember
  8266 IRAM is nearly FULL (link error awaits).
- GPIO features (DTR/RTS, breakout box -- planned): flash pins 6-11
  forbidden on both; 8266 strapping 0/2/15; ESP32 strapping 0/2/5/12/15
  with GPIO12 (VDD_SDIO!) the brick-capable one; ESP32 34-39 input-only
  (refuse as outputs); GPIO16 (8266) via digitalRead only, never the raw
  GPIO_IN register. Released pins on WS disconnect -- the tool never
  leaves hardware in a state a closed tab no longer documents.
- Reads that "work sometimes" are a speed or wiring problem, not luck:
  always show raw next to decoded, never present a plausible value
  without its status (the web_i2c golden rule).
- struct changes (SavedCfg, persisted types) = clean build (ABI).


## Session 3 additions (2026-07-23) -- three field crashes distilled

- A crash MESSENGER must be the most paranoid code in the file: it runs at
  the most fragile moment and re-arms itself on failure. House-style string
  building only; printf-family long formats are a link-layout lottery on the
  8266 (Alignment exccause=9, unpatched by non32xfer).
- An OOM guard protects nothing unless it covers the LARGEST CONTIGUOUS ask
  of the guarded block (reserve, vector doubling, temporaries churn) -- and
  the arithmetic belongs IN the test suite, mirrored loudly.
- The WS caps test BEFORE queueing: frames that scale with config must only
  depart on a near-empty channel, or stacking parks the canal over HARD and
  persistence resurrects the wall on every boot.
- A channel that ACCEPTS a send is not a READY channel. Flag guards have race
  windows; one-shot deliveries go where success has a history (send_info_),
  never at the topologically earliest attach.
- Virgin .noinit is pure noise: zero every field a report will ever print, or
  the first real crash publishes credible garbage and burns the instrument.
- Per-platform constants (MAX_PORTS 8/16, caps) invalidate any forensic math
  done with the other chip's numbers. Grep the ifdef before computing.


## Session 4 additions -- the allocator war

- The SYS/CONT allocator collision needs no external partner: your own TCP
  traffic supplies both sides. The fix is not throttling CPU (nobody was
  starved -- mxpass 198 us to the last instant) but ELIMINATING main-context
  allocator occupancy (reserved buffer + digit appender) and SPACING lwip
  submissions under a memory floor.
- A colored-cause channel turns YAML toggles into readable signatures:
  modem-sleep announced itself as blue governor ticks before anyone opened
  the config.
- Brace every if before mechanically splitting expressions; validate
  generated JSON by skeleton reconstruction through a real parser, per
  platform branch.
- A silent parse catch converts protocol corruption into 'dead hub' -- the
  most expensive misdirection of the day. Failures must speak, with bytes.
- Scripted edits die half-way looking successful; the write goes LAST after
  ALL asserts, and truth is the re-passing suite, never the printed log.
