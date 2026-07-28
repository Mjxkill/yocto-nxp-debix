# TESTS V14.0 — Étape 2 restructuration : modules RT + persistance

Date : 2026-07-29. Architecture : `docs/ARCHI/ARCHI_V14_RESTRUCTURATION.md`
§10 (validée GO, avec persist en fin d'étape). Un module = un build + un
test board + un commit (extraction pure).

## Modules extraits

| Commit | Module | Lignes .c/.h | Contenu |
|---|---|---|---|
| `dbd8445a` | `strip_dyn.c/h` | 216+88 | V12-EXP gate + V13-COMP tranche + V13.3 lien stéréo (link_partner inline .h) |
| `f5d08b8e` | `master.c/h` | 54+103 | V13.7 meq double-banque + makeup LUFS + coeffs BS.1770 ; application crossfade reste dans audio_thread (étape 3) |
| `3722e580` | `automix.c/h` | 679+158 | Cerveau AUTOMIX LIVE complet : Dugan, soundcheck/lock/keeper, autolive, balance quadrants, gate auto, solo v2, EQ placement. **Invariants gravés en tête de automix.h** |
| `a8ab1e04` | `voice.c/h` + `dsp_block.h` | 236+76+43 | vfocus (duck_render) + spatializer + helpers NEON partagés |
| (2e) | `persist.c/h` | 597+40 | save/load fichiers dédiés + save_state_to + parse_state_lines (reste static interne) + scene_apply |

`mixer-pro.c` : 6 255 → **4 341** lignes. Structs anonymes nommées pour les
externs (`bmx_state`, `eqx_state`, `vf_state`, `vspat_state`, `meq_params`,
`mk_state`) — inits identiques, seule adaptation. `state.h` porte les
externs transversaux (presets_dirty, mic_map, out_gain, insert, assistant).

## Validation board

| Test | Résultat |
|---|---|
| 2a gate (`dca2286c`) : set_expander round-trip complet | −45/3/2/120/18/40 relus exacts + **gr_db −18.0 vivant en RT** (micro silencieux) ✓ |
| 2a comp : on/ratio/makeup appliqués ; lien stéréo : fader src0=0.5 → src1 miroir 0.5 ✓ |
| 2b master (`dd633215`) : master_eq répond, params opérateur relus au boot, LUFS silence cohérent ✓ |
| 2c automix (`9dbf6891`) : **morceau complet** — keepers en correction active (kick −1.8, drums −7.9, bass −6.7, line −14.4), LUFS −15.1 tenu par la balance, xrun 0 ✓ |
| 2c : xruns_cap/play 1/1 sur 1er run (frontière de flux pw-play) → contre-mesure 2e run : **0/0 en flux stable et à l'arrêt** — transitoire, pas une régression ✓ |
| 2d voice (`6cddf02d`) : cuts vfocus vivants pendant chant (0.60/0.75/0.49/0.32/0.38 dB), vspat réglable live 60→50 %, xruns UAC2 0/0 ✓ |
| 2e persist (`f9e42e18`) : gate 19 → restart → moteur relit 19 (boot) ; scène save(19) → set 25 → recall → 19 (scene_apply) ; nominal 15 restauré ✓ |
| Chaque build | 0 erreur, 0 warning nouveau ✓ |

## Incidents de méthode (corrigés immédiatement, consignés)

1. **Garde-fous d'extraction trop littéraux** : « V13-BANDMIX », « bmx_tick »,
   « persistence_thread » apparaissent dans des COMMENTAIRES des zones voisines
   → 3 asserts déclenchés à vide. Règle : ancrer sur les définitions
   (`static void X`), jamais sur des mots.
2. **Offsets calculés avant les `replace`** (extraction persist) : la découpe
   a mutilé `persistence_thread` — build rouge, restore git, passe refaite
   dans l'ordre replaces→index→cut. Règle : TOUS les replaces avant tout
   calcul d'offset.
3. **`gcc -fsyntax-only` host s'arrête à `arm_neon.h`** (ligne ~1199) : tout
   ce qui suit n'est PAS vérifié sur host — seul bitbake fait foi pour la
   moitié basse du fichier.

## Test utilisateur : EN ATTENTE (écoute complète — balance, vfocus,
spatializer, scènes au tactile)

## Reste (ARCHI V14)

Étape 3 : `uac2_ring.c` + `audio_loop.c` (cœur RT). Étape 4 : `control.c` +
ops distribuées dans les modules (les externs temporaires g_* redeviennent
privés). Étapes 5-8 : effects/, gui-http, web JS/CSS, QML.
