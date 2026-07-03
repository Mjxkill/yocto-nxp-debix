# ARCHI V10 — Refonte UI « Console Model AB » (3 surfaces, 1 backend)

Date : 2026-07-03 · Direction visuelle validée par l'utilisateur sur maquette
(artifact concept-v1 : châssis anthracite, accent ambre, VU aiguilles, knobs
rotatifs, faders à course, meters canvas). Le même design system s'applique
aux 3 surfaces, **y compris les sous-menus effets**.

## 1. Objectif

Remplacer l'interface Alpine.js monolithique (index.html ~4 500 lignes,
sliders HTML natifs) par une interface « console hardware » pro, fiable,
déclinée sur :
- **S1 — Web pro** : PC/tablette du réseau (remplace la GUI actuelle)
- **S2 — Écran local 1024×600** : MIPI-DSI tactile de la Debix (table de
  mixage complète autonome)
- **S3 — App extérieure** : application native desktop (Tauri) — contrôle
  distant + scènes/presets + enregistrement

## 2. Invariants (non négociables)

- mixer-pro (RT99) **inchangé** dans son chemin audio : période 2 ms,
  chaîne native, protocole socket JSON conservé tel quel (compat GUI
  actuelle pendant la transition).
- Latence audio < 10 ms : rien de l'UI ne tourne sur le core 2 ; l'écran
  local (kiosk) tourne sur cores 0-1 avec nice > 0 et cgroup mémoire.
- Le daemon ML et son push 100 Hz ne changent pas.
- Déploiement = fichiers statiques + un binaire ; pas de node/npm sur la
  board à l'exécution.

## 3. Backend temps réel : `mixer-api` (WebSocket)

Extension de mixer-gui-http (même binaire, libmicrohttpd + upgrade WS) :

- **État versionné** : un document JSON unique `state` (schema_version,
  seq) = fusion de get_state/get_insert/get_assistant/get_input_map/
  get_output_gain/get_fx×4 + liste plugins. Poussé complet à la connexion,
  puis **deltas** `{seq, path, value}` à chaque changement.
- **Meters binaires** : trame binaire compacte 30 Hz (26+18+8 peaks u8 +
  spec 128 u8 × taps actifs + VU master) — plus de JSON pour le flux chaud.
- **Commandes** : mêmes ops JSON qu'aujourd'hui, transportées par le WS,
  réponse corrélée par id. mixer-api reste un proxy → socket mixer-pro
  (aucune logique métier dupliquée).
- **Fiabilité** : heartbeat 2 s ; client : reconnexion expo + resync full
  state sur écart de seq ; serveur : sd_notify watchdog systemd.
- SSE/endpoints actuels conservés le temps de la migration, retirés en fin
  de V10.

## 4. Front unique : `mixer-ui` (Svelte, compilé statique)

Un seul code, deux cibles de build :
- `dist/desktop/` → servi par mixer-gui-http (S1) et embarqué dans Tauri (S3)
- `dist/panel/` → layout 1024×600 fixe, cibles tactiles ≥ 40 px (S2)

**Design system** (tokens de la maquette) : `--chassis #14181c`,
`--panel #1a2025`, `--well #0f1316`, texte blanc chaud `#e9e5da`, accent
unique ambre `#e5a13c`, sémantique meters vert/ambre/rouge distincte de
l'accent, mono tabulaire pour toute valeur numérique, labels capitales
espacées.

**Composants** (bibliothèque interne, pas de dépendance UI externe) :
Knob (SVG, drag vertical + molette + double-tap reset), Fader (course
réelle, dB), MeterStrip (canvas segments + peak hold), VUNeedle (canvas,
ballistique 300 ms), Spectrum (canvas, bins log + courbe enveloppe ML),
Toggle, EnumSelect, Card, TabBar.

