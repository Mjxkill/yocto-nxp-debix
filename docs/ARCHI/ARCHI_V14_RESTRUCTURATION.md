# ARCHI V14 — Restructuration en modules à API réutilisables

Date : 2026-07-28. Statut : **TERMINÉ 2026-08-02** — 8 étapes exécutées,
validées board (fiches TESTS_V14.0_ETAPE0→8). Restes : arbitrage
beta_fx.js (closure 1125 l., fiche étape 7), passe 2 (re-privatisation
des externs), écoute globale utilisateur. Décision tac5212.c : **exception assumée** (convention kernel
mono-fichier par codec, facilite les diffs avec un driver upstream).

Règle produit (fixée par Michael, gravée skill methode-dev §5) :
> « je ne veux pas de code > 1000 lignes… je veux des API réutilisables !
> chaque fichier expose ses API pour une fonction donnée… fonctions groupées. »

- **Limite dure : 1000 lignes par fichier**, tout langage (C, JS, QML).
- **Un fichier = une fonction du produit**, avec un `.h` qui expose des
  fonctions groupées à préfixe commun (`automix_*`, `smp_*`, …), état interne
  privé au module.
- Méthode : **extraction pure** (déplacer, pas réécrire), un module à la fois,
  build + test board + commit à chaque pas — même discipline que le lot 5b.

## 1. État des lieux — fichiers hors limite

| Fichier | Lignes | Facteur de dépassement |
|---|---:|---|
| mixer-pro.c | 7 008 | ×7 |
| index.html | 4 227 | ×4,2 (JS inline) |
| beta.html | 4 100 | ×4,1 (JS inline : 3 240 lignes) |
| effects.c | 2 389 | ×2,4 |
| mixer-gui-http.c | 1 654 | ×1,7 |
| tac5212.c | 1 281 | ×1,3 — **driver kernel, exception à trancher §8** |
| StripFxDrawer.qml | 1 093 | ×1,1 |

Cause racine du monolithe : chaque fonctionnalité (sampler, looper, bandmix,
vfocus…) a été **empilée dans mixer-pro.c** au lieu de naître dans son module.
La règle vaut donc aussi pour l'avenir : toute nouvelle fonctionnalité = un
nouveau module avec son API.

## 2. Découpage de mixer-pro.c (7 008 → 13 modules)

Cartographie mesurée des sections actuelles (lignes réelles) :

| Section actuelle | Lignes | → Module cible |
|---|---:|---|
| State + structs globales | ~340 | `state.h` |
| UAC2 isolation + rings | ~830 | `uac2_ring.c/h` |
| Logging + ALSA helpers | ~90 | `util.c/h` |
| Sampleur V12-SMP | ~185 | `sampler.c/h` |
| Looper + lien stéréo | ~290 | `looper.c/h` |
| Expandeur + compresseur natif | ~215 | `strip_dyn.c/h` |
| BANDMIX complet (rôles, balance quadrants, gate auto, solo, placement EQ, master EQ+LUFS, tick 1 Hz, compute mix) | ~840 | `automix.c/h` + `master.c/h` |
| VFOCUS + VSPAT | ~240 | `voice.c/h` |
| MIDIX | ~285 | `midix.c/h` |
| Audio loop RT + play DSP | ~600 | `audio_loop.c/h` |
| **Control socket (ops)** | **~2 200** | `control.c/h` + ops éclatées §3 |
| Persistance + scènes + parse_state_lines | ~600 | `persist.c/h` |
| Biquads RBJ (rbj_peak_core, shelfs, hpf) | ~150 | `dsp_bq.c/h` |
| Signals + main + init | ~450 | `mixer-pro.c` (reste) |

Chaque module : préfixe commun, API dans son `.h`
(`automix_tick_1hz()`, `automix_compute_mix()`, `smp_trigger()`,
`uac2_ring_try_push_period()`, `persist_save()`, `bq_peak()`, …).

**Passe 1 (ce plan)** : découpage mécanique — les structs partagées entre
threads (`g_st`, `g_bmx`, `g_vf`, …) vont dans `state.h` avec leurs `extern`
documentés (qui écrit, qui lit, sous quel lock). Risque quasi nul : le binaire
produit est le même code, réorganisé.
**Passe 2 (optionnelle, après validation)** : resserrer — rendre l'état privé
aux modules là où c'est possible sans toucher au chemin RT.

## 3. Le control socket (2 200 lignes) — ops distribuées aux modules

Le plus gros bloc. Principe : `control.c` ne garde que l'accept/parse/dispatch
(~300 lignes) ; chaque module expose `int <mod>_op(const char *op, char *args,
char *reply, size_t n)` et **possède ses propres ops** (les ops automix vivent
dans `automix.c`, les ops sampler dans `sampler.c`, …). Une table de dispatch
statique dans `control.c` route vers les modules.

