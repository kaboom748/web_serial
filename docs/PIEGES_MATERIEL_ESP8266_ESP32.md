# web_spi — Pièges matériels ESP8266 / ESP32

## Le mémo « anti-casse-tête » pour le prochain programmeur

> Révision v2.3 (2026-07-24) : Piège 56 (le faux-refus du tampon mute).
> Révision v2.2 (2026-07-23 soir) : Pièges 50-55 (collision allocateur SYS, modem-sleep, if sans accolades, catch muet, éditions partielles, économie safe-mode).
> Révision v2.1 (2026-07-23) : Pièges 43-49 ajoutés (session web_serial n°3 — trois crashs de terrain instruits : nano-vfprintf/alignement, fenêtre OOM, empilement WS).

Ce document ne remplace pas le cahier des charges (CDC), l'audit (L-n) ni la
logique d'implémentation : il les **complète** sur un seul axe — les vérités
matérielles des ESP8266 et ESP32 qui, si on les ignore, coûtent des jours.
Chaque point est du concret vérifiable, formulé « le piège → pourquoi → ce
qu'on fait ». Les renvois `GF-n`, `IMP-n`, `L-n` pointent vers le dossier.

Beaucoup viennent directement du vécu de `web_i2c` (même serveur, même
discipline) ; ils s'appliquent tels quels. Ceux marqués **[SPI]** ou
**[ST7735]** sont propres à ce projet.

---

## 0. La règle qui résume tout

**L'ESP8266 est une machine coopérative mono-cœur avec ~40 Ko de heap libre et
~4 Ko de pile.** Tout ce qui bloque, alloue, ou grossit non-borné le tue. Le
dossier a déjà internalisé ça (buffers fixes, throttle EMA, `set_timeout`, page
en flash). Ce mémo dit *où exactement* ça mord dans le contexte SPI+ST7735, et
*ce que l'ESP32 change* (souvent : « il pardonne, mais pas partout »).

---

## 1. Mémoire — le front principal sur ESP8266

### 1.1 Le budget heap réel, et pourquoi le framebuffer le mange

Sous ESPHome + WiFi, un ESP8266 démarre avec **~40 à 46 Ko de heap libre**,
*avant* que le driver ST7735 n'alloue son framebuffer. Or ce framebuffer fait
(GF-13, à graver dans le README et affiché dans Info) :

| Dalle × format | Octets | Verdict ESP8266 |
|---|---|---|
| 128×160 × 16 bpp | 40 960 | **déconseillé** (heap restante < 6 Ko → instable) |
| 128×128 × 16 bpp | 32 768 | limite |
| 80×160 × 16 bpp | 25 600 | OK |
| 128×160 × 8 bpp | 20 480 | OK (mais lent, cf. §2.3) |

C'est un **fait ESPHome vanilla**, pas une faiblesse de web_spi : un
128×160×16b sur ESP8266 est déjà fragile sans aucun outil. La conséquence pour
nous est absolue et c'est **IMP-6** : **le miroir ne copie jamais le
framebuffer et n'en envoie jamais une copie brute.** 41 Ko copiés = OOM
immédiat ; 41 Ko × 5 Hz en WS = backlog explosé. Le miroir à tuiles delta
(lecture directe de `buffer_` via le getter greffé, hash, envoi de tuiles de
512 o max construites *dans* `out_`) est la seule voie. Zéro allocation par
tuile (GF-5). Sur ESP32 (200-300 Ko de heap) le problème disparaît, mais **on
garde l'architecture tuiles** — elle sert aussi le débit WS.

### 1.2 Fragmentation : le tueur lent (vécu web_i2c)

Sur ESP8266, allouer/libérer des tampons de taille variable **fragmente** le
heap ; au bout de quelques heures, `getFreeHeap()` ment (il reste des octets,
mais pas contigus) et une allocation banale échoue. Règle héritée, non
négociable : **aucune allocation sur le chemin chaud** (`tap_bytes_`,
`tap_end_`, `dc_level_` — GF-1). Les `std::string` des messages restent courts
et à vie brève. Toutes les structures d'état sont **fixes et dimensionnées à la
compilation** (`TapFrame`, `SpiTapEntry[WSPI_AGG_SLOTS]`, `tile_hash_[80]`,
`dev_stats_[8]`). Le soak de 24 h (T-8/T-9) surveille la **dérive du min-heap** :
c'est le seul test qui attrape la fragmentation.

### 1.3 La pile est minuscule — surtout ne pas y poser de gros tampons

La pile « cont » de l'ESP8266 fait **~4 Ko**. Un tampon local de quelques
centaines d'octets sur un chemin d'appel profond suffit à la faire déborder —
et le crash est loin du coupable, donc difficile à diagnostiquer. C'est
exactement le piège #2 de web_i2c (une trame construite sur la pile, atteinte
par un chemin profond, qui plantait). Application directe **[SPI]** : les
**mires** (`TEST fill/rgb/checker/gradient…`) ne construisent jamais l'écran
entier en RAM. Elles streament par **lignes de 256 o depuis un tampon de pile
fixe** (CDC §6.3). De même l'autotest/loopback (L-2/L-3) utilisent un motif de
256 o ×2, pas plus. Règle : sur ESP8266, un tampon local > ~256 o est un
signal d'alarme ; sur ESP32 (pile de tâche ~8 Ko, réglable) c'est plus souple,
mais le code doit rester correct sur les deux.

### 1.4 IRAM (ESP8266) — le piège si on ajoute une interruption

