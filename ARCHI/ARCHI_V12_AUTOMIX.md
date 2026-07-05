# ARCHI V12-AMX — Automix Dugan (gain sharing)

Date : 2026-07-06 · Priorité 0 de la roadmap V12 (ordre utilisateur).

## Principe (Dan Dugan, standard broadcast/conférence depuis 1975)

Pour les tranches MEMBRES du groupe automix, chaque micro reçoit un
auto-gain proportionnel à sa part de l'énergie totale :

    g_i = (E_i · w_i) / Σ_j (E_j · w_j)

La somme des gains vaut toujours 1 (NOM constant) : pas de pompage, pas
de gate, ouverture/fermeture naturelle des micros à la parole. w_i =
poids par tranche (priorité animateur). L'auto-gain COMPOSE avec le
fader utilisateur — il ne le remplace jamais.

## Intégration mixer-pro (native, zéro NPU/DSP)

| Étape | Où | Coût RT |
|---|---|---|
| Énergie par tranche membre | audio_thread, par bloc : moyenne de x² sur in_block[i] (96 échantillons) | ~N mul/membre/bloc — négligeable |
| Enveloppe | lissage 1er ordre par membre (attack ~10 ms, release ~200 ms — asymétrique parole) | trivial |
| Gains Dugan | par bloc : somme pondérée + division par membre, plancher floor_db (déf. −15 dB) | trivial |
| Application | `ig = input_gain[s] × automix_gain[s]` dans les phases A (sends) ET C (master) de mix_block | 1 mul |
| Slew | automix_gain lissé (resp_ms, déf. 100 ms) dans smooth_gains() | trivial |

État : `automix_on` (global), `automix_member[26]`, `automix_weight[26]`
(dB→lin), `automix_env[26]`, `automix_gain[26]` (lu par la GUI).

## Ops socket

- `set_automix {src, on, weight_db}` — adhésion + poids par tranche
- `set_automix_cfg {on, resp_ms, floor_db}` — global
- `get_automix` → {on, cfg, members[], gains_db[]} (GUI 10 Hz)

## Persistance

mixer_state (bloc « automix ») — même mécanique V9.5.21b (dirty + 1 s).
Reset usine : couvert (purge mixer_state).

## GUI (E2, après validation moteur)

- Tranche : badge/toggle « AMX » (natif + web) + poids dans le drawer
- Affichage du gain auto par tranche : barre fine ambre sous le fader
  (lecture get_automix 10 Hz, pages MIXER seulement — leçon N8)

## Étapes

- **E1 moteur** : état + calcul + application + ops + persistance.
  Tests : (a) 2 tones USB (123 Hz / 1 kHz) sur 2 tranches membres →
  gains ~-3 dB chacun quand les 2 jouent, ~0/-15 dB quand un seul ;
  (b) delta xruns = 0 ; (c) fader utilisateur intact (compose).
- **E2 GUI** : toggles + poids + visu gains.
- V2 (plus tard) : auto-mix « sémantique » NPU (classification des
  sources + politique apprise, pipeline ml-inference existant).

## Invariants

- Ne touche JAMAIS input_target/faders utilisateur — multiplicateur séparé.
- Tranches non-membres : automix_gain ≡ 1 (chemin strictement identique
  à aujourd'hui quand automix_on=0 — un seul mul de plus).
- Blocs de 96 frames à 48 kHz (2 ms) inchangés ; zéro allocation RT.