Bénéfices : chaque fichier reste sous 1000 lignes ; ajouter une op d'un module
= toucher CE module uniquement ; la GUI et la doc MIXER_PRO_REFERENCE.md se
mappent 1:1 sur les modules.

## 4. Découpage de effects.c (2 389 → répertoire effects/)

| Module | Contenu | Lignes est. |
|---|---|---:|
| `fx_dynamics.c` | compressor + limiter natif | ~350 |
| `fx_space.c` | reverb Schroeder + delay | ~230 |
| `fx_eq.c` | EQ 3 bandes + para_eq_x16 + spectral env FIR + exciter | ~700 |
| `lv2_host.c` | host lilv : instanciation, ports, run | ~700 |
| `lv2_meta.c` | métadonnées UI V9.5.21 (groupes, unités, types) | ~400 |
| `fx_chain.c` | cascade V9.4 + passthrough + API commune `fx_*` | ~250 |
| `effects.h` | API publique inchangée (les appelants ne bougent pas) | 107 |

## 5. Découpage de mixer-gui-http.c (1 654 → 5 modules)

| Module | Contenu | Lignes est. |
|---|---|---:|
| `http_core.c` | main, MHD, send_text/json/file, fichiers statiques | ~350 |
| `sse_state.c` | producteur d'état versionné + SSE + sysload | ~400 |
| `api_routes.c` | on_request : routage + POST | ~550 |
| `alsa_ctl.c` | amixer helpers + SOF TLV bytes + alsa_store | ~350 |
| `mixer_bridge.c` | socket vers mixer-pro | ~100 |

## 6. GUI web : sortir le JS/CSS des HTML

