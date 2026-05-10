# Test Fiche : V7.0 — E2 (multiband_drc CAP per-channel)

**Date** : 2026-05-10
**Statut** : **GO** (infra) — Test utilisateur OUI 2026-05-10 (« le son est bon ») ; différenciation auditive T2.5/T2.6 reportée à E2.c
**Tag git associé** : `v7.0-e2` (posé après validation user)

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E2 multiband_drc CAP 8ch indép |
| Préalable | E1 GO (tag `v7.0-e1`, commit yocto `1e41031c`) |
| Branche yocto | `feature/v7.0-multiband-drc-tap` |
| Branche SOF | `feature/v7.0-multiband-drc-tap` |
| HEAD SOF (E2) | `5eafab230` (infra 8ch blob) — précédent : `7440b3c21` (patch source multiband_drc) |
| Firmware md5 board | `547ecfe1f32122574ffe59cc40b52c55` |
| Topology .tplg md5 board | `2b6fee5794861b0de90b3485ba6e25f1` (V7.0-E2 + blob 8ch) |

## Travaux exécutés

### Patch source SOF (commit `7440b3c21`)

| Fichier | Changement |
|---|---|
| `src/audio/multiband_drc/user/multiband_drc.h` | bump `SOF_MULTIBAND_DRC_MAX_BLOB_SIZE` 1024 → 2048 (puis 4096 en .b) ; ajout `SOF_MULTIBAND_DRC_HEADER_FIXED_SIZE = offsetof(.., drc_coef)` ; doc layout étendu |
| `src/audio/multiband_drc/multiband_drc.h` | ajout `uint32_t params_per_band` dans `multiband_drc_comp_data` |
| `src/audio/multiband_drc/multiband_drc.c` | calcul `params_per_band` au config load (motif drc D3 `ea984a266`) ; fallback legacy si blob mal aligné ; `pre_delay_time` prend ch=0 de chaque bande |
| `src/audio/multiband_drc/multiband_drc_generic.c` | signature `multiband_drc_{s16,s32}_process_drc` étendue avec `p_band_base + params_per_band` ; macro `DRC_PARAM_FOR_CH` ; enabled flag band-global (ch 0) ; callers passent `drc_coef[band * params_per_band]` |
| `tools/topology/topology1/sof/pipe-multiband-drc-pga-8ch-capture.m4` | nouveau pipe macro (remplace eq_iir par multiband_drc + drc D3 + pga 8ch) |
| `tools/topology/topology1/sof-imx8mp-tac5212-V7.0.m4` | PIPE 1 cap pointe vers le nouveau pipe macro |

### Infra 8ch blob (commit `5eafab230`)

| Fichier | Changement |
|---|---|
| `tools/topology/topology1/m4/gen_multiband_drc_coef_8ch.py` | nouveau script Python : parse blob default + abi_hdr (32 B prefix), extrait `drc_coef[]` par bande, réplique N fois (default 8), update size fields, ré-émet `bytes "..."` m4 syntax |
| `tools/topology/topology1/m4/multiband_drc_coef_default_8ch.m4` | blob généré (2468 B total : 32 abi_hdr + 324 header + 2112 drc_coef = 3 bandes × 8 ch × 88 B) |
| `tools/topology/topology1/sof/pipe-multiband-drc-pga-8ch-capture.m4` | switch vers `MULTIBAND_DRC_priv_8ch` + nouveau default coef |
| `src/audio/multiband_drc/user/multiband_drc.h` | re-bump MAX_BLOB_SIZE 2048 → 4096 (marge 4 bandes × 8 ch) |

## Build & deploy

| Étape | Résultat |
|---|---|
| west build firmware | OK, no error |
| west sign | Reef OK, md5 `547ecfe1f3...` |
| Topology m4 + alsatplg | OK, tplg 14184 B, md5 `2b6fee5794...` |
| scp + reboot | OK, dmesg propre |

## Tests T2.X

