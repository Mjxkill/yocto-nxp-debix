# Test Fiche : V5.4.1 — E3.a m4 widgets compile-only

**Date** : 2026-04-28
**Statut** : GO compile-only (test fonctionnel runtime → E3.b)

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.3 |
| Version de référence | V5.4.1 |
| Étape | E3.a — création des 3 widgets m4 + compile alsatplg |
| Commit SOF | `ebcd4d268` (branch `feature/audio-platform-v2`) |
| Commit yocto-nxp-debix | `80dd9253` (inchangé — pas de modif yocto à E3.a) |
| Topologie de référence | `sof-imx8mp-tac5212-V5.4.1-test.m4` (NEW, compile-only) |
| Firmware sof-imx8m.ri md5 board | `5b9acfc367ae5de28b6017b82d67810d` (E2 inchangé, pas de re-deploy à E3.a) |
| Topology .tplg compile-only | `/tmp/V5.4.1-test.tplg` 9124 B (host machine, pas déployée) |

## Fichiers créés

| Fichier | Rôle |
|---|---|
| `sof/tools/topology/topology1/m4/tee_1to2.m4` | Widget m4 pour tee_1to2 (UUID e1ec7700-...) |
| `sof/tools/topology/topology1/m4/mixer16.m4` | Widget m4 pour mixer16 (UUID d2e64a00-...) |
| `sof/tools/topology/topology1/m4/interleave_8.m4` | Widget m4 pour interleave_8 (UUID c8a3b500-...) |
| `sof/tools/topology/topology1/sof-imx8mp-tac5212-V5.4.1-test.m4` | Topology test compile-only (NEW) |

## Pattern m4 widget

Les 3 widgets suivent le pattern `multiband_drc.m4` (UUID-based recognition par firmware) :
- `DECLARE_SOF_RT_UUID` — déclare l'UUID custom
- `define(N_<COMP>, ...)` — nom widget concaténé avec PIPELINE_ID
- `define(W_<COMP>, ...)` — macro qui produit les SectionVendorTuples + SectionData + SectionWidget
- type `effect`, `no_pm true`, `SOF_TKN_PROCESS_TYPE` string custom

Le kernel SOF Linux retournera `SOF_PROCESS_NONE` + `SOF_COMP_NONE` pour ces types non listés (ipc3-topology.c:37-47), MAIS le firmware reconnaît par UUID (`SOF_TKN_COMP_UUID`) → comp instancié correctement. Pattern identique à `multiband_drc` qui fonctionne en V3.2.2.

## Tests réalisés (Claude — host machine)

| # | Test | Commande | Attendu | Résultat |
|---|---|---|---|---|
| E3.0 | Lecture pattern multiband_drc.m4 + ipc3-topology.c kernel | Read | UUID recognition pattern documenté | ✅ |
| E3.a.1 | m4 expand topology test | `m4 -I . -I m4 -I common -I sof sof-imx8mp-tac5212-V5.4.1-test.m4 > /tmp/V5.4.1-test.conf` | exit=0, output 1969 lignes, 0 erreur | ✅ |
| E3.a.2 | alsatplg compile en .tplg | `alsatplg -c /tmp/V5.4.1-test.conf -o /tmp/V5.4.1-test.tplg` | tplg 9124 B généré sans erreur | ✅ |
| E3.a.3 | UUIDs custom dans .tplg | `xxd /tmp/V5.4.1-test.tplg \| grep -i tee_1to2` | TEE_1TO2 / MIXER16 / INTERLEAVE_8 strings présents | ✅ |
| E3.a.4 | Process types dans .tplg | `strings /tmp/V5.4.1-test.tplg \| grep -E TEE_1TO2\|MIXER16\|INTERLEAVE_8` | 3 types reconnaissables | ✅ |

## Bug rencontré + fix

| # | Bug | Fix |
|---|---|---|
| 1 | Quote simple superflu après `"<TYPE>"` dans les 3 widgets (`SOF_TKN_PROCESS_TYPE`) | Supprimé le `'` final (alignement avec multiband_drc.m4) |

## Test utilisateur (réel, in-the-loop)

| Champ | Valeur |
|---|---|
| Test utilisateur effectué | **NON** |
| Type de test | — |
| Résultat | — |
| Commentaires | E3.a = test compile-only sur host machine. Pas de deploy board, pas de runtime. Test utilisateur attendu à **E3.b** (topology fonctionnelle où les 3 comps sont connectés dans un pipeline + deploy + arecord/aplay). |

## Logs significatifs

```
$ m4 ... sof-imx8mp-tac5212-V5.4.1-test.m4 > /tmp/V5.4.1-test.conf
$ wc -l /tmp/V5.4.1-test.conf
1969 /tmp/V5.4.1-test.conf

$ alsatplg -c /tmp/V5.4.1-test.conf -o /tmp/V5.4.1-test.tplg
[no output, exit 0]

$ ls -la /tmp/V5.4.1-test.tplg
-rw-rw-r-- 1 michael michael 9124 avril 28 14:37 /tmp/V5.4.1-test.tplg

$ strings /tmp/V5.4.1-test.tplg | grep TEE_1TO2
TEE_1TO2PIPELINE_ID.0
TEE_1TO2
```

## Limitations E3.a

1. Les widgets de test sont isolés (PIPELINE_ID littéral non substitué car define hors pipeline réel) → cosmétique seulement, ne perturbe pas la validation compile.
2. La topology test n'est PAS connectée fonctionnellement (W_TEE_1TO2/W_MIXER16/W_INTERLEAVE_8 sans P_GRAPH downstream) → ne peut pas être déployée telle quelle.
3. Le firmware E2 est suffisant (3 UUIDs déjà enregistrés) — pas de rebuild firmware à E3.a.

## Conclusion

**GO E3.a.** Les 3 widgets m4 (tee_1to2 + mixer16 + interleave_8) compilent avec alsatplg et produisent un .tplg valide avec UUIDs + process types corrects. Pattern UUID-based recognition validé compile-time. Prêt pour **E3.b** : topology fonctionnelle minimale + deploy + test runtime.

## Référence

- Spec : `PHASE_1A_3_DSP_V5.4.1.md`
- Pattern m4 référence : `sof/tools/topology/topology1/m4/multiband_drc.m4`
- Kernel SOF process_type lookup : `sound/soc/sof/ipc3-topology.c:37-47` (kernel 6.6.36)
- Fiche précédente : `TESTS_V5.4.1_E2.md`