`beta.html` = 390 lignes CSS + 470 lignes HTML + **3 240 lignes JS inline**.
Découpage en fichiers **locaux, servis par mixer-gui-http** (zéro CDN préservé,
même origine, rien d'externe) :

- `beta.html` (~500) — structure seule, `<link>` + `<script src>` locaux
- `beta.css` (~400)
- `js/core.js` (~500) — état, SSE, réseau, helpers
- `js/strips.js` (~900) — tranches, faders, VU
- `js/automix.js` (~900) — page AUTO MIX, balance, réglages
- `js/fx.js` (~950) — effets, insert LV2, mastering

Même traitement pour `index.html` (4 227). Les deux pages partagent `core.js`
(factorisation inter-pages en bonus).

## 7. QML : StripFxDrawer.qml (1 093)

Extraction de 2–3 sous-composants (`FxCompSection.qml`, `FxEqSection.qml`, …)
→ tous les fichiers QML < 1000. Petit chantier, dernier de la liste.

## 8. Points de décision (à trancher par Michael)

1. **tac5212.c (1 281, driver kernel)** : la convention amont kernel est le
   mono-fichier par codec (les drivers upstream font 1 000–4 000 lignes).
   **TRANCHÉ 2026-07-28 : exception assumée** — s'aligner sur la convention
   amont facilite tout diff futur avec un driver upstream.
2. **`fx_eq.c` à ~700 et `strips.js` à ~900** : proches de la limite — si une
   évolution les fait dépasser, on re-découpe à ce moment-là (la règle est
   dure, pas anticipatoire).
3. **Passe 2 d'encapsulation** (état privé par module) : après validation
   complète de la passe 1, ou jamais si la passe 1 suffit.

## 9. Ordre d'exécution — un module = un commit + test board

Du risque le plus faible au plus fort ; chaque étape laisse un binaire
fonctionnel identique :

| Étape | Chantier | Risque |
|---|---|---|
| 0 | `state.h` + `util.c` + `dsp_bq.c` + Makefile/recette multi-fichiers | nul (fondations) |
| 1 | Modules froids : `persist.c`, `sampler.c`, `looper.c`, `midix.c` | faible |
| 2 | `strip_dyn.c`, `voice.c`, `automix.c`, `master.c` | moyen (chemin RT, extraction pure) |
| 3 | `uac2_ring.c`, `audio_loop.c` | moyen+ (cœur RT) |
| 4 | `control.c` + ops distribuées | moyen (gros volume, mécanique) |
| 5 | effects/ (6 modules) | faible (API effects.h inchangée) |
| 6 | mixer-gui-http (5 modules) | faible |
| 7 | beta/index → HTML+CSS+JS séparés | faible (servir des fichiers locaux) |
| 8 | StripFxDrawer.qml | faible |

**Validation à chaque étape** : build 0 warning nouveau → md5 → scp → batterie
courte (restart round-trip persistance, un morceau dataset, delta xruns = 0,
`get_drift` propre) → commit. Fiche finale : `TESTS/TESTS_V14_RESTRUCTURATION.md`.
`MIXER_VERSION` bumpé (v14.0-modules) à la première étape qui touche le moteur.

**Interdits pendant le chantier** : aucune modification de comportement, aucun
renommage d'op, aucune « amélioration au passage ». Toute envie d'amélioration
va dans une liste pour après.

## 10. Architecture détaillée — étape 2 (modules RT) + placement de persist

Rédigée 2026-07-28 après l'étape 1 (sampler/looper/midix extraits). Chaque
module suit le patron validé à l'étape 1 : `.h` = doc du rôle + API en
fonctions groupées + externs temporaires pour les ops (qui migreront à
l'étape 4), `.c` = code déplacé tel quel.

### 10.1 `strip_dyn.c/h` — dynamique par tranche + lien stéréo (~520 l.)

Contenu : expandeur/gate V12-EXP (`g_exp`, `exp_render`, config), compresseur
natif V13-COMP (`g_cmp`, `cmp_render`, `cmp_configure`), lien stéréo V13.3
(`g_link`, `link_partner`) — le lien vit ici car il miroite précisément les
écritures fader/mute/gate/comp.
API : `exp_render(in_block)`, `cmp_render(in_block)`,
`cmp_configure(src, on, thr, ratio, atk, rel, mk)`, `link_partner(src)`
(inline .h, appelé par les handlers socket).
Invariant RT : render appelés par l'audio_thread sous target_lock, états
jamais vidés en live.

### 10.2 `automix.c/h` — AUTOMIX LIVE complet (~950 l.)

Contenu : `g_bmx` (rôles, parts de réf, balance quadrants, gate auto, solo
v2, staging, risk), `automix_update` (Dugan par bloc), `bmx_tick` (1 Hz :
soundcheck/lock/keeper/balance), le calcul du mix (staging+gate+comp+faders),
l'EQ de placement par rôle (`g_eqx`, `eqx_config`, `eqx_render` — lit
`g_bmx.role`, donc même module).
API : `automix_update(in_block, N)`, `bmx_tick_1hz()`, `bmx_compute_mix(...)`,
`eqx_config(i, role)`, `eqx_render(in_block)` + `extern g_bmx`/`g_eqx`
(ops + persist, temporaire).
Invariants gravés en tête de .h : pas de signal → aucun gain ne bouge
(gated act[]) ; un reset n'écrase jamais un réglage opérateur ; gel des
montées si prog < crête−3 dB.

### 10.3 `master.c/h` — bus master (~430 l.)

Contenu : EQ mastering 3 bandes (`g_meq_p`, `g_meq_bank`, double-banque +
crossfade), makeup LUFS BS.1770-4 (coeffs UIT exacts — ne pas recalculer),
limiteur −1 dBFS. API : `meq_recalc()`, `meq_render(...)`, `save_master_eq()`,
`master_makeup_tick()` + `extern g_meq_p` (ops + persist).

### 10.4 `voice.c/h` — traitement voix (~250 l.)

Contenu : vfocus V13 (`g_vf`, analyse bandes + cuts RBJ par bloc) +
spatializer V13.9 (`g_vspat`, widener Lauridsen). Les deux sont « la voix »
et partagent la sélection de la tranche lead. API : `vf_render(...)`,
`vf_set(...)`, `vspat_render(...)` + externs temporaires.

### 10.5 `persist.c/h` — sérialisation d'état + scènes (~600 l.)

Contenu : `save_state_to`, `parse_state_lines` (parseur commun lot 5b),
`load_mixer_state`, scènes V13 (apply/save sans coupure), presets_dirty.
**Placement : EN FIN d'étape 2** (pas étape 1 comme prévu initialement) —
persist lit/écrit les structs `g_bmx`/`g_vf`/`g_vspat`/`g_meq_p` : les
extraire avant obligerait à déménager ces types DEUX fois (mixer-pro.c →
state.h → automix.h/…). Une fois automix/master/voice en place, persist
inclut leurs .h et se déplace en une passe. Aucun changement de périmètre,
seulement d'ordre.

### 10.6 Ce qui reste dans mixer-pro.c après l'étape 2

State/init/main, UAC2+ASRC (étape 3 : `uac2_ring.c`), audio_thread +
play_thread (étape 3 : `audio_loop.c`), control socket (étape 4), insert
LV2 chain + divers (rejoindront control/audio_loop selon leur nature à
l'étape 3/4). Cible post-étape 4 : mixer-pro.c ≈ 450 lignes.
