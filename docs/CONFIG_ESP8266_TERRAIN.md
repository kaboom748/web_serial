# web_serial -- Contraintes YAML ESP8266, prouvees au banc
### (session forensique 2026-07-23 : 6 crashs instruits, 3 mecanismes distincts, tous clos)

Chaque entree = la contrainte, SA SIGNATURE OBSERVABLE dans l'interface, et
l'arbitrage. Le hub expose expres les variables cachees (CPU MHz, causes de
throttle colorees, causes de drop) pour que ces signatures se lisent a l'oeil.

## wifi: power_save_mode
- **Effet mesure** : modem-sleep = la radio dort entre les beacons DTIM ->
  nos pbufs sortants s'accumulent pendant les siestes + rafales entrantes au
  reveil -> plongeons periodiques du largest sous 7 680.
- **Signature** : TICS BLEUS dans le chronogramme GOVERNOR (cause heap).
  Power save off = bandeau sans bleu. Diagnostic d'un toggle YAML par
  graphique -- verifie au banc dans les deux sens.
- **Arbitrage** : banc sur secteur -> `power_save_mode: none` (turbulence
  supprimee a la source). Batterie -> actif, les soupapes absorbent
  (prouve : largest remonte seul, `drop causes heap = 0`).

## board_build.f_cpu (80 vs 160 MHz)
- 160 MHz **exonere** comme cause du wedge HWDT (teste 80 = identique).
- Le binaire ne dit RIEN de l'horloge reelle : le champ `CPU` du panneau
  (mhz runtime) est la seule verite. Deux hubs identiques aux Max pass du
  simple au double = ce champ. Charge web_serial a 160 : 3-8 % moteur --
  80 MHz reste luxueux pour ce composant.

## api: (Noise) + le visionneur ESPHome
- `reboot_timeout: 0s` obligatoire hors Home Assistant (sinon reboot cyclique).
- Chaque (re)connexion Noise = grosses allocations cote SYS. Le visionneur
  qui flappe fut un suspect de collision ; verdict final : PAS necessaire au
  crash (l'appareil nu etait stable, NOTRE trafic suffisait), mais c'est un
  amplificateur de churn. Pour un soak "pur", capturer au CH340 plutot.
- **Angle mort structurel** : les logs de setup() partent AVANT le wifi --
  le visionneur OTA ne les verra JAMAIS. D'ou les salves BBSHOTS (rapport
  boite noire re-emis a 15/25/35 s) et le miroir logger de bb_push_.

## captive_portal / web_server (annotations du capitaine, confirmees)
- "Fait planter web_serial prend trop de ram (LOOP)" -- cohabitation RAM
  impossible sur 8266. Ne jamais co-activer avec web_serial.

## logger: hardware_uart: UART1
- Libere UART0 pour le bus ; logs sur GPIO2 TX-only.
- Consequence forensique : l'adaptateur de flash (GPIO1/3) ne voit PAS les
  logs applicatifs -- mais il voit le banner ROM et le dump HWDT a 74880
  (cristal 26 MHz). Voir Piege 42 : bring-up sur GPIO1/3, production en
  swap GPIO15/13 + boite noire.

## build_flags: -DDEBUG_ESP_HWDT (garde-le commente, arme a la demande)
- L'instrument DECISIF du wedge Level1Int : au prochain HWDT, une VRAIE
  pile du contexte gele (ctx sys) au lieu du scan heuristique qui sert des
  fantomes (les trames dns du backtrace = bruit de scan, aucun composant
  ne resolvait de DNS).

## safe_mode (l'economie des reproductions)
- Seuil "Successful after: 60s" vs boots de crash-loop ~40 s : le compteur
  MONTE. 10 tentatives = verrouillage 300 s, OTA-only. En chasse au crash :
  ESPACER les reproductions, surveiller le compteur.

## Composants resolveurs de DNS (time/sntp, mqtt)
- Absents de ce YAML -- c'est ce qui a permis d'ecarter les trames dns du
  scan comme bruit. Si un jour ajoutes sur 8266 : serveurs EN IP
  (ex. sntp servers: [x.x.x.x]) pour eviter le resolveur en contexte SYS.

## esp8266: framework: version
- La famille de collision umm/lwip en contexte SYS vit en terre core/SDK.
  ALLOCQUIET ferme NOTRE cote quelle que soit la version ; si la bete
  remordait un jour : `version: latest` (ou epinglage) = le levier suivant.

## L'OIGNON DES DEFENSES TAS (qui tire en premier, qui dort)
| Seuil largest | Couche | Effet |
|---|---|---|
| < 7 680 | Gouverneur (tic BLEU) | quota /2 -- on ralentit notre egresse |
| < 6 144 | Soupape de cohabitation (capitaine) | 1 soumission lwip/passe, info 3,5 s |
| < 5 120 | Frein radio (dur) | egresse bridee ferme |
| < 1 536 | Garde du tap | trames sacrifiees (drop cause heap) |
Sante = les couches EXTERNES travaillent, les profondes dorment
(capture de reference : bleus + `drop causes heap = 0`).
