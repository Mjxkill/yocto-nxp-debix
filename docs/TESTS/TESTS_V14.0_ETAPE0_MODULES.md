# TESTS V14.0 — Étape 0 restructuration : fondations modules

Date : 2026-07-28. Architecture : `docs/ARCHI/ARCHI_V14_RESTRUCTURATION.md`
(validée GO utilisateur). Règle appliquée : pas de fichier > 1000 lignes,
un fichier = une fonction produit avec API en `.h` (fonctions groupées).

## Ce qui a été fait (extraction pure — code déplacé tel quel, zéro renommage)

| Nouveau fichier | Lignes | Contenu extrait de mixer-pro.c |
|---|---:|---|
| `state.h` | 139 | `struct alsa_pcm` + `struct mixer_state` + asserts + `extern g_st` (définition de g_st reste dans mixer-pro.c) |
| `util.c` / `util.h` | 83+40 | `mlog`, `pcm_open`, `pcm_recover` ; `s32_to_f`/`f_to_s32` en inline dans le .h (chemin RT) |
| `dsp_bq.c` / `dsp_bq.h` | 65+36 | `struct eqx_bq` + designers RBJ : `eqx_hpf`, `rbj_peak_core`, `eqx_peak`, `meq_shelf` (fonctions pures, zéro état global) |

`mixer-pro.c` : 7 008 → **6 760** lignes. Makefile + SRC_URI recette mis à
jour. `MIXER_VERSION` → `v14.0-modules`. Décision actée : `tac5212.c` =
exception assumée (convention kernel mono-fichier par codec).

## Validation board (binaire md5 `08d8e0fc8b20…`, remplace `85fe26996d4e…`)

| Test | Chemin prouvé | Résultat |
|---|---|---|
| Build bitbake | 2720 tâches, 0 erreur, **0 warning nouveau** (warnings ioctl/write préexistants — vérifiés présents dans HEAD) | ✓ |
| Boot service | `v14.0-modules` actif, mixer-ml-inference actif, frames avancent | ✓ |
| T1 : gate_db 19 → restart → moteur relit 19 (set partiel freeze_db renvoie gate_db 19.0) → restore 15 | boot → `parse_state_lines` → état moteur | ✓ |
| T2 : master_eq mid −2.5 appliqué puis −3.0 (nominal) | `meq_compute` → `eqx_peak`/`meq_shelf` (dsp_bq.c) | ✓ |
| T3 : vfocus ON conservé au restart, `active=0` cuts 0.00 sans signal | invariant « pas de signal → rien ne bouge » | ✓ |
| T4 : morceau KatWright 60 s : cuts vfocus vivants (1.74/1.69/1.58/0.74/0.47 dB pendant chant), **delta xrun = 0** (16→16, comptés depuis boot), drift −25 ppm corrections 1122/1122 + 518/518, fills 192/192, wr/rd avg 1999 µs | `vf_peak_coefs` → `rbj_peak_core` en RT + stabilité globale | ✓ |

## Test utilisateur : EN ATTENTE (écoute — extractions mathématiquement
identiques, l'oreille reste le juge final)

## Reste (ARCHI V14, ordre validé)

Étape 1 : modules froids (`persist.c`, `sampler.c`, `looper.c`, `midix.c`) —
puis 2 : `strip_dyn.c`/`voice.c`/`automix.c`/`master.c` — 3 : `uac2_ring.c`/
`audio_loop.c` — 4 : `control.c` + ops distribuées — 5 : effects/ — 6 :
mixer-gui-http — 7 : GUI web JS/CSS — 8 : QML.
