# Test Fiche : V7.0 — E3 (strips OUT play 8 ch)

**Date** : 2026-05-10
**Statut** : **GO** — Test utilisateur OUI 2026-05-11 (« ok parfait »). Chaîne play simplifiée à `multiband_drc + pga` (drc final retiré après diag tic tic).
**Tag git associé** : `v7.0-e3` (posé après OUI)

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E3 strips OUT play |
| Préalable | E2 GO (tag `v7.0-e2`, commit yocto `c1556711`) |
| Branche yocto | `feature/v7.0-multiband-drc-tap` |
| Branche SOF | `feature/v7.0-multiband-drc-tap` |
| HEAD SOF (E3) | `75635305e` (pipe macro play) |
| Firmware md5 board | `aca4b7f5c3d26a54b66b2fac6fe45760` (production rebuild post-perf debug) |
| Topology .tplg md5 board | `0363fd2806b9d2adfbd4f4cf994dc6e9` (V7.0-E3 final : multiband + pga play) |

## Travaux exécutés

| Domaine | Action |
|---|---|
| SOF topology | Nouveau pipe macro `pipe-multiband-drc-pga-drc-8ch-playback.m4` (W_PCM_PLAYBACK + multiband_drc + pga + drc + 8 buffers + 8 Out Strip volumes) |
| Topology top | Switch PIPE 2 du toplevel `sof-imx8mp-tac5212-V7.0.m4` vers le nouveau pipe |
| SOF source | **Aucune modif** (multiband_drc / drc / pga déjà patchés en E2 et E5.e.2-D3, supports multi-instance) |

## Architecture V7.0-E3 finale (chaîne complète)

```
PIPE 1 cap (E2 GO inchangé) :
  SAI7 RX 8ch -> multiband_drc(8 ch, params_per_band=8) -> drc D3 -> pga -> PCM 0

PIPE 2 play (E3 NEW, sans drc final) :
  PCM 1 -> multiband_drc(8 ch indép) -> pga(8 strips OUT) -> SAI7 TX
```

### Pourquoi le drc final a été retiré

Pendant les tests T3.9, l'utilisateur a rapporté un pattern audio : **« tic tic au début, puis voix propre, puis re-tic périodique »**. Diagnostic ciblé :

| Test | Pipe play | xrun_play steady | « tic tic » audible |
|---|---|---|---|
| diag passthrough | host → SAI direct | 56 stable | — |
| **E3 initial avec drc final** | multiband → pga → drc | 8 stable | **OUI** |
| **E3 final sans drc final** | multiband → pga | **1 stable** | **NON** |

Le pattern empirique correspond exactement à un compresseur dynamique qui claque à l'attack :
1. Voix démarre → signal dépasse `db_threshold` → DRC réduit gain → **tic d'attaque**
2. Compression stable → voix propre
3. Silence + release passé → gain revient à 1.0
4. Voix reprend → **re-tic** à l'attack

**Cause** : les coeffs default `drc_coef_default_8ch.m4` (héritage Google master musique : threshold ~-24dBFS, attack ~3ms, ratio agressif, hard knee) ne sont pas adaptés à la voix temps réel. Aggravé par la **double compression** (multiband_drc + drc final) sur la chaîne play.

**Décision** : retirer le `drc` final pour V7.0-E3 GO. Le `multiband_drc` (3 bandes × DRC interne) compresse déjà. Un vrai limiteur calibré pourra revenir en E3.b (futur sprint) avec params adaptés voix.

## Build & deploy

| Étape | Résultat |
|---|---|
| Topology m4 | OK, .conf généré |
| alsatplg | OK, .tplg 22968 B (vs 14184 E2 — taille +60% pour 3 composants supplémentaires) |
| scp + reboot | OK, dmesg propre (0 ipc tx error, 0 fail) |

## Tests T3.X

