# web_serial — Guide des commandes & schéma de câblage frontend ↔ backend

Inventaire exhaustif établi par lecture du code (`web_serial.cpp`, page HTML/JS
embarquée incluse). Toute ligne citée renvoie au fichier courant.

---

## 1. Comment une commande voyage

```
navigateur                          ESP
──────────                          ───
send('PORT UP 3')  ──WS OP_TEXT──▶  handle_ws_frame_()  (l.1054)
                                      └─▶ handle_command_(line)  (l.2037)
                                            └─▶ réponse WS : {"t":"ok"|"err","msg":…}
                                            └─▶ info_pending_ = true  (push d'état)

0x01 <slot> <data> ──WS OP_BIN───▶  handle_ws_frame_()  (l.1048)
                                      └─▶ switch_ingress_(bridge)   ← DONNÉES, pas une commande
```

**Deux canaux distincts** : le texte = plan de contrôle (commandes) ; le binaire
`0x01` = plan de données (RX du COM local du navigateur). Le sens descendant
`0x02 <slot> <data>` est l'egress vers le COM.

**Pré-traitement à l'entrée** : `handle_command_` ne fait que sauter les blancs
de tête (`skip_to_token`). **Aucune conversion de casse, aucun trim de fin.**

> ⚠️ **Les verbes sont SENSIBLES À LA CASSE.** Dans la console `cmd >`,
> `port del 3` → `unknown command`. Il faut `PORT DEL 3`.
> (Seuls certains *arguments* sont insensibles : `ON|OFF|KILL` et le mode `TAP`.)

---

## 2. Référence complète des commandes backend

Ordre de dispatch réel dans `handle_command_` (le **premier** match gagne).

### 2.1 Données / ligne série

| Commande | Syntaxe | Validation | Réponse | Push info |
|---|---|---|---|---|
| **TX** | `TX <hex> [hex…]` | garde `p[2]` = espace/fin ; **max 128 octets** (excédent ignoré en silence) ; 0 octet → err | `err` si vide ; `err` si console seule dans son VLAN ; `ok` si aucun uart dans le VLAN | non |
| **DMXTX** | `DMXTX <hex> [hex…]` | **max 64 canaux** (excédent ignoré) ; 0 → err | `ok` (timing break expérimental) | non |
| **LINE** | `LINE <baud> [<D><P><S>]` | baud **300…2 000 000** sinon err ; `D`=7 sinon 8 ; `P`=E/O sinon N ; `S`=2 sinon 1 | `ok "line: …"` | **oui** |
| **FRAME** | `FRAME <gap_ms> [<delim hex>\|-]` | gap **clampé 1…1000** ; `-` ou absent → delim **remis à 0** | `ok "framing: …"` | **non** ⚠ |

`LINE` applique `set_baud_rate` + `set_data_bits` + `set_parity` +
`set_stop_bits` + `load_settings(false)` — **sans garde de plateforme**
(fonctionne sur ESP8266 comme sur ESP32).

### 2.2 Ports (le switch)

**Toutes** les sous-commandes `PORT …` terminent par `save_config_()` **+**
`info_pending_ = true` (l.2348-2349, après la chaîne `else if`).

| Sous-commande | Syntaxe | Comportement / validation |
|---|---|---|
| **ADD TCP** | `PORT ADD TCP <port> [buf]` | port 1-65535, ≠ port UI, pas de doublon (même n° + même type) ; `buf` par défaut `WSER_PORT_BUF_DEF` |
| **ADD UDP** | `PORT ADD UDP <port> [buf]` | idem ; ESP8266 → module `wser_udp` (raw lwip) |
| **ADD BRIDGE** | `PORT ADD BRIDGE` | bridge **à slot** (flux Connect) : `buf_cap = 0`, max 4 slots, réponse `t:"bridge"` |
| **ADD BRIDGE TRUNK** | `PORT ADD BRIDGE TRUNK [buf]` | bridge **sans slot** (null-modem RAM) ; `buf` honoré |
| **DEL** | `PORT DEL <id>` | refuse les ports fixes (uart/console) ; libère le buffer ; réponses `ok` + `t:"pdel"` |
| **VLAN** | `PORT VLAN <id> <1-8>` | changement = **purge** de l'état de détection (ring, streaks, badges) |
| **UP / DOWN** | `PORT UP\|DOWN <id>` | `UP` efface les badges LOOP/XVLAN |
| **RATE** | `PORT RATE <id> <o/s>` | storm control **ingress** ; 0 = illimité ; seau fractionnaire exact (les petits débits sont honorés) |
| **ORATE** | `PORT ORATE <id> <o/s>` | shaper **egress** ; 0 = illimité ; ré-arme le seau ; seau fractionnaire exact (`out 10` = 10 o/s réels, plus jamais un fil mort) |
| **ADOPT** | `PORT ADOPT <id>` | ré-attache un COM ouvert à un bridge sans slot |
| **WIRE** | `PORT WIRE <a> <b>` / `PORT WIRE <a> -1` | trunk ; `b == a` = **hairpin/LOOPBACK** ; `-1` débranche (le `-` est bien géré) ; refus si extrémité déjà câblée / liée à un slot / non-bridge ; **retrofit d'un buffer 256 o** si absent |
| **RESET** | `PORT RESET` | remet à zéro tx/rx/drop/txerr + badges + `ws_drop_` |