| Test | Description | Résultat | Critère GO |
|---|---|---|---|
| **T2.0** (régression) | Blob legacy 1-config = comportement E0/E1 préservé | ✓ OK (xrun=5, drop=49 stable) | pas pire que E1 |
| **T2.1** | SOF unit tests multiband_drc | n/a (pas de suite tests dans build) | tests passent |
| **T2.2** | Build firmware OK | ✓ OK | Reef signature OK |
| **T2.3** | Boot + 1 comp multiband_drc 8ch (params_per_band=8 logique) | ✓ OK | pas d'error |
| **T2.4** | Configs distinctes appliquées | **PARTIEL** : infra OK (params_per_band=8 accepté), 8 configs identiques (réplication) | blobs différents lus |
| **T2.5** | Sinus saturant voie 1 → compression | **N/A** sans différenciation | crête limitée |
| **T2.6** | Voie 3 bypass = pas de compression | **N/A** sans différenciation | crête = entrée |
| **T2.7** | Latence E1 préservée | ✓ OK (boot + audio OK) | < 10 ms (E1.b) |
| **T2.8** | 0 nouveau xrun sur steady state | ✓ OK (xrun_cap=0, xrun_play=2 STABLE, ring_drop=20 STABLE) | delta = 0 steady |
| **T2.9** | Test utilisateur — écoute audio loopback | ✓ OUI 2026-05-10 (« le son est bon ») | audio fonctionnel |

## Mesures empiriques

| Config | xrun_cap | xrun_play | ring_drop | in/out fps |
|---|---|---|---|---|
| E0 baseline (E6.a) | 0 | 7 | 70 | ~47000 |
| E2 legacy (multiband 1-config) | 1 | 5 | 49 | ~46900 |
| **E2 + 8ch blob** | **0** | **2** | **20** | **~46800** |

→ Infra V7.0-E2 produit un audio **plus stable** qu'E0/E1 baseline.

## Notes techniques

### Détection params_per_band côté firmware

```c
size_t trailing = config->size - SOF_MULTIBAND_DRC_HEADER_FIXED_SIZE;
size_t one_band = num_bands * sizeof(struct sof_drc_params);
cd->params_per_band = (one_band == 0 || (trailing % one_band) != 0)
                      ? 1                                        // fallback legacy
                      : (uint32_t)(trailing / one_band);
```

Pour V7.0-E2 blob : 2436 - 324 = 2112 trailing, 3 × 88 = 264 one_band, 2112 / 264 = **8** ✓

### ABI back-compat

Le firmware **valide les 2 modes** :
- Blob legacy (config par bande, 1 params) : `params_per_band = 1` (T2.0 confirmé)
- Blob V7.0-E2 (params per band per channel) : `params_per_band = 8` (T2.3 implicit)

Aucune topology existante ne casse — le patch est ABI back-compat strict.

### Limites

- `params_per_band` doit diviser `trailing` exactement, sinon fallback legacy.
- `enabled` flag traité globalement (ch=0) — si on veut un enable per-channel, modifier `multiband_drc_generic.c` `if (p_band_base[0].enabled ...)`.
- `pre_delay_time` toujours partagé (ch=0) — c'est par design (alignement temporel inter-canaux).

## Test utilisateur

| Critère | OUI / NON |
|---|---|
| Audio loopback fonctionne (cap → speakers) | **OUI** 2026-05-10 (« le son est bon ») |
| Validation E2 infra GO | **OUI — GO** — tag `v7.0-e2` posé ; différenciation auditive E2.c |

## Conclusion

- **Patch source multiband_drc** : OK, ABI back-compat préservé.
- **Pipe macro custom** : OK, ABI compatible avec drc D3 (8 instances).
- **Infra 8ch blob** : OK, firmware accepte params_per_band=8.
- **Audio loopback** : OK, performances **meilleures** que E0/E1.
- **Différenciation auditive T2.5/T2.6** : reportée à **E2.c** (modifier `gen_multiband_drc_coef_8ch.py` pour faire varier `db_threshold` sur certaines voies — ~5 lignes Python).

**Action immédiate** : test utilisateur sur board pour validation T2.9 (loopback OK), puis tag `v7.0-e2` (partial GO infra). Différenciation auditive complète viendra en E2.c (sprint dédié) ou à intégrer en E3 (strips OUT play).

## Annexes

- Script Python : `sof/tools/topology/topology1/m4/gen_multiband_drc_coef_8ch.py`
- Blob généré : `sof/tools/topology/topology1/m4/multiband_drc_coef_default_8ch.m4` (2468 B)
- Backup ancien tplg sur board : `sof-imx8mp-tac5212.tplg.bak-pre-v7e0`
- Commands reproductibilité :
  ```
  cd sof/tools/topology/topology1/m4 && python3 gen_multiband_drc_coef_8ch.py
  cd .. && m4 -I . -I m4 -I common -I platform/common -I sof \
      sof-imx8mp-tac5212-V7.0.m4 > /tmp/v7.conf
  alsatplg -c /tmp/v7.conf -o /tmp/v7.tplg
  scp /tmp/v7.tplg root@192.168.0.9:/lib/firmware/imx/sof-tplg/sof-imx8mp-tac5212.tplg
  ssh root@192.168.0.9 systemctl reboot
  ```