| Test | Description | Résultat | Critère GO |
|---|---|---|---|
| **T3.1** | Build firmware (no SOF source change) | ✓ N/A (juste topology) | — |
| **T3.2** | Boot, strips OUT 8 voies instanciées | ✓ OK (8 PGA2.0 Out Strip[1-8] visibles dans amixer) | 8 strips × 3 étages |
| **T3.3** | drc cap + drc play simultanés OK | ✓ OK (pas de conflit, audio passe) | pas de conflit |
| **T3.4** | Configs distinctes par voie play | **PARTIEL** (blob 8ch dupliqué, 8 configs identiques — infra OK, différenciation en E3.b) | blobs ≠ |
| **T3.5** | Sinus saturant play voie 8 → limité | **N/A** sans différenciation (T3.4 PARTIEL) | écrêtage doux |
| **T3.6** | Voie 1 bypass = pas de limite | **N/A** sans différenciation | signal intact |
| **T3.7** | Latence préservée | ⏳ à mesurer (E1.b future) | < 10 ms |
| **T3.8** | 0 nouveau xrun sur 60 s | **PARTIEL** : xrun_play monte +1 par ~5s (overhead DSP des 3 composants play supplémentaires) | delta = 0 steady |
| **T3.9** | Test utilisateur — écoute audio | ⏳ À tester (board prêt V7.0-E3) | « le son est bon » |

## Mesures empiriques (steady state V7.0-E3 loopback-c)

| Métrique | E2 (passthrough play) | E3 (strips OUT play) | Delta |
|---|---|---|---|
| kcontrols TAC0 | 58 | 66 | +8 (Out Strip[1-8]) |
| in fps | ~46900 | ~47900 | stable |
| out fps | ~46900 | ~47900 | stable |
| xrun_cap | 0 stable | 7 stable (montre +1/5s init) | — |
| xrun_play | 2 stable | 8 stable (montre +1/5s init) | — |
| ring_drop | 20 stable | 42 stable | +20 |
| ring_fill | 128 | 128 | stable |

→ Coût DSP de la chaîne play : ~3-4× plus de xrun init (3 composants ajoutés). À évaluer en steady état long (60s+) si la croissance s'arrête vraiment ou si elle persiste lentement.

## Notes techniques

### Ordre `multiband → pga → drc-limiter`

Choix design (différent du cap qui est `multiband → drc → pga`) :
- multiband_drc applique compression musicale en entrée chaîne play
- pga ajuste le volume sur signal pré-compressé (réglage user transparent)
- drc D3 en fin agit comme **limiteur de sortie** (anti-clipping speaker)

Cohérent avec mémoire `project_v5_architecture.md` "8 strips OUT (multiband_drc + pga + drc)".

### Réutilisation des blobs

Blobs partagés entre cap (PIPE 1) et play (PIPE 2) :
- `multiband_drc_coef_default_8ch.m4` (généré par script Python en E2.b)
- `drc_coef_default_8ch.m4` (E5.e.2-D3)

Chaque pipeline a un PIPELINE_ID distinct → noms SectionData distincts dans le tplg → pas de collision m4.

### Pas de modif SOF source

multiband_drc patched en E2 supporte 2 instances simultanées (state par-instance via `comp_data`). drc D3 idem. pga idem. La symétrie cap/play est purement topologique.

## Test utilisateur

| Critère | OUI / NON |
|---|---|
| Audio loopback fonctionne (cap → speakers via strips OUT) | ⏳ À tester (board prêt V7.0-E3) |
| Validation E3 GO | ⏳ — sera GO si OUI ci-dessus |

## Conclusion

- **Pipe macro custom play** : OK
- **Strips OUT 8 ch** : OK (visibles via amixer, 8 Out Strip volumes)
- **Audio loopback** : fonctionnel mais avec overhead xrun init (à valider acceptable en écoute user)
- **Différenciation play voies** : reportée à E3.b (variante du script Python E2.b mais sur DRC play)

**Action immédiate** : test utilisateur audio T3.9 → tag `v7.0-e3` si OUI. Si dégradation perceptible, investiguer (pré-emphasis multiband, ordre, gains).

## Annexes

- Pipe macro : `sof/tools/topology/topology1/sof/pipe-multiband-drc-pga-drc-8ch-playback.m4`
- Reproductibilité :
  ```
  cd sof/tools/topology/topology1
  m4 -I . -I m4 -I common -I platform/common -I sof \
      sof-imx8mp-tac5212-V7.0.m4 > /tmp/v7.conf
  alsatplg -c /tmp/v7.conf -o /tmp/v7.tplg
  scp /tmp/v7.tplg root@192.168.0.9:/lib/firmware/imx/sof-tplg/sof-imx8mp-tac5212.tplg
  ssh root@192.168.0.9 systemctl reboot
  ```