L'IRAM de l'ESP8266 (~32 Ko, partagée avec le cache d'instructions) est
**quasi pleine** après le core + WiFi. Toute fonction marquée `IRAM_ATTR` /
`ICACHE_RAM_ATTR` y consomme de la place, et un dépassement d'IRAM est une
**erreur de link** brutale (« section .text will not fit in region iram1 »).
Le CDC a raison de préciser que `dc_level_` **n'a pas besoin d'IRAM** : il est
appelé dans le fil du driver, hors interruption. **Corollaire pour la suite :**
si un jour quelqu'un veut échantillonner un GPIO *sous interruption* (ex. une
fausse bonne idée d'analyseur logique — déjà rejetée en R-1/R-2), il devra
mettre le handler en IRAM et se heurtera au mur. C'est une des raisons de fond
pour lesquelles l'analyseur chronogramme est **IMPOSSIBLE** ici, pas seulement
une question de débit. Sur ESP32, l'IRAM est plus large et la question est
moins tendue, mais un ISR reste `IRAM_ATTR`.

---

## 2. Temps, watchdog, WiFi — ne jamais bloquer

### 2.1 `delay()` est interdit au-delà de ~1 ms (GF-2)

Sur ESP8266, le WiFi tourne **coopérativement** : il a besoin que la boucle
rende la main régulièrement. Un `delay()` long (ou une boucle d'attente)
affame la pile réseau → déconnexions, puis le **watchdog logiciel (~3,2 s)**
puis le **watchdog matériel (~8 s)** redémarrent la puce. **[ST7735]** Les
attentes normatives du contrôleur sont donc un piège mortel si on les code
naïvement :

| Après… | Attente datasheet | Ce qu'on écrit |
|---|---|---|
| RESET matériel ou SWRESET | **120 ms** avant SLPOUT | `set_timeout` chaîné, jamais `delay(120)` |
| SLPOUT | **120 ms** avant DISPON | idem |
| SLPIN → SLPOUT | ≥ 120 ms | idem |
| impulsion RESET | ≥ 10 µs | 1 ms bas (marge ×100) **non bloquant** |

Le séquenceur d'init (`LCDINIT`, FSM `initfsm_state_`) sérialise ces 120 ms par
**timeouts enchaînés** — une petite machine à états, pas une suite de `delay`.
`LCDRST` = 1 ms bas puis timeout 120 ms puis relance. C'est LE point où un
débutant « fait simple » avec `delay(120)` et se retrouve avec un ESP qui
reboote en boucle dès qu'il touche l'écran.

### 2.2 Le boot est une fenêtre sacrée (vécu web_i2c)

Pendant les premières secondes, la pile WiFi fait son handshake. Une écriture
SPI **longue** à ce moment (un driver qui pousse un framebuffer complet dès le
premier `update()`) bloque la boucle et fait échouer le handshake → l'ESP
n'apparaît jamais sur le réseau, ou reboote. D'où **B-4 / `WSPI_BOOT_GRACE`** :
les écritures de plus de `WSPI_BOOT_MAX` (64) octets sont **différées pendant
12 s** après le boot. Ne pas retirer ce garde-fou en pensant « ça ralentit le
premier affichage » — c'est ce qui rend le boot fiable.

### 2.3 **[ST7735]** Le mode 8 bits : lent, mais pas instable (à comprendre)

En `eight_bit_color: true`, le driver convertit 332→565 **par pixel** et envoie
par `write_byte` — soit **40 960 appels délégué** par refresh 128×160 (A9). Le
tap ajoute ~1,2 µs/appel → **~49 ms** de CPU en plus des ~150 ms que ce chemin
coûte déjà en vanilla. Un refresh peut donc frôler **200 ms**. Le réflexe
« ça va tripper le watchdog » est **faux ici**, et il faut savoir pourquoi :

- le poller n'est **pas préemptif** : un refresh est une seule passe, pas une
  boucle infinie ;
- 200 ms < 3,2 s (WDT logiciel) : aucune trame ne le déclenche ;
- le WDT est nourri **entre** les refresh (retour de `loop()`).

Verdict mesuré (T-9) : **lenteur, pas instabilité, zéro reset.** On le
**supporte + on avertit à la compilation** (GF-12) : le walk détecte
`eight_bit_color: true` et `to_code` émet un warning recommandant 16 bits ou
80×160. Point de vigilance : sur ESP8266 **à 80 MHz**, ce chemin est deux fois
plus lent qu'à 160 MHz — recommander 160 MHz aux configs 8 bits.

### 2.4 Le throttle EMA protège le WiFi (hérité, ne pas affaiblir)

Le tap n'émet qu'au plus une trame par `K × (période de loop lissée)` : boucle
rapide → émet librement ; boucle chargée → n'émet presque plus, rend le CPU au
serveur et au WiFi (GF-7). C'est ce qui permet à `Live` de tenir sans crasher
sur un bus chargé. Le miroir a sa propre borne (au plus 1 tuile/loop sur
ESP8266, et seulement si backlog < moitié — GF-4/GF-5). Règle structurelle du
projet : **au pire on throttle, jamais on plante.**

---

## 3. **[SPI]** Les broches matérielles — LE piège n°1 du SPI sur ESP

### 3.1 Mauvaises broches = bit-bang silencieux 30-80× plus lent

C'est **la** panne SPI la plus fréquente et la plus insidieuse. Si les broches
déclarées ne sont pas celles du SPI **matériel** du MCU, ESPHome bascule
**sans aucune erreur** sur un délégué logiciel (`SPIDelegateBitBash`,
spi.h:300+), 30 à 80 fois plus lent. L'utilisateur voit « 8 MHz » dans sa
config et un écran qui rame ; rien dans les logs ne le signale.

**Les broches HW à connaître par cœur (à coder en dur côté JS, cf. L-2) :**

| MCU / bus | SCK | MOSI | MISO | CS natif |
|---|---|---|---|---|
| **ESP8266** HSPI | GPIO14 | GPIO13 | GPIO12 | GPIO15 |
| **ESP32** VSPI (SPI3) | GPIO18 | GPIO23 | GPIO19 | GPIO5 |
| **ESP32** HSPI (SPI2) | GPIO14 | GPIO13 | GPIO12 | GPIO15 |

Différence **capitale** entre les deux familles :
- **ESP8266** : hors HSPI (14/13/12), c'est **bit-bang, point.** Aucune
  alternative matérielle.
- **ESP32** : le **GPIO matrix** route le SPI vers presque n'importe quelle
  broche — mais seules les broches **IOMUX natives** (le tableau) donnent la
  pleine vitesse ; toute autre broche plafonne (~40 MHz → moins) et ajoute de
  la latence. Donc sur ESP32 « ça marche » sur d'autres broches, mais plus
  lentement, et personne ne comprend pourquoi.

C'est pour ça que **L-1 (durée→débit effectif)**, **L-2 (`SELFTEST`)** et
**L-3 (`LOOPBACK`)** existent : ils rendent ce piège *visible* — « 8 MHz
demandés, **240 kHz mesurés** → bus en SPI logiciel ? pins ≠ SPI matériel ? ».
Sans ces mesures, le programmeur *et* l'utilisateur perdent des heures. C'est,
à mon avis, la fonctionnalité la plus rentable de tout l'audit.

### 3.2 **[ST7735]** Les vitesses ont deux plafonds distincts (GF-8)

Le contrôleur ne lit pas à la même vitesse qu'il écrit :

- **écriture** : cycle SCL ≥ 66 ns → **15,1 MHz max** ;
- **lecture** : cycle SCL ≥ 150 ns → **6,6 MHz max**.

Lire à 8 MHz renvoie des octets **corrompus « parfois »** — un piège classique,
faux-intermittent, qui fait dire « mon RDDID marche une fois sur trois ». La
prescription (GF-8) l'évapore d'office : toute lecture (§6) **bascule
automatiquement** le délégué à ≤ 4 MHz, puis restaure. Le RELAI plafonne
l'écriture à 15 MHz quand la cible est l'afficheur. Ne pas « optimiser » en
lisant à la vitesse d'écriture.

---

## 4. GPIO — les broches qu'on ne touche pas, et celles qui bootent

La boîte à outils GPIO (**L-4** : lire/piloter/pulser une broche hors bus, ex.
le **backlight** — cause n°1 d'« écran noir ») ouvre la porte à des dégâts si
elle n'est pas bardée de garde-fous. D'où **GF-17** (liste de protection) et
**GF-18** (broches de strapping). Détail matériel par MCU :

### 4.1 ESP8266

- **Flash : GPIO6-11.** Interdites absolues (GF-17). Y toucher = crash instant
  (c'est le bus flash). GPIO9/10 ne sont utilisables qu'en mode flash DIO,
  risqué — à traiter comme interdites.
- **Strapping : GPIO0, 2, 15** (GF-18). Leur niveau **au reset** choisit le
  mode de boot : GPIO0 doit être HAUT (bas = mode flash), GPIO2 HAUT, GPIO15
  BAS. Les piloter *en service* est permis (avec avertissement), mais s'ils
  sont maintenus au mauvais niveau **au prochain reset**, l'ESP ne boote pas.
- **GPIO16 (D0) est à part.** Il n'est **pas** dans le registre d'entrée GPIO
  standard (`GPIO_IN`, 0x60000318) — il a son propre registre RTC. En Arduino,
  `digitalRead(16)`/`digitalWrite(16)` fonctionnent (ils gèrent le cas), donc
  la prescription CDC §5.1 (`digitalRead(dc_num)`) est **correcte** même si le
  D/C ou le backlight est sur GPIO16. **Mais** si quelqu'un « optimise » en
  lisant le registre `GPIO_IN` directement, GPIO16 renverra toujours 0. Pas
  d'interruption ni de PWM sur GPIO16 non plus.
- **ADC : un seul (A0/TOUT), 0-1 V** (0-3,3 V avec le pont du Wemos). Le
  moniteur VCC (**L-13**, `ESP.getVcc()`) exige `ADC_MODE(ADC_VCC)` et **est
  incompatible avec toute lecture de A0** — la validation Python doit refuser
  la cohabitation (déjà prévu).

### 4.2 ESP32

- **Flash : GPIO6-11** (idem, SPI0/1 internes). Interdites.
- **Broches d'entrée seule : GPIO34, 35, 36 (VP), 39 (VN)** — **input only,
  sans pull interne, impossibles à piloter en sortie.** Donc jamais de D/C,
  RESET, CS ou MOSI dessus, et la commande `GPIO <n> 0|1` doit **refuser**
  34-39 (à ajouter au verdict de GF-17 : « broche en entrée seule »). Un
  débutant qui met le RESET du ST7735 sur GPIO34 aura un écran mort
  inexplicable.
- **Strapping : GPIO0, 2, 5, 12, 15** (GF-18). **GPIO12 (MTDI) est le plus
  dangereux** : il sélectionne la **tension du flash** (VDD_SDIO) au boot. Le
  tirer HAUT au reset sur une carte à flash 3,3 V fait choisir 1,8 V → la carte
  ne boote plus, et un `espefuse` mal placé peut la **briquer définitivement**.
  Le garde-fou GF-18 doit être particulièrement explicite sur GPIO12 (ESP32).
- **ADC2 est en conflit avec le WiFi** : impossible de lire un canal ADC2 tant
  que le WiFi tourne. Sans objet pour le VCC (pas d'`getVcc()` sur ESP32,
  champ `n/a` en L-13), mais à savoir si un jour on lit de l'analogique.
- **Détecteur de brownout** : l'ESP32 se **reset** tout seul si VCC chute
  (contrairement à l'ESP8266). Utile à corréler avec la sentinelle d'init
  (L-8) : sur ESP32, un creux d'alim se voit comme un reset (raison de reset
  dans Info) ; sur ESP8266, il faut le VCC monitor (L-13) pour l'attraper.

### 4.3 Le relâchement à la déconnexion (hérité de la philosophie web_i2c)

Toute broche pilotée par `GPIO`/`GEN`/le RELAI est **relâchée à la
déconnexion WS** et sur auto-timeout (GF-9). L'outil ne laisse **jamais** le
matériel dans un état qu'un onglet fermé ne documente plus. C'est la même
règle que le HOLD et la pause auto-resume : un seul maître, et il rend toujours
la main. Registre des broches touchées borné (≤ 8 entrées).

---

## 5. ESP8266 vs ESP32 — les différences d'API à encapsuler d'emblée

Tout code qui touche le matériel doit exister en **deux variantes** derrière
les gardes `USE_ESP8266`/`USE_ESP32` et `USE_ARDUINO`/`USE_ESP_IDF`. À faire
une fois, proprement, sinon ça pique à chaque fonction :

| Besoin | Arduino (8266 + ESP32-arduino) | ESP-IDF (ESP32) |
|---|---|---|
| lire un GPIO | `digitalRead(n)` | `gpio_get_level((gpio_num_t) n)` |
| piloter un GPIO | `pinMode` + `digitalWrite` | `gpio_set_direction` + `gpio_set_level` |
| relâcher en entrée | `pinMode(n, INPUT_PULLUP)` | `gpio_set_direction` + `gpio_set_pull_mode` |
| heap libre | `ESP.getFreeHeap()` | `esp_get_free_heap_size()` |
| VCC (8266 only) | `ESP.getVcc()` | **n/a** |

Bonne nouvelle : `web_i2c.cpp` a **déjà** ces helpers bi-framework
(`wi_pin_get/drive/release`), testés sur les deux. Le CDC prescrit de les
**copier tels quels** (référentiel API, PARTIE 0). Ne pas les réécrire.

### 5.1 `micros()` déborde à ~71 min — sur les DEUX (vécu web_i2c)

`micros()` renvoie un `uint32_t` qui **boucle toutes les ~71,58 minutes**, sur
ESP8266 comme sur ESP32 (même via Arduino). C'est le piège #9 de web_i2c :
tout calcul de delta (`us - précédent`) doit se faire en **arithmétique
non signée 32 bits** — `(a - b) >>> 0` en JS, soustraction `uint32_t` en C++ —
sinon un delta devient négatif/absurde une fois par heure. S'applique ici à :
la durée de trame **L-1** (`dur_us = micros() - tap_.us` dans `tap_end_`), les
deltas inter-trames du journal (hérités), et les mesures de débit de
`SELFTEST`/`LOOPBACK`. Testé sur hôte (vecteurs de wrap) avant matériel.

### 5.2 Le miroir ne provoque **pas** de course — mais attention à l'ISR/tâche

Sur ESP8266 (mono-cœur, coopératif) **et** sur ESP32 (les composants ESPHome
tournent tous dans la **tâche loop unique** par défaut), le scan du miroir
(dans `loop()`) et l'`update()` du driver s'exécutent **séquentiellement**,
jamais en parallèle. Donc lire `buffer_` pendant que le driver l'écrit n'est
**pas** une course — les deux ne se chevauchent pas. **La règle qui en
découle :** ne **jamais** lire `buffer_` depuis une **interruption** ou une
**autre tâche FreeRTOS** — là, la course serait réelle. Le miroir reste
strictement dans `loop()`, budget borné (WSPI_TILES_SCAN/SEND). C'est un point
subtil : ce qui est sûr aujourd'hui le reste *tant qu'on ne sort pas de la
boucle*.

---

## 6. **[ST7735]** Le miroir — endianness et format, le piège des couleurs

### 6.1 RGB565 est stocké **gros-boutiste** en mémoire

Le driver ST7735 range le RGB565 dans `buffer_` **octet fort d'abord**
(big-endian, st7735.cpp:319-320). Le miroir envoie les octets **bruts** (GF-14 :
l'ESP ne convertit jamais un pixel) et c'est le **JS** qui interprète. La trame
tuile porte donc `fmt=0` (565BE) : le décodeur JS doit lire big-endian. Se
tromper d'ordre = des couleurs « presque bonnes mais délavées/fausses », un
grand classique. Le format 8 bits indexé (RGB332) est `fmt=1` : le JS applique
la **même table 332→565 que le driver**, plus `use_bgr`.

### 6.2 **[ST7735]** R/B inversés et offsets de dalle — deux pièges à rendre visibles

Deux bugs ST7735 archi-fréquents, que l'UI doit faire **sauter aux yeux** plutôt
que masquer :

- **use_bgr / MADCTL bit RGB** : rouge et bleu inversés. La mire `rgb` affiche
  trois bandes **étiquetées par position** (« bande de gauche = R attendu ») :
  si le rouge n'est pas où l'étiquette le dit, l'inversion crève l'écran. Le
  décodeur JS éclate aussi MADCTL bit à bit (MY MX MV ML **RGB** MH).
- **col_start / row_start (dalles « green tab »)** : offsets de 2/1 px non
  appliqués → image rognée ou décalée. La géométrie **avec offsets** est
  extraite au walk build-time et **affichée dans Info** ; la mire `window`
  (cadre 1 px + croix) rend un mauvais offset immédiatement visible (bords
  rognés). Zéro re-saisie utilisateur, et le piège est diagnostiqué, pas subi.

---

## 7. **[SPI]** Le protocole de lecture — la source n°1 d'« impossible » mal posé

### 7.1 Le cycle d'horloge factice (dummy clock) décale tout d'un bit

Après l'octet de commande, les lectures **24/32 bits** (RDDID 04h, RDDST 09h,
RAMRD 2Eh) exigent **1 cycle d'horloge factice** avant le premier bit utile ;
les lectures **8 bits** (RDID1-3, RDDPM, RDDMADCTL…) n'en ont **pas**. Oublier
le dummy = **tout est décalé d'un bit**, et on lit « n'importe quoi » — LE détail
qui fait abandonner en criant « mon RDDID est cassé ». Prescription (§3.2) :
lire **(N+1) octets** et **réaligner d'un bit** côté C++ (`realign_dummy1(buf,
n)`, ~6 lignes, **testable sur hôte** — vecteurs bit-à-bit en T-6). À
appliquer *seulement* pour 04h/09h/2Eh (GF-11).

### 7.2 SDO non câblé = lectures 0xFF/0x00 plausibles (IMP-4)

Les bits de lecture sortent sur la broche **SDO** du ST7735, distincte du MOSI.
Beaucoup de modules 1,8"/0,96" **ne la routent pas** au connecteur. Aucun
logiciel ne lit une broche absente. Le piège : les lectures renvoient alors
0xFF (ou 0x00) **constants**, ce qui *ressemble* à une réponse. D'où **PROBE**
(RDID1 ×3 : si constant → bandeau « SDO non câblé — lectures indisponibles »)
et le détecteur MISO mort généralisé (**L-7** : `rx_and==0xFF && rx_or==0xFF`
après ≥ 64 o). Règle d'or, héritée de web_i2c : **jamais présenter une valeur
sans son statut brut à côté** (`raw` dans le JSON), jamais de décodage
plausible-mais-faux (GF-11).

---

## 8. Build-time — l'auto-patch, fragile aux versions d'ESPHome

Pas un piège *runtime*, mais celui qui **surprendra le programmeur à la
prochaine mise à jour d'ESPHome**, donc à documenter en gras. web_spi greffe du
code dans les **sources copiées du build** à chaque compilation
(`monitor_patch.py`) : le hook `SPIMonitor` sur `spi`, et **en v2 un getter
`wspi_buffer()`** dans `display_buffer.h` (§7.3, activé seulement si
`display_id:`). Les ancres (numéros de ligne, texte exact) **bougent d'une
version ESPHome à l'autre**. Deux garanties imposées :

- **échec bruyant** : si l'ancre a disparu, le pre-script **stoppe la
  compilation** avec un message clair (« retirez `display_id:` en attendant »),
  jamais un binaire silencieusement cassé ;
- **idempotence + adaptativité** : marqueur anti-double-greffe, regex tolérante
  aux espaces.

Preuve d'innocuité de la greffe framebuffer : ajouter des **labels d'accès**
(`public:`/`protected:`) est du C++ légal, **aucun membre ajouté** ⇒ layout
inchangé ⇒ **ABI intacte** pour tous les autres composants. Le getter rend le
pointeur en **lecture seule** (contrat commenté). Tests T-1 obligatoires :
patcheur sur les sources 2026.7.0 réelles **et** sur un upstream simulé cassé
(vérifier l'échec bruyant). Rappel de la discipline `web_i2c.h` : changer une
**struct** (`TapEntry`/`TapFrame`) = **clean build obligatoire** (ABI) ; un
changement `page.cpp`/`.cpp` seul est incrémental-safe.

---

## 9. La page servie — flash, alignement, ASCII (hérité web_i2c)

- **La page (~30-34 Ko HTML+JS) vit en flash (PROGMEM), servie par tranches de
  512 o** (`progmem_memcpy`, GF-16). Ne **jamais** la charger entière en RAM
  (elle vaut ~75 % du heap ESP8266). Elle reste **en flash, jamais en RAM**.
- **Sur ESP8266, lire la PROGMEM se fait par accès aligné 32 bits.** Déréférencer
  un `const char*` PROGMEM octet par octet **crashe** (exception LoadStoreError).
  Le service par `progmem_memcpy` (v1) est correct — ne pas le « simplifier » en
  boucle de copie naïve.
- **`page.cpp` reste ASCII pur, un seul littéral PROGMEM** (GF-16). Tout glyphe
  non-ASCII passe par `\uXXXX` (JS) ou entité HTML. C'est une contrainte
  vérifiée à chaque build (piège récurrent de web_i2c : un caractère accentué
  qui casse le littéral).

---

## 10. Récapitulatif — le tableau à coller au mur

| # | Piège | Cible | Parade | Réf. |
|---|---|---|---|---|
| 1 | Framebuffer 41 Ko = OOM | 8266 | miroir tuiles, zéro copie | IMP-6, GF-5 |
| 2 | Fragmentation heap | 8266 | buffers fixes, rien sur chemin chaud | GF-1 |
| 3 | Pile ~4 Ko débordée | 8266 | mires/tests streamés, tampon ≤ 256 o | §1.3 |
| 4 | IRAM pleine si ISR | 8266 | pas d'échantillonnage sous interruption | R-1/R-2 |
| 5 | `delay(120)` du ST7735 | 8266 | `set_timeout` chaînés | GF-2 |
| 6 | Boot bloqué par grosse écriture | 8266 | grâce de boot 12 s | B-4 |
| 7 | Mode 8 bits ~200 ms/refresh | 8266 | supporté + warning compil, viser 160 MHz | GF-12 |
| 8 | **Mauvaises broches = bit-bang muet** | les 2 | mesure débit effectif + SELFTEST/LOOPBACK | L-1/2/3 |
| 9 | Lire à 8 MHz = octets corrompus | ST7735 | lecture forcée ≤ 4 MHz | GF-8 |
| 10 | Broches flash 6-11 | les 2 | interdites | GF-17 |
| 11 | Strapping 0/2/15 (8266), +5/12 (ESP32) | les 2 | avertissement, GPIO12 dangereux | GF-18 |
| 12 | GPIO34-39 = entrée seule | ESP32 | refus en pilotage | §4.2 |
| 13 | GPIO16 hors registre standard | 8266 | `digitalRead`, pas le registre brut | §4.1 |
| 14 | `micros()` déborde à 71 min | les 2 | delta en uint32 non signé | §5.1 |
| 15 | Lire `buffer_` depuis ISR/tâche | les 2 | miroir uniquement dans `loop()` | §5.2 |
| 16 | RGB565 gros-boutiste | ST7735 | JS lit big-endian | §6.1 |
| 17 | R/B inversés, offsets green-tab | ST7735 | mires `rgb`/`window`, géométrie affichée | §6.2 |
| 18 | Dummy clock (RDDID décalé d'1 bit) | ST7735 | `realign_dummy1`, testé hôte | §7.1 |
| 19 | SDO non câblé = 0xFF plausible | ST7735 | PROBE + jamais de valeur sans `raw` | IMP-4, GF-11 |
| 20 | Auto-patch cassé à la maj ESPHome | build | échec bruyant, idempotent | §7.3 |
| 21 | PROGMEM lue non alignée = crash | 8266 | `progmem_memcpy` uniquement | §9 |

---

## En un paragraphe

L'ESP8266 est la contrainte ; l'ESP32 pardonne, sauf sur les broches d'entrée
seule (34-39), le strapping GPIO12, et les broches non-IOMUX qui bornent la
vitesse SPI. Le dossier a déjà bâti la bonne architecture (tuiles, throttle,
`set_timeout`, buffers fixes, échecs bruyants) — ces pièges expliquent
**pourquoi** chaque garde-fou existe et **où** le prochain « ça marche pas »
va surgir. Les trois qui coûteront le plus de temps si on les ignore : le
**bit-bang muet sur mauvaises broches** (§3.1), le **`delay()` du ST7735**
(§2.1), et le **dummy clock des lectures** (§7.1). Tout le reste est dans le
tableau §10.

---
---

# PARTIE II — Le vécu web_serial / wser (transports réseau, 4 crashs terrain)

Ajoutée après la campagne web_serial : un hub série multi-transports (TCP,
UDP, Web Serial bridges, module raw-lwip maison) poussé jusqu'à la tempête
de boucles entretenue sur D1 mini. **Quatre crashs terrain, quatre modes de
mort différents, chacun symbolisé à l'adresse près** (pyelftools sur l'ELF)
et corrigé. Ces pièges-là ont coûté une nuit ; qu'ils n'en coûtent plus.

## 11. Les TROIS contextes d'exécution du 8266 — la carte qui manquait

Le §5.2 disait « tout tourne dans la loop ». C'est vrai des *composants* ;
c'est FAUX de la pile réseau. La vérité complète :

- **cont** : la pile de `loop()` (~4 Ko). `yield()`/`delay()` permis.
- **sys** : la pile système. **Les callbacks lwip raw, les timers OS et les
  handlers WiFi s'exécutent ICI.** Tout `yield()` (donc tout `delay()`,
  et tout code qui en appelle un) y déclenche
  `Panic core_esp8266_main.cpp __yield` — crash terrain n°3, symbolisé
  dans `web_server::handleRequest` (ESPAsyncWebServer exécute ses handlers
  en contexte lwip).
- **ISR** : les vraies interruptions (IRAM, cf. §1.4).

**Règle absolue pour tout callback lwip/réseau : ring-fill-only.** Copier
dans un anneau fixe, incrémenter des compteurs, `pbuf_free`, RIEN d'autre —
pas de log (`ESP_LOG` écrit l'UART !), pas d'allocation, pas de yield. Le
travail se fait dans `loop()` qui draine l'anneau. Le pattern `rx_stage_`
n'est pas une préférence : c'est la SEULE forme légale.

## 12. AUTO_LOAD — les passagers clandestins du binaire

Le YAML ment par omission : un composant peut en embarquer d'autres via
`AUTO_LOAD()` sans qu'aucune ligne ne les nomme. Vécu : `captive_portal:`
charge d'office `web_server_base` **et** `ota.web_server` — donc
**ESPAsyncWebServer complet, résident, handlers en contexte sys** (cf. §11)
et ~8-12 Ko de heap — sur un YAML qui ne contient PAS `web_server:`.
**La vérité n'est jamais le YAML : c'est le dump de config au boot**
(`[C][...]` lignes). Pour chasser un composant, vérifier sa disparition DANS
LE DUMP, et remonter les AUTO_LOAD dans les `__init__.py` d'ESPHome quand un
`[C]` inexpliqué apparaît. Corollaire : `ota: platform: esphome` (port 8266)
ne dépend de rien de tout ça — c'est l'OTA à garder.

## 13. Toute file non bornée est un crash différé (vecteur qui double)

Crash terrain n°1 : la file d'egress WebSocket (`out_`, un `std::vector`)
enflait sous tempête de logs ; son DOUBLEMENT a demandé 10 752 octets
contigus — OOM. Le piège du doublement : le besoin transitoire est
ancien+nouveau bloc SIMULTANÉS, un garde « heap libre > taille » ne le voit
pas venir. **Prescription pour chaque file du produit : deux plafonds.**
Cap doux = les messages droppables (logs, trames, info) sont sautés et
COMPTÉS (compteur visible dans l'UI — `WS drops`) ; cap dur = larguer le
client/consommateur (il se reconnecte sur une file vide). Dimensionner par
plateforme (2k/4k sur 8266, 8k/16k sur ESP32). « Au pire on throttle,
jamais on plante » s'applique à CHAQUE tampon, pas seulement au chemin de
données.

## 14. La tempête de logs est un tueur à part entière (crash par WDT)

Crash terrain n°2 : watchdog MATÉRIEL, loop montée à 880 ms. Arithmétique :
chaque `ESP_LOGW` ≈ **5 ms d'écriture SYNCHRONE sur l'UART0 à 115200**,
plus le transfert vers le client API (chiffré Noise = CPU). Un verdict de
détection re-loggé 3×/s sur 4 ports = 60 ms/s de logs → la loop enfle →
le WiFi s'affame → erreurs → plus de logs → spirale → WDT.
**Prescriptions :** (a) tout événement à état collant (badge, verdict,
panne) se logge **sur la TRANSITION uniquement** (`bool first = !flag;`
avant de poser le flag) — jamais sur la répétition ; (b) les échecs en
rafale (sendto…) loggent le PREMIER d'un run avec errno, puis comptent en
silence ; (c) en session de tempête/banc : `logger: level: INFO` — DEBUG +
client API connecté est un amplificateur ; (d) une escalade de
« took a long time » qui MONTE régulièrement = spirale en cours, pas du
bruit : chercher la boucle de rétroaction.

## 15. Les chaînes de `+` sur std::string — l'OOM du pauvre (crash n°4)

`std::string j = "a" + to_string(x) + "b" + ...` × 59 : chaque `+` crée une
temporaire réallouée. Construire ~1,9 Ko d'info JSON ainsi = des dizaines
d'allocations par push, chaque seconde. Sur un heap laminé (largest 7 Ko,
frag 37 %), une étape a demandé 1 921 octets qui n'existaient plus — OOM en
plein `cont`, fragments de JSON visibles dans la stack dump.
**Prescriptions :** (a) `reserve()` avant toute phase d'append en boucle ;
(b) GARDE HEAP à l'entrée de tout bâtisseur de gros message
(`largest_block < besoin+marge` → **sauter ce push**, le suivant
resynchronise — un panneau qui clignote vaut mieux qu'un reboot) ;
(c) les fragments ASCII dans une stack dump (morceaux de JSON, de noms de
champs) sont des indices INDÉPENDANTS du build — les lire.

## 16. Modules sans dépendance : le garde de compil doit être TOOLCHAIN

Un module purifié de tout include ESPHome ne voit PLUS les macros ESPHome.
Vécu : garde `#ifdef USE_SOCKET_IMPL_LWIP_TCP` dans un .cpp qui n'inclut
que lwip → macro jamais définie → **unité de traduction VIDE** → le .o
existe, creux → mur d'`undefined reference` au link, invisible à toute
passe de validation qui ne linke pas.
**Prescription :** les gardes d'un module autonome reposent sur (a) les
macros de TOOLCHAIN (`ESP8266`, `ARDUINO_ARCH_ESP8266` — posées par
PlatformIO, visibles partout) et (b) une macro PROPRE au module
(`WSER_TARGET_LWIP`) que le build system pose explicitement
(`cg.add_build_flag` dans `__init__.py`). Jamais une macro d'un framework
que le module n'inclut pas.

## 17. La dérive de stub — quand le faux enseigne une API inventée

Un stub de test écrit pour la commodité peut définir des helpers qui
N'EXISTENT PAS en amont. Vécu : le stub lwip offrait `ip_addr_get_u32` ;
le module l'a appris ; 86 checks verts, mutations, sanitizers — et mort à
la PREMIÈRE compilation contre le vrai lwip (le vrai nom : `ip4_addr_*`,
et la forme varie IPv4-only vs dual-stack).
**Prescriptions :** (a) un stub ne définit QUE des noms existant en
amont — interdiction d'inventer, écrite en commentaire-loi dans le stub ;
(b) canaliser tout accès brut à une structure externe par des shims nommés
du module (`wser_ip4_get/set`) : la surface à vérifier contre l'amont tient
en un seul endroit ; (c) des `static_assert` canaris sur les valeurs/formes
dont on dépend (codes d'erreur distincts, tailles) : une release qui dérive
casse le BUILD avec un message nommé, pas le terrain.

## 18. lwip raw — les trois contrats + le piège d'alignement

Écrire un client TCP/UDP sur l'API raw exige d'honorer, testés
hostilement :
1. **Le callback d'erreur reçoit un pcb DÉJÀ LIBÉRÉ.** Premier geste :
   `pcb_ = nullptr`. Tout appel lwip dessus ensuite = use-after-free.
2. **`tcp_abort()` dans un callback impose de retourner `ERR_ABRT`** —
   sinon lwip touche le pcb mort.
3. **Flow control par refus** : recv qui retourne `ERR_MEM` → lwip garde le
   pbuf (refused_data) et relivre ; ne JAMAIS jeter un octet TCP ; l'anneau
   doit faire ≥ 1 MSS pour garantir la progression (static_assert).
Et le piège transversal, payé sur ESP32 : **lwip valide l'ALIGNEMENT du
pointeur sockaddr** (`sendto` → EINVAL sur un `uint8_t[28]` non aligné —
l'ingress marchait, l'egress jamais, signature « txerr qui grimpe »).
Jamais de tableau d'octets pour une adresse : `struct sockaddr_storage`
ou un POD maison, avec `static_assert(alignof(...) >= 4)`.

## 19. L'échec de CRÉATION de socket doit backoff comme l'échec de bind
### 19-bis. ...et la RECRÉATION après échecs d'envoi aussi (vécu post-doc)

Panne WiFi + tempête : chaque sendto échoue en dur (errno=5, plus de
route), le compteur d'échecs atteint son seuil, on recrée le socket — et
**le bind RÉUSSIT** (opération locale, WiFi non requis), donc le backoff de
bind ne s'arme jamais : boucle serrée recréation/échec à plusieurs Hz,
churn de pcb + 2 lignes de log par cycle, pendant que la puce essaie de se
ré-associer. **Deux parades cumulées** : (a) suspendre l'egress quand
`network::is_connected()` est faux (txfail gelé, silence total) ;
(b) toute recréation ARME le backoff de création (tok=1, tok_ms=now) —
5 s minimum entre recréations, quelle que soit la cause.

`socket()`/`udp_new()` qui retourne nullptr (type non supporté, OOM) et
qu'on retente au prochain passage = **retry + ESP_LOGE à ~50 Hz**, pour
toujours. Vécu avec `SOCK_DGRAM` sur l'impl `lwip_raw_tcp` du 8266 (qui le
rejette par design — l'UDP y exige le module raw maison). Même régime que
le bind : 5 s entre essais, 3 échecs → port DOWN + UN message.

## 19-ter. Le dernier tampon à borner : le TEMPS de passe (gouverneur CPU)

On peut borner chaque mémoire et chaque débit et mourir quand même : sous
tempête, une passe qui sert N ports ayant TOUS du travail fait tout le
travail disponible — 974 ms mesurées — et le WiFi du 8266, qui vit ENTRE
les passes (coeur coopératif), s'affame jusqu'à la déassociation
('Unspecified' puis 'Authentication Failed' = famine, pas mot de passe).
**Prescription : un budget de temps par passe** (20 ms sur 8266), vérifié
ENTRE les ports (jamais au milieu d'une opération), avec un **curseur
d'équité** : la passe s'arrête où le budget meurt et reprend LÀ au tour
suivant — tourniquet, personne n'affame personne. Le travail est étalé,
pas perdu : les buffers absorbent, l'éviction police, le débit se
dégrade — et le WiFi respire. Sur ESP32 (WiFi = tâche FreeRTOS séparée) le
budget n'est que de la courtoisie envers les autres composants ; sur 8266
c'est une question de survie du lien.

## 20. Forensique de crash — le protocole qui a résolu les quatre

- **L'ELF doit être celui DU BUILD crashé** : une recompilation décale les
  adresses (vécu : `SPIComponent::dump_config` symbolisé pour un crash de
  tempête = ELF désynchronisé, non-sens sémantique = signal d'alarme).
  Copier l'ELF immédiatement après chaque flash de chasse.
- **HA add-on ESPHome** : les builds vivent dans le conteneur —
  côté hôte : `/mnt/data/supervisor/apps/data/<slug>_esphome/build/<nom>/
  .pioenvs/<nom>/firmware.elf` ; en une ligne depuis un SSH add-on
  (protection mode OFF) :
  `docker cp addon_<slug>_esphome:/data/build/<nom>/.pioenvs/<nom>/firmware.elf /config/`
  puis Samba.
- **HW WDT = AUCUNE trace fiable sur 8266.** Le reset watchdog materiel ne
  sauvegarde pas de contexte : le "PC/BT" imprime au boot suivant est un
  FOSSILE RTC d'un ancien crash (preuve vecue : backtrace identique au bit
  pres sur CINQ builds differents). Ne symboliser un HWDT que si la trace
  CHANGE entre crashs ; sinon, poser une boite noire maison : des miettes
  de phase en RAM `.noinit` (survit au reset WDT), une ecriture par etape
  de la loop, lecture+log au boot -- la miette dit OU la machine etait,
  la ou la backtrace ment.
- **Symboliser** : `xtensa-lx106-elf-addr2line -pfiaC -e firmware.elf
  <addr...>` ou pyelftools (table des symboles STT_FUNC : suffit pour
  nommer la fonction). `last failed alloc call` = l'APPELANT du malloc
  fatal — la donnée la plus fiable ; le reste de la stack contient du
  bruit (vieux cadres morts) : chercher les récurrences ENTRE crashs
  (une adresse commune à 3 morts = le fil rouge) et les fragments ASCII.
- **Un crash, un symbole, un nom — AVANT tout retrait de module.**
  « Enlève des trucs et regarde » n'est pas un protocole.
- Les crashs comptent dans safe_mode (10 tentatives → 300 s) : garder le
  compte en session de chasse.

## 21. Web Serial API — le navigateur ne dira JAMAIS « COM7 »

La spec cache le nom OS du port (vie privée) ; seul le sélecteur de Chrome
le montre, jamais la page. Disponible : `getInfo()` = VID:PID USB — nommer
les liens par puce (CH340, FTDI, CP210x…) ; un port VIRTUEL (VCOM,
com0com) n'a AUCUNE identité → « virtual/unnamed ». Étiqueter « COM #n »
un compteur de session = garantir la confusion avec les vrais COM du PC.
Séquence propre : `requestPort` → `open` → binding applicatif → reader ;
déconnexion : `reader.cancel()` AVANT `close()` ; débranchage = fin de la
boucle de lecture ; reboot du périphérique = uptime qui recule.

## 22. L'observabilité AVANT le diagnostic

« L'UDP ne marche pas » est resté insoluble jusqu'à l'ajout de : compteur
`txerr` par port + errno loggé au PREMIER échec d'un run + auto-recréation
après N échecs durs. La capture suivante disait « errno=22 » → EINVAL →
alignement → fix. **Tout chemin d'échec silencieux est une dette de
debug** : compter, logger une fois, exposer dans l'UI. L'interface devient
le Wireshark du pauvre — et c'est elle qui a innocenté ou accusé à chaque
étape.

## 23. Détection par empreintes — compétition et saturation

Un détecteur à empreintes consommables (anti-double-comptage) dans un même
domaine de diffusion : plusieurs boucles se VOLENT les empreintes — le
chemin le plus rapide rafle, les autres restent sans badge. Et la
saturation (buffers qui évincent, re-fragmentation) casse le match exact —
un raté, jamais une fausse alarme, par design. Diagnostic honnête : les
COMPTEURS hurlent même quand les badges se taisent (tx/rx symétriques,
drops qui grimpent). Pour attribuer proprement : isoler chaque paire dans
son VLAN, ou brider le débit (un rate sur un maillon étrangle tout
l'anneau — propriété d'une boucle à gain 1).

## 24. Récapitulatif II — à coller sous le tableau §10

| # | Piège | Cible | Parade | Vécu |
|---|---|---|---|---|
| 22 | yield/log/alloc en contexte sys (callbacks lwip) | 8266 | ring-fill-only, travail dans loop() | crash 3 |
| 23 | AUTO_LOAD embarque AsyncWebServer (captive_portal) | les 2 | croire le DUMP, pas le YAML ; chasser dans les __init__.py | crash 3 |
| 24 | File non bornée → doublement de vecteur → OOM | 8266 | cap doux (skip+compte) + cap dur (larguer le client) | crash 1 |
| 25 | Tempête de logs (UART sync + API Noise) → WDT | 8266 | logs sur TRANSITION, premier-d'un-run, INFO en banc | crash 2 |
| 26 | Chaînes de + std::string → OOM de construction | 8266 | reserve() + garde heap → sauter le push | crash 4 |
| 27 | Garde de compil = macro framework invisible → UT vide | module | macros toolchain + flag module posé par le build | link 8266 |
| 28 | Le stub invente une API inexistante | tests | miroir-amont only + shims nommés + canaris | compile 8266 |
| 29 | sockaddr non aligné → sendto EINVAL permanent | les 2 | sockaddr_storage / POD + static_assert alignof | ESP32 UDP |
| 30 | Échec de création socket retenté à 50 Hz | les 2 | backoff 5 s / 3 échecs → DOWN + un message | UDP 8266 |
| 31 | ELF ≠ build du crash → symbole absurde | forensique | copier l'ELF à chaque flash ; docker cp + addr2line | crash 4 |
| 32 | « COM #n » inventé côté navigateur | UI | identité par VID:PID, « virtual/unnamed » sinon | bridges |
| 33 | Échec silencieux = diagnostic impossible | produit | txerr + errno-premier + auto-récup, visibles UI | UDP ESP32 |
| 34 | Recréation sans backoff pendant panne réseau | les 2 | gate network::is_connected + recréation arme le backoff | panne WiFi 8266 |
| 35 | Tout borner SAUF le temps de passe → WiFi affamé | 8266 | budget par passe (20 ms) + curseur d'équité entre ports | loop 974 ms |
| 36 | Budget par passe SANS plafond de duty → 95% CPU, WDT dans le FIQ WiFi | 8266 | seau de temps rechargé à 35% du temps-mur ; vide = phase sautée | crash 6 |
| 37 | Inondation de paquets + pbufs introuvables → wDev_ProcessFiq se coince (mode connu du SDK) | 8266 | frein par heap : sous un plancher radio (5 Ko), l'ingress bouclant est jeté ; le fil réel jamais | crashs 6-8 |
| 38 | Backtrace HWDT = fossile RTC (identique entre builds !) | 8266 | miettes .noinit par phase, lues au boot (boite noire) | crashs 6-9 |
| 39 | Software-serial TX cycle-exact + FIQ WiFi dense = wedge HWDT | 8266 | UART materiel (UART1 TX-only GPIO2, sans rx_pin) ; sinon fenetres d'ecriture ~2 ms | boite noire 0x60 |
| 40 | Fallback software serial SILENCIEUX d'ESPHome | 8266 | audit au build (replique des regles de selection) : log de boot + badge UI + loi de quota par transport | 9 resets |

---

## En un paragraphe (Partie II)

La Partie I disait : l'ESP8266 est la contrainte, ne jamais bloquer, tout
borner. La campagne web_serial ajoute l'étage au-dessus : **les trois
contextes d'exécution sont la loi** (un yield en sys tue), **le binaire
ment moins que le YAML** (AUTO_LOAD), **toute croissance non bornée — file,
chaîne, log — finit en crash sous tempête**, et **un module autonome doit
porter ses gardes toolchain, ses shims nommés et ses canaris**, parce que
ses stubs et ses frameworks lui mentiront un jour. Et quand ça meurt quand
même : un ELF du bon build, une adresse commune entre les morts, un
symbole, un nom — le protocole qui a clos quatre enquêtes en une nuit.

## Piege 41 -- La pile CONT de 4 Ko : le code qui grossit casse la ou il etait deja profond
Sur ESP8266/Arduino, loop() vit sur une pile de 4096 octets, canari au fond
(`cont_check` verifie a CHAQUE retour de loop). Le chemin d'appel le plus
profond (trame ws -> parseur -> handler -> prefs -> construction d'un gros
JSON -> send) accumule les frames de CHAQUE feature ajoutee au fil des
semaines -- jusqu'a crever les 4 Ko. Symptomes en deux familles selon la
profondeur de l'ecrasement: canari seul -> Exception propre via cont_check;
plus profond -> structures SDK corrompues -> HWDT errant dans ets_post /
ets_intr_unlock (PC baladeur, delais variables, "impossible a reproduire").
Remede structurel: JAMAIS de gros travail (mega-strings, envois) en
profondeur -- les handlers levent un drapeau, la loop execute au niveau ~zero.
Detection: DEBUG_ESP_HWDT change la geometrie des piles et transforme des
HWDT muets en Exceptions attrapables (mais pietine les structs .noinit --
retirer en production).

## Piege 42 -- Les postmortems suivent UART0 ou qu'il pointe
Exception decode + stack dump s'impriment sur UART0 AU PINOUT COURANT et AU
BAUD COURANT du bus. En swap GPIO15/13: le postmortem part DANS l'equipement
du banc, invisible (seule la boite noire RTC temoigne). En GPIO1/3: le CH340
devient console forensique gratuite (banner ROM + dump HWDT a 74880 avec
cristal 26 MHz; postmortem au baud du bus, ex. 19200) -- au prix du spew de
boot injecte dans le bus. Bring-up: GPIO1/3. Production: GPIO15/13 + boite
noire. Le banner ROM se mire aussi sur GPIO2 pendant le boot (LED bleue).


## Piege 43 -- nano-vfprintf et les litteraux longs : la loterie d'alignement (crash #6 web_serial)
Un snprintf dont le format contient un LONG run litteral (>~16 chars d'affilee)
emprunte le chemin word-copy de __ssputs_r (newlib-nano). Selon la PARITE de
placement du litteral -- qui change a CHAQUE edition de liens -- l'acces mot
non aligne leve Exception Alignment (exccause=9), que le handler non32xfer ne
rattrape PAS (il ne corrige que exccause=3). Symptome vicieux : un build passe,
la recompilation suivante crashe, meme code source. Les mini-formats (%02X,
%d courts) survivent sur le chemin octet -- d'ou des snprintf voisins innocents.
REGLE : sur 8266, les messages longs se construisent en std::string + to_string
(le style maison de web_serial existe pour CA). Cas aggrave vecu : le formateur
etait celui du rapport de crash -- chaque mort rearmait la livraison, chaque
livraison tuait. Un MESSAGER de crash doit etre le code le plus paranoiaque du
fichier, pas le plus elegant.

## Piege 44 -- La garde OOM doit couvrir LA PLUS GROSSE DEMANDE CONTIGUE, pas la taille finale (crash #5)
Compile en -fno-exceptions : un echec d'allocation std::string/vector = abort =
reset materiel, AUCUN chemin d'erreur. Une garde "largest >= N" ne protege que
si N couvre la pire demande contigue du bloc garde : reserve() (taille pleine
d'un coup), croissance de vecteur (x2 transitoire), ET le churn des dizaines de
temporaires operator+. Fenetre de mort vecue : garde a 3072, reserve a ~4,7 k
-> largest dans (3072..4700) passe la porte et meurt DANS l'allocation. Fix :
porte = facture complete + marge (5824), re-verification juste avant reserve
(le tas bouge pendant la construction), et l'ARITHMETIQUE en miroir dans la
suite de tests pour que tout futur agrandissement hurle avant le terrain.

## Piege 45 -- Les caps qui testent AVANT d'encoder : l'empilement qui mure le canal
Politique classique : if(out.size()>HARD) drop_client; if(>SOFT) drop_frame;
sinon encode. Le test se fait AVANT l'ajout -> une grosse trame (info 8 ports
~3 k) posee sur un out_ garni juste SOUS soft (lignes sys) gare le canal a
~5 k > HARD, et c'est L'ENVOI SUIVANT qui execute drop_client. Si la config est
persistee, chaque reboot ressuscite le poison : boucle connect/die eternelle,
hub "mure" injoignable. REGLE (STACKDEFER) : toute trame qui SCALE avec la
config ne part que sur canal quasi vide (backlog <= 512), sinon differee et
retentee -- et l'inegalite (defer + pire_trame < HARD) vit dans les tests.

## Piege 46 -- .noinit sur flash vierge = aleatoire PUR : initialiser tout ce que les rapports lisent
Le magic protege la LECTURE du crumb, pas son contenu residuel : sur premier
boot (mismatch), zero-er explicitement CHAQUE champ que le rapport imprimera
(mx*, evt, phase). Sinon le premier vrai crash publie du garbage credible
("mxpass 18 minutes") qui detruit la confiance dans l'instrument -- et la
confiance d'un instrument forensique EST l'instrument.

## Piege 47 -- Un canal qui ACCEPTE un envoi n'est pas un canal PRET
Les gardes par drapeau ont des fenetres de course : ici, le GET de page attache
AUSSI stream_client_, et un push place entre out_.assign(headers) et
serving_page_=true glisse une trame WS ENTRE les en-tetes HTTP et le HTML --
page corrompue ET tronquee (Content-Length calcule sans la trame). REGLE : les
livraisons uniques (rapports, hello) partent d'un point qui a un HISTORIQUE de
succes (ex. la tete de send_info_ : si une info passe, le WS est vraiment la),
jamais du point topologiquement le plus precoce.

## Piege 48 -- Les constantes par plateforme : toute forensique chiffree doit citer LA bonne
WSER_MAX_PORTS = 8 (8266) / 16 (ESP32) ; caps 2048/4096 vs 8192/16384. Une
analyse d'incident menee avec la constante de l'autre puce produit un recit
faux et un correctif a cote (vecu : "info 5,4 k > HARD" calcule avec la table
ESP32 contre les caps 8266 -- le vrai mecanisme etait l'empilement, Piege 45).
Reflexe : grep de LA valeur ifdef AVANT tout calcul.

## Piege 49 -- 80 ou 160 MHz sur le MEME binaire 8266 : exposer l'horloge
system_update_cpu_freq change le silicium sans changer le firmware. Tous les
budgets en microsecondes valent alors le simple ou le double en CYCLES -- deux
hubs identiques aux Max pass du simple au double sont indistinguables sans la
variable. Un champ mhz runtime (ESP.getCpuFreqMHz / esp_clk_cpu_freq) coute une
ligne et ferme l'angle mort ; '0' = inconnu divulgue, jamais devine.


## Piege 50 -- La collision d'allocateur en contexte SYS : lwip -> umm pendant que TON code alloue
LA bete de la session : HWDT Level1Int, PC en ROM (0x40000F68), ctx sys,
sommet de pile = umm_malloc appele par le coeur lwip, et TA chaine d'envoi
(string/encode/write) juste dessous. Mecanisme : le contexte principal est
DANS l'allocateur (constructions de chaines), une preemption WiFi/lwip veut
un pbuf -> malloc cote SYS -> spin interruptions verrouillees -> HWDT ~8 s.
Faits etablis : independant de l'horloge (80/160 testes), AUCUN partenaire
externe requis (appareil nu stable ; page web seule OU visionneur API seul
= crash en 27-65 s -- ton propre trafic TCP fournit les deux cotes).
REMEDE (ALLOCQUIET, generalisable) : les grands constructeurs de messages
sur UN buffer reserve au setup et reutilise, nombres via un appender a
scratch de pile (zero std::to_string, zero temporaire std::string, zero
croissance de vecteur post-setup) ; plus une soupape d'espacement : sous
plancher memoire, UNE soumission lwip par passe (affichage sacrifiable,
uptime non). Resultat terrain : config tueuse + tempete complete, 382 s+
sans reboot la ou la bete tuait en 27-65 s.

## Piege 51 -- Le modem-sleep WiFi a une signature MEMOIRE, pas seulement une latence
Power save actif : pbufs accumules pendant les siestes DTIM + rafales au
reveil -> plongeons periodiques du plus gros bloc contigu. Sur un
gouverneur a causes colorees, ca se lit d'un coup d'oeil (tics "heap").
Banc secteur : `power_save_mode: none`. Batterie : laisser les soupapes
absorber -- mais SAVOIR que la turbulence vient du YAML, pas du code.

## Piege 52 -- Le if sans accolades + une transformation mecanique = l'orphelin
Transformer `if (c) une_expression;` en plusieurs instructions laisse tout
sauf la premiere s'executer INCONDITIONNELLEMENT. Vecu : le JSON gagnait
'true0' (litteral saute, nombre colle) -> parse mort -> panneau squelette.
REGLES : (1) accolader AVANT toute transformation mecanique ; (2) valider
le resultat par RECONSTRUCTION DE SQUELETTE -- litteraux verbatim + '0'
pour chaque nombre -- passe a un vrai parseur JSON, PAR branche de
plateforme (l'ifdef/8266 vs esp32 peut differer).

## Piege 53 -- Le catch muet transforme une corruption de protocole en 'hub mort'
Un `try{parse}catch{return}` silencieux fait disparaitre les trames
malformees sans console ni journal : le symptome devient indistinguable
d'un appareil eteint, et on chasse au mauvais endroit pendant des heures.
REGLE : tout echec de parse PARLE (erreur + premiers 80 octets dans le
journal). Une trame pourrie ne doit jamais pouvoir se deguiser.

## Piege 54 -- L'edition scriptee qui meurt a mi-course a l'air d'avoir marche
Un script d'edition (heredoc/replace) dont un assert echoue APRES certaines
transformations en memoire mais AVANT l'ecriture laisse le fichier intact
en imprimant des messages de succes partiels. Trois occurrences en un jour,
chacune attrapee par une suite en echec, jamais par la lecture du log.
REGLES : ecriture UNIQUE en fin de script apres TOUS les asserts ; verite =
la suite de tests qui repasse, jamais 'ca a imprime' ; en cas d'echec,
imprimer le contexte reel (count + repr de la region) au lieu de mourir muet.

## Piege 55 -- L'economie safe-mode pendant une chasse au crash-loop
Boots de crash ~40 s < seuil de succes 60 s : chaque reproduction INCREMENTE
le compteur ; a 10, verrouillage 300 s OTA-only -- en pleine session de
debug, c'est l'outil qui te confisque l'appareil. Espacer les essais,
connaitre les trois nombres du YAML (60 s / 10 / 300 s), et se rappeler
qu'un flash serie remet les pendules.


## Piege 56 -- Juger un envoi par la taille d'un tampon que le flush mute
Pattern fatal : `avant = buf.size(); envoyer(); if (buf.size()==avant) ->
refuse`. Si l'envoi encode PUIS flushe de maniere synchrone (client local
rapide, ESP32), le tampon retombe a sa taille d'avant : le SUCCES devient
indistinguable du refus. Vecu : la confession de pertes du terminal RAW
re-emise a chaque passe (mur de [lost N B] identiques, bandeau a 630 Mo
pour 25 ko transferes), curseur de lecture gele, ring sature, vraies
pertes en cascade -- la machinerie de verite mentant sur la taille de son
mensonge. REGLE : le verdict d'un envoi appartient a l'ENVOYEUR (retour
bool : encode/refuse), jamais a l'arithmetique d'un tampon partage que le
chemin d'envoi a le droit de vider. Corollaire jumeau du meme jour : deux
producteurs pour un meme ring (l'archeologie d'un navire pre-construit +
une quille forgee de bonne foi) = chaque octet offert deux fois, drop
double de l'offert. Un ring, UN ecrivain -- et un grep de non-regression
sur le nombre de sites d'appel dans le paquet livre.