### 2.3 Détection & garde-fous

| Commande | Syntaxe | Effet | Persisté | Push info |
|---|---|---|---|---|
| **LOOPDETECT** | `LOOPDETECT ON\|OFF\|KILL` | 1 / 0 / 2 ; `OFF` efface les badges (et les rings si XVLAN off aussi) | **oui** | oui |
| **XVLANDETECT** | `XVLANDETECT ON\|OFF\|KILL` | idem, verdict inter-VLAN | **oui** | oui |
| **FLOOR** | `FLOOR <octets>` | plancher du garde-mémoire ; **min 1024** | **non** ⚠ | oui |
| **RADIOFLOOR** | `RADIOFLOOR <n>` | plancher du frein radio ; `0` = OFF + avertissement | **non** (voulu) | oui |
| **WALK** | `WALK [ON\|OFF]` | levier d'ablation du parcours de tas ; `OFF` → `largest_cache_ = 0` | non | non |

### 2.4 Journal / observation

| Commande | Syntaxe | Effet |
|---|---|---|
| **TAP** | `TAP FULL\|SUMMARY\|LIVE\|BATCH\|OFF [dir]` | mode d'affichage du tap ; `dir` = 0 (TX) / 1 (RX) / absent = tous ; **`TAP` nu = FULL, tous** |
| **ARM** | `ARM [0\|1\|*] [octet hex]` | déclencheur ; sans argument = armé sur tout |

### 2.5 Système

| Commande | Effet |
|---|---|
| **REBOOT** | ack WS, puis `set_timeout(200 ms)` → `App.safe_reboot()` (laisse la file WS partir) |
| **BRIDGE** | alias historique → **rappelle** `handle_command_("PORT ADD BRIDGE")` |
| *(inconnu)* | `{"t":"err","msg":"unknown command"}` |

---

## 3. Câblage frontend → backend (contrôle par contrôle)

### 3.1 Contrôles câblés (tous vérifiés vers une commande existante)

