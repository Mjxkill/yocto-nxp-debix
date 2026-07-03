# ARCHI V10 — Refonte UI « Console Model AB » (VERSION 3, consolidée)

Date : 2026-07-03 · v1 rejetée (critic cc98ef86), v2 rejetée (critic
f237748f — a aussi trouvé un bug réel corrigé en c859750b), v3 = consolidation
finale intégrant les deux passes + la décision utilisateur : **priorité au
spectaculaire visuel, techno moderne assumée, temps de fiabilisation accepté**.

Direction visuelle VALIDÉE utilisateur (maquette concept-v3) : châssis
anthracite `#14181c`, accent ambre `#e5a13c`, VU aiguilles ⇄ goniomètre
phosphore, spectre + waterfall, knobs rotatifs, faders à course, racks
d'effets en cartes.

## 1. Surfaces

- **S1 Web pro** (PC/tablette) — remplace la GUI Alpine à terme (`/beta`
  d'abord, bascule en P3, ancienne en `/legacy`).
- **S2 Écran local 1024×600** (MIPI-DSI tactile) — kiosk plein écran.
- **S3 App desktop** (Tauri 2) — P5, fallback IP manuelle si mDNS absent.

## 2. Invariants

- Chemin audio RT intouchable (mixer-pro core 2, période 2 ms, protocole
  socket JSON inchangé). Latence < 10 ms.
- Aucun runtime node/npm SUR LA BOARD (le build tourne sur le PC de dev).
- La GUI actuelle reste fonctionnelle jusqu'à la bascule P3.

## 3. Front : Svelte (structure) + canvas/WebGL hors framework (rendu)

Décision (utilisateur + suggestion critic it.1) :
- **Svelte 5 + Vite** : structure, pages, bindings, store. Build **local
  PC**, `dist/` statique **commité** dans meta-local (déploiement scp
  inchangé, reproductible, zéro toolchain board).
- **Rendu temps réel HORS framework** : chaque visualisation est un module
  canvas/WebGL avec son propre requestAnimationFrame, alimenté par le store
  sans passer par la réactivité Svelte (règle : AUCUN re-render framework
  à 30/60 Hz).
- **WebGL vendoré** (`regl` ~90 Ko, vendu dans dist) pour le « wow » :
  waterfall spectral GPU, goniomètre phosphore avec rémanence shader,
  bloom sur les VU, transitions de pages. Fallback canvas 2D automatique
  si WebGL indisponible (WPE mal configuré) — mêmes données, rendu sobre.
- Composants : Knob, Fader, MeterStrip, VUNeedle, Goniometer, Spectrum,
  Waterfall, Toggle, EnumSelect, Card, TabBar. Pages : MIXER, EFFETS
  (rack via meta get_fx), MASTERING, ROUTING (matrice 26×18), SYSTÈME.

## 4. Backend : `/api/state` SSE versionné + POST (pas de WebSocket)

Acté aux itérations 1-2 (MHD_upgrade = framing RFC6455 manuel, risque
injustifié ; SSE éprouvé ; commandes en POST déjà fiables) :

- **Flux `/api/state`** : full state `{schema_version, seq, state}` à la
  connexion ; deltas `{seq, patch}` ensuite ; **full state re-poussé sur
  tout changement structurel** (set_insert/set_fx_engine) — élimine les
  patchs sur structures disparues.
- **Architecture serveur** (flaws qwen it.2 intégrés) :
  - UN **thread producteur** unique construit trames meters + state ;
    les callbacks SSE ne font que consommer (pas de usleep ×N clients,
    pas de mixer_request par client).
  - Buffers **par connexion** (le bug du buffer static partagé est déjà
    corrigé en c859750b).
  - Limite clients SSE (6) → 503 + Retry-After ; `/api/debug`
    (clients/deltas/resyncs/latence p95).
- **Miroir d'état** (assumé, borné) : aucune logique métier ni validation
  (mixer-pro seul juge — l'écho POST est une MAJ optimiste, corrigée par
  re-poll). Re-poll différencié : données chaudes (insert params ML,
  meters déjà à 30 Hz) à **5 Hz** — la fréquence de l'actuel pollAssistant,
  donc charge identique à aujourd'hui et latence ≤ 200 ms (répond au flaw
  « 1 Hz inacceptable ») ; données froides (routing, liste plugins) à 1 Hz
  + re-poll immédiat après chaque POST écho.
- Meters : **JSON actuel conservé** (mesuré suffisant) ; toute optimisation
  future exige une mesure préalable.
- sd_notify watchdog systemd sur mixer-gui-http ET mixer-pro.

## 5. Écran local (S2) — kiosk WPE, critères durcis

- Weston + cog (WPE WebKit) → `http://localhost:8080/panel`.
- cgroup `memory.max=400M` (revu : 300M jugé irréaliste pour canvas
  intensif — flaw qwen), `cpu.weight` bas, cores 0-1.
- **Watchdog préventif** (flaw « safe mode trop tardif ») : surveillance
  RSS kiosk + stalls capture ; si RSS > 350M OU 1 stall attribuable →
  bascule safe-mode (UI minimale meters+mute) AVANT l'échec perçu.
- Mesures GO/NO-GO avant adoption : RSS massif sous rendu COMPLET
  (spectrum+waterfall+26 meters+VU), stalls 60 s ×3, page /panel seule
  (pas de double flux legacy pendant la mesure).
- Plan B chiffré : LVGL (≈3× l'effort de la page panel), décision sur
  mesures uniquement.

## 6. Scènes

Nouvelle op mixer-pro `flush_state` (écriture immédiate de mixer_state,
hors debounce 1 s). `save_scene` = flush_state → copie nommée ;
`load_scene` = restauration + re-push full state SSE. Pas de race avec
g_presets_dirty.

## 7. Phasage

- **P0** : squelette Svelte+Vite, tokens, composants (base = maquette
  concept-v3), page MIXER statique servie en `/beta`.
- **P1** : `/api/state` SSE versionné (thread producteur) + store client
  (seq/resync) ; MIXER branché réel ; smoke-test déploiement étendu.
- **P2** : EFFETS (meta get_fx) + MASTERING + visualisations WebGL
  (waterfall/gonio/bloom, fallback 2D).
- **P3** : ROUTING + SYSTÈME + scènes ; bascule `/` ↔ `/legacy`.
- **P4** : kiosk S2 (critères §5).
- **P5** : Tauri S3.

Chaque phase : build + smoke-test + validation board + fiche TESTS.

## 8. Risques résiduels assumés

- Svelte/Vite = dette de toolchain côté PC (versions lockées,
  `package-lock.json` commité) — assumé par l'utilisateur (wow > minimalisme).
- WebGL sur WPE/Vivante à valider tôt (P2 : démo waterfall sur board avant
  généralisation) ; fallback 2D systématique.
- Le miroir 5 Hz laisse ≤ 200 ms de latence sur les changements initiés
  hors UI — jugé acceptable (identique à l'existant).