**Pages** (les « sous-menus », même look) :
1. **MIXER** — 8 tranches + master (la maquette)
2. **EFFETS** — sélecteur bus FX (4) ; par bus : navigateur de plugins par
   catégorie (réutilise cat/ins), puis **rack du plugin** : cartes par
   groupe (meta grp/bande), knobs pour les continus, toggles, enums —
   remplace les listes de sliders actuelles en réutilisant le meta
   (label/kind/unit/sp) déjà exposé par get_fx.
3. **MASTERING** — panel assistant : enveloppe 64 pts, FFT IN/OUT, dials
   exciter/limiteur, M/A par étage, gate.
4. **ROUTING** — matrice 26×18 (grille tactile), remap mics, gains sortie.
5. **SYSTÈME** — charges, versions, scènes (sauvegarde/rappel de
   mixer_state nommés), maintenance (tac-reset, logs).

## 5. Écran local (S2)

Weston kiosk + **cog (WPE WebKit)** pointé sur http://localhost:8080/panel.
Recettes Yocto : weston déjà présent, ajouter cog/wpewebkit (meta-
openembedded). Budget mesurable avant validation : RAM < 350 Mo,
0 xrun ajouté sur 10 min de lecture (critère GO/NO-GO). Plan B documenté :
LVGL natif si NO-GO.

## 6. App extérieure (S3)

Tauri 2 (Rust + WebView système) : découverte mDNS (`_mixerab._tcp`,
avahi côté board), même front, plus : gestionnaire de scènes (fichiers
mixer_state nommés via nouvelles ops save_scene/load_scene), enregistrement
8 pistes USB côté PC (WASAPI/CoreAudio/ALSA), A/B mastering. Hors périmètre
du sprint 1.

## 7. Phasage

- **P0** : tokens + bibliothèque composants + page MIXER servie en
  `/beta` (S1) — l'actuelle GUI reste la référence.
- **P1** : mixer-api WS (état versionné + meters binaires) ; page MIXER
  branchée au réel ; smoke-test déploiement.
- **P2** : pages EFFETS + MASTERING (le meta get_fx est déjà prêt).
- **P3** : ROUTING + SYSTÈME + scènes ; bascule `/` → nouvelle UI,
  ancienne en `/legacy`.
- **P4** : S2 kiosk 1024×600 + mesures GO/NO-GO.
- **P5** : S3 Tauri + mDNS.

Chaque phase : build + smoke-test + validation board + fiche TESTS.

## 8. Risques & diagnostics

- Kiosk vs RT : mesurer stalls capture (méthode 60 s validée) avec kiosk
  actif AVANT d'adopter ; sinon LVGL.
- WS dans mixer-gui-http : MHD upgrade websocket — si trop fragile,
  fallback SSE amélioré (état versionné garde tout son intérêt).
- Svelte : build PC uniquement (dist commité ou artefact de recette) —
  jamais de toolchain node sur la board.
- Ne pas casser la GUI actuelle pendant P0-P3 (`/beta` parallèle).


---

# RÉVISION 2 (post-critic cc98ef86 — 2 workers, verdict initial NON approuvé)

Le critic a identifié 6 contradictions fondées. Décisions de révision — le fil
conducteur est LA SIMPLIFICATION (chaque point retiré = une classe de bugs en
moins) :

## R1 — ABANDON du WebSocket → SSE versionné + POST (inchangés)
Le critic a raison deux fois : (a) MHD_upgrade ne fournit qu'un socket TCP
brut, le framing RFC6455 (masking/ping/fragmentation) serait à écrire à la
main = risque injustifié ; (b) le SSE actuel est éprouvé et répond au besoin
30 Hz ; les commandes passent déjà très bien en POST /api/cmd.
→ mixer-api = le mixer-gui-http actuel + un flux SSE `/api/state` :
  - à la connexion : full state {schema_version, seq, state}
  - ensuite : deltas {seq, patch} ; sur CHANGEMENT STRUCTUREL (set_insert,
    set_fx_engine) : re-push full state (règle simple → pas de paths
    invalides, répond au flaw "plugin change de structure")
  - client : si seq reçu != seq attendu → resync (GET full state)
  - limite : 4 clients SSE max, 503 + Retry-After au-delà (load shedding)
  - un endpoint /api/debug : nb clients, deltas, resyncs (observabilité)