| Contrôle UI | id | Commande émise |
|---|---|---|
| Send (console) | `tx-send` (+ Entrée) | `TX <hex>` (texte→hex + EOL cr/lf/crlf, ou hex brut) |
| TX frame (DMX) | `dmx-send` | `DMXTX <hex…>` |
| Apply (ligne) | `s-apply` | `LINE <s-baud.value\|\|115200> <D><P><S>` |
| DMX preset | `s-dmx` | remplit 250000/8/N/2 + gap 4 → appelle `s-apply` **et** `s-fapply` |
| Modbus preset | `s-mb` | met gap 4 → appelle `s-fapply` |
| Set (framing) | `s-fapply` | `FRAME <gap\|\|10> <delim\|->` |
| Reset counters | `port-reset` | `PORT RESET` |
| + TCP | `add-tcp` | `PORT ADD TCP <prompt> <new-buf>` |
| + UDP | `add-udp` | `PORT ADD UDP <prompt> <new-buf>` |
| + Bridge | `add-bridge` | `PORT ADD BRIDGE TRUNK <new-buf>` |
| pill VLAN | `p-vlan` | `PORT VLAN <id> <(vlan%8)+1>` (cycle 1→8) |
| pill UP/DOWN | `p-updn` | `PORT UP\|DOWN <id>` |
| pill rate | `p-rate` | `PORT RATE <id> <prompt>` (regex `^\d+$`) |
| pill out | `p-orate` | `PORT ORATE <id> <prompt>` (regex `^\d+$`) |
| pill wire | `p-wire` | câblé → confirm → `PORT WIRE <id> -1` ; sinon prompt → `PORT WIRE <id> <n>` |
| × (supprimer) | `p-del` | confirm → `PORT DEL <id>` |
| champ Floor | `floor` | `FLOOR <v>` **uniquement si v ≥ 1024** |
| Loop detect | `ld-off/on/kill` | `LOOPDETECT OFF\|ON\|KILL` |
| XVLAN detect | `xv-off/on/kill` | `XVLANDETECT OFF\|ON\|KILL` |
| Connect a local port | (flux PC) | `PORT ADD BRIDGE` (nu → slot + `t:"bridge"`) |
| déconnexion / débranchage COM | (flux PC) | `PORT DEL <pi>` |
| Re-link | (flux PC) | `PORT ADOPT <prompt>` |
| Console talks on VLAN | `cvlan` | `CVLAN <v>` |
| Detail Full / Summary | `tap-full` / `tap-sum` | `TAP FULL\|SUMMARY <tap-filter>` |
| Delivery Live/Batch/Off | `tap-live/batch/off` | `TAP LIVE\|BATCH\|OFF` |
| Arm | `arm-btn` | `ARM <0\|1\|*> <octet>` |
| Reboot hub | `sys-reboot` | confirm → `REBOOT` |
| console `cmd >` | `cmd-go` / Entrée | **passe-plat brut** (n'importe quelle commande) |

**Résultat : aucun bouton mort.** Chaque contrôle pointe vers une commande que
le backend implémente réellement.

### 3.2 Contrôles purement client (aucune commande)

`log-pause`, `log-filter`, `log-clear`, `log-csv` (export CSV), `dec-btn`,
`dec-clear`, `dec-filter`, `new-buf` (aperçu « fits » seulement — consommé par
les boutons `+ TCP/UDP/Bridge`), `tap-filter` (consommé par `tap-full/sum`).

### 3.3 Commandes backend SANS contrôle UI (console `cmd >` uniquement)

| Commande | Pourquoi |
|---|---|
| `WALK [ON\|OFF]` | levier d'ablation de diagnostic |
| `RADIOFLOOR <n>` | réglage expérimental (suggéré dans le placeholder de la console) |
| `BRIDGE` | alias historique, remplacé par le flux Connect |

---

## 4. Ce qui survit à un reboot

**Persisté** (`SavedCfg`, magic `0x5B`) : `console_vlan`, `loopdet`,
`xvlandet`, et pour chaque **port réseau dynamique** : type, vlan, up,
net_port, buf_cap, rate, orate, wire.

**NON persisté** : réglages de ligne (`LINE`), framing (`FRAME`), mode `TAP`,
`ARM`, `WALK`, `RADIOFLOOR` (volontaire), `FLOOR`, et **les ports
BRIDGE/TRUNK eux-mêmes** (l.2654 saute `PT_BRIDGE`).

> Conséquence : le champ `SavedPort.wire` et la passe de validation de trunk au
> boot existent mais **ne servent jamais**, puisqu'aucun bridge n'est
> sauvegardé. Écart avec le README qui annonce « TRUNK … Persisted ».

---

## 5. Anomalies & points de vigilance relevés

| # | Constat | Portée |
|---|---|---|
| 1 | **`s-baud` écrit dans `.placeholder` au lieu de `.value`** (l.507) alors que les 5 autres champs de ligne utilisent `syncIf`→`.value`. Comme l'input a `value="115200"` en dur, le placeholder ne s'affiche jamais : le champ **ment en permanence** et sert pourtant de **source** à `LINE`. | bug frontend |
| 2 | **Sous-commande `PORT` inconnue** (`PORT FOO`) : aucune erreur, mais **écriture flash + push info** quand même. Le fallback de haut niveau, lui, répond « unknown command ». | incohérence |
| 3 | **Échecs silencieux** sur arguments invalides : `PORT VLAN/UP/DOWN/RATE/ORATE/DEL/WIRE` sortent **sans message**. (Le frontend filtre par regex, donc surtout atteignable depuis la console brute.) Contraste avec la doctrine maison « un refus silencieux est une dette de debug ». | incohérence |
| 4 | **`save_config_()` à chaque commande `PORT`** — y compris `PORT RESET` qui ne change rien de persistant. Une écriture flash par clic de pill. | usure flash |
| 5 | **`FRAME` ne demande pas de push info** (contrairement à `LINE`) : les champs gap/delim ne se resynchronisent qu'au push périodique (≤ 2 s). | latence UI |
| 6 | **`TX` tronque à 128 o, `DMXTX` à 64 canaux**, en silence. Aucun avertissement côté page. | silencieux |
| 7 | **`BRIDGE` (alias) rappelle `handle_command_`** → une trame de pile supplémentaire sur les 4 Ko de la pile `cont`. Unique chemin récursif du parseur. | doctrine STACKFIX |
| 8 | **Verbes sensibles à la casse** dans la console `cmd >`, sans message d'aide. | ergonomie |
| 9 | **Aucune authentification ni limitation de débit** sur le plan de contrôle (assumé : outil de LAN, cf. README). | assumé |

---

## 6. Rappel : rafraîchissement de l'état

- **`info`** (état complet des ports) : poussé **à chaque changement**
  (`info_pending_`) **et** toutes les **2 s** (l.3036).
- **`bs`** (barres de remplissage) : poussé à **4 Hz** (250 ms, l.2528).
- Les deux passent par l'egress WS : sous tempête, le **cap doux les jette**
  (`ws_drop_`) — c'est une soupape de survie, pas un défaut. Un bouton peut
  donc sembler « ne pas répondre » le temps qu'un push passe.


---

## Comportements recents (2026-07-23, a connaitre pour operer)

- **Champ Baud (onglet UART)** : il affiche desormais la VALEUR live du fil
  (plus un placeholder). Garde de focus : un champ en cours d'edition n'est
  jamais ecrase ; des qu'il perd le focus, il revient a la verite du prochain
  push info. Consequence : un brouillon abandonne ne survit plus -- Apply
  envoie toujours ce que tu VOIS. (Ex-mine F10 : Apply pour changer la parite
  envoyait un LINE 115200 involontaire.)
- **Avertissement de cadrage (F6)** : si `gap < 2 caracteres` au baud courant,
  un texte ambre apparait a cote de Frame gap avec le calcul (ex. `gap 10 ms
  < 2 chars (66.7 ms) at 300 baud -- the tap will SPLIT real frames`). Le
  backend n'est PAS modifie : la consigne du tap reste souveraine, le piege
  est divulgue.
- **Transferts (upload/coller dans RAW)** : le cadenceur suit le baud live ;
  sur changement de LINE en plein transfert, le journal note `transfer
  repaced to N baud`. Si la telemetrie a plus de 3 s (backlog WS), le
  transfert SE MET EN ATTENTE (`telemetry stale -- holding` sur la barre)
  plutot que de sur-cadencer et corrompre.
- **Ligne `BLACK BOX (previous boot crashed): ...`** : livree UNE fois au
  premier client apres un crash (via le flux info, jamais pendant le service
  de page). Grille des `evt` : 1 = page servie, 2 = push info, 3 = debut de
  service de page. `phase 0x00` = hors sous-phases instrumentees. Les maxima
  (mxpass/mxwin/mxwalk) sont en microsecondes et desormais toujours propres
  (crumb initialise sur flash vierge).
- **`drop causes` (panneau MONITOR)** : ventilation de Dropped -- `thr`
  (throttle 20 ms) / `heap` (garde-tas) / `bkl` (backlog WS). L'onglet Views
  (carte TAP FUNNEL) en fait l'entonnoir avec taux de survie.
- **Info paginee (ESP32, 9+ ports)** : le hub envoie les ids 0-7 et 8-15 en
  alternance (`"pg":0/1`) ; la page fusionne. Invisible a l'usage ; a savoir
  si on lit le WS a la main. Le 8266 (table de 8) ne pagine jamais.
- **Onglet Views** : 7 cartes calculees cote client (topologie cliquable,
  chronogramme du gouverneur, modele predit-vs-mesure avec badge a 3 verdicts,
  conservation, inter-arrivees, carte memoire a echelle ancree, entonnoir).
  Rafraichissement coalesce a 4 Hz sous flood ; l'horodatage `updated Ns ago`
  divulgue toute telemetrie muette.


## Lecture des instruments (ajouts session 4)

- **Tics du GOVERNOR** : chaque tic = une fenetre d'1 s ou le quota s'est
  effondre (/2). Rouge = backlog WS ; ambre = famine de duty ; **bleu =
  largest < 7 680 (1,5 x frein radio)** -- signature typique du modem-sleep
  WiFi (voir CONFIG_ESP8266_TERRAIN.md). Sante : couches externes actives,
  `drop causes heap = 0`.
- **`WS drops` qui grimpe sous detresse memoire** : la soupape de
  cohabitation (plancher 6 144) limite a UNE soumission lwip par passe --
  l'affichage est sacrifiable, l'uptime non. La cadence info passe a 3,5 s
  et l'horodatage l'avoue.
- **`BAD FRAME from hub: <err> -- <80 octets>`** dans le journal : une trame
  JSON illisible a ete rejetee BRUYAMMENT (plus jamais de panneau squelette
  muet). Ces 80 octets sont la piece a conviction a coller au rapport.
- **`BLACK BOX replay N/3`** cote visionneur OTA : le rapport de crash
  re-emis a 15/25/35 s post-boot, sans prerequis de page (les logs de setup
  sont invisibles a l'OTA -- angle mort structurel ferme).


## Le terminal RAW -- la console qui ne ment pas (route B, contrat complet)

- **Ce qu'il montre** : TOUT ce que le vlan livre au membre console, en
  octets bruts, **une couleur par port source** (namespace 100+id) -- plus
  les flux PC-bridge 0x02 (couleurs slots) et ton echo local (cosmetique).
- **G1, le recu de livraison** : chaque Send et chaque fin de transfert
  imprime dans le terminal `delivered[send] -> u0 +N | b3 +N (drop +M)` --
  les deltas tx/drop de CHAQUE membre du vlan, tires de deux photos info.
  `drop +0` partout = acheminement fidele, hors de tout doute.
- **G2, la perte avouee inline** : si le tampon console deborde (tempete),
  le trou devient un marqueur `[lost N B]` couleur alerte, INSERE dans le
  flux a l'endroit exact du manque -- impossible de lire par-dessus. La
  ligne du port console compte la meme perte dans SWITCH PORTS.
- **G3, le bandeau de verite** : en-tete du panneau, `vlan N raw --
  in-sync` (vert) ou `LOST N B (console buffer)` (rouge), cumule live.
- **Mecanique** : ring 1024 o de records [src][len<=64][octets] ; drain <=4
  records/passe SOUS les soupapes (backpressure : un refus de valve laisse
  le record en place, zero perte en transit -- la perte n'existe qu'au
  debordement du ring, et elle s'avoue). Record de confession en-ring
  [0xFF][4][LE32], premier parmi les nouveaux. Frappe/collage : inchange
  (TX -> ingress console -> vlan, transferts cadences au baud + hold).


## LOGGING -- les trois robinets du plan d'observation

Heritage assume du produit d'origine (renifleur UART), devenus les commandes
MANUELLES du plan d'observation. Ils gouvernent le JOURNAL seul : jamais le
fil (l'entonnoir le jure), jamais les compteurs, jamais le terminal vlan
(0x03 vit sous ses propres soupapes -- Delivery Off et la console-qui-ne-
ment-pas continue de tout dire).

- **Detail** : `Full` = chaque trame tapee devient sa ligne (octets + us) --
  la matiere premiere de SERIAL DECODE et de l'histogramme INTER-ARRIVALS.
  `Summary` = transactions coalescees (`RX 2048B x13`) -- ~10x moins de
  messages WS, motifs lisibles, forensique a l'octet indisponible.
- **Delivery** : `Live` = a la naissance (sous throttle 20 ms + gardes --
  tes `drop causes thr/bkl`). `Batch` = accumulation cote hub, rafales --
  horodatage preserve, BEAUCOUP moins de soumissions/passe : l'ami de
  QOSLANE et de la soupape de cohabitation en tempete. `Off` = zero octet
  de journal sur le WS.
- **Filter** : loupe d'AFFICHAGE cote navigateur (familles de lignes) --
  zero effet fil/compteurs.

Grille : debug protocole au calme -> Full+Live+all. Tempete/soak/banc de
torture -> Summary+Batch (ou Off). Le terminal ne bouge dans aucun cas.
Episodes `info starved by BACKLOG` = la signature exacte de Full+Live sous
tempete ; ces boutons sont le remede en amont des soupapes automatiques.
