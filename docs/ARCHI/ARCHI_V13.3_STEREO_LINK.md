# ARCHI V13.3 — LIEN STÉRÉO de tranches (option A, moteur)

**Objectif** (utilisateur 2026-07-11) : lier deux tranches adjacentes en
paire stéréo. Une paire liée : faders solidaires, dynamique (gate/comp)
solidaire, effets DSP (DRC par canal) et TAC (EQ biquads) solidaires, et
**sends stéréo** (impaire → canal L du bus, paire → canal R) au lieu du
mono-vers-les-deux.

## Périmètre

- Paires candidates : tranches réelles 0-15, paires fixes (2k, 2k+1) —
  alignées sur le matériel (TAC k = CH1/CH2) et l'usage (U1/U2 stéréo PC).
  8 paires, indexées 0-7.
- Les tranches 16+ (P1/P2, returns) ne sont pas liables (P1/P2 sont déjà
  un couple par construction).

## Répartition moteur / GUI

| Aspect | Qui | Comment |
|---|---|---|
| État `link[8]` + persistance | **moteur** | `set_link {pair,on}` / `get_links` ; ligne `links` en fin de mixer_state (fgets, rétro-compatible) → restart + scènes |
| Fader (input_gain), mute | **moteur** | miroir : un `set_input_gain`/`set_mute` sur une tranche liée s'applique aux deux (source de vérité unique, toutes les GUIs héritent) |
| Gate / compresseur | **moteur** | miroir de `set_expander`/`set_comp` (mêmes params sur les deux) |
| Sends F1-F4 | **GUI** | paire liée : impaire→bus L seul, paire→bus R seul (le moteur ne miroir PAS les sends — la matrice reste libre, c'est le pattern d'écriture qui change) |
| Master/routage | **personne** | PAS de miroir (chaque côté garde son routage L/R — pan) |
| EQ TAC (biquads) | **GUI (drawer)** | paire liée : écrit CH1 (BQ1/5/9) ET CH2 (BQ2/6/10) avec les mêmes paramètres |
| DSP DRC par canal | **GUI (drawer)** | paire liée : écrit les deux canaux du blob |
| Automix adhésion/poids | **moteur** | miroir (une paire liée est UNE source) |

## Invariants

1. Le miroir moteur est **idempotent et non récursif** (application
   directe aux deux indices dans le handler, pas de ré-émission d'op).
2. `link` ne modifie AUCUN chemin audio par lui-même — c'est un
   comportement d'écriture. Lier/délier ne change pas le son tant qu'on
   ne touche à rien (pas d'égalisation forcée des gains au moment du
   lien ; le premier geste resynchronise).
3. Rétro-compatibilité : mixer_state sans ligne `links` = tout délié.
4. Les scènes portent l'état de lien (scene_apply parse `links`).

## Diagnostic

- `get_links` → `{"links":[0,0,...]}` ; test miroir : set_input_gain
  sur 2k → get_strip_routing de 2k+1 doit suivre.
- GUI : icône 🔗 entre les deux tranches d'une paire (banques IN DSP /
  IN USB), état partagé LCD/web via get_links.