## R2 — ABANDON des meters binaires → JSON actuel conservé
Le critic a raison : rien ne prouve que le JSON get_meters (≈2 Ko à 30 Hz
sur LAN) soit un problème. On garde. Optimisation UNIQUEMENT si mesure.

## R3 — ABANDON de Svelte → vanilla ES modules + Web Components, ZÉRO build
Résout la contradiction "pas de node sur la board" (il n'y a plus de node
NULLE PART), cohérent avec le déploiement actuel (scp de fichiers), et
répond au flaw canvas : chaque widget (Knob, Fader, MeterStrip, VUNeedle,
Spectrum) = un Web Component vanilla avec son requestAnimationFrame interne,
AUCUN framework dans la boucle 30 Hz. Structure :
  www/ui/{tokens.css, components/*.js, pages/*.js, store.js (état+seq+resync),
  api.js (SSE+POST)} — modules ES natifs, pas de bundler.
La maquette validée est déjà du vanilla : elle DEVIENT la base du code.
(Réponse à "pourquoi pas modulariser l'Alpine existant" : c'est exactement
ça — on garde l'approche sans-build, on remplace le monolithe par des
modules, et le design imposait de réécrire les widgets de toute façon.)

## R4 — mixer-api assume un MIROIR d'état (clarification H1)
Le critic a raison : tenir un état versionné n'est pas un proxy pur. On
l'assume et on le borne : mixer-api tient un miroir (agrégat des get_*),
AUCUNE logique métier (pas de décision audio, pas de validation des valeurs
— mixer-pro reste seul juge). Le miroir se met à jour : par écho des
commandes POST qui transitent, et par re-poll get_* à 1 Hz (rattrape toute
divergence, y compris le daemon ML qui pousse en direct).

## R5 — Kiosk : critères durcis + safe mode (suggestions critic adoptées)
- cgroup memory.max=300M + cpu weight bas, kill propre avant OOM kernel
- mesure RSS réelle (massif) + stalls capture 60 s×3 AVANT adoption
- test SANS le SSE legacy en parallèle (le critic note que doubler les flux
  pendant la mesure fausse le verdict) — la page /panel n'utilise QUE le
  nouveau flux
- SAFE MODE : si 3 stalls/60 s détectés (lecture /api/debug), l'UI panel
  bascule seule en mode minimal (meters+mute, pas de spectre) — plan B
  intermédiaire avant LVGL
- Plan B LVGL chiffré : ~3× l'effort de la page panel, décision uniquement
  sur mesures

## R6 — Scènes : flush synchrone
save_scene force d'abord un flush de la persistance mixer-pro (nouvelle op
flush_state qui écrit mixer_state immédiatement, hors debounce 1 s) puis
copie le fichier. Pas de race avec g_presets_dirty.

## R7 — Divers actés
- Formats de réponse hétérogènes mixer-pro : normalisés dans le miroir (le
  front ne voit QUE le state du flux SSE).
- Tauri/mDNS OS-dépendant : P5, fallback saisie IP + scan subnet.
- Le cache /api/sysload 500 ms est retiré au profit du state versionné
  (source de vérité unique — le critic a relevé la divergence).

## Phasage révisé
P0 : tokens+composants (base = maquette) + page MIXER statique /beta
P1 : /api/state SSE versionné + store client + MIXER branché réel
P2 : EFFETS (meta get_fx) + MASTERING
P3 : ROUTING + SYSTÈME + scènes (flush_state) + bascule / ↔ /legacy
P4 : kiosk 1024×600 (critères R5)
P5 : Tauri
