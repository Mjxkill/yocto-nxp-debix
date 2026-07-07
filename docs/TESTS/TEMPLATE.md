# Test Fiche : V<version> — E<étape>

**Date** : YYYY-MM-DD
**Statut** : GO / REVIEW / NOK / RÉTRO-FILL

## Référentiel

| Champ | Valeur |
|---|---|
| Phase de référence | Phase 1a.X |
| Version de référence | V<x.y.z> |
| Étape | E<n> — <description> |
| Commit SOF | `<hash>` (branch `<branch>`) |
| Commit yocto-nxp-debix | `<hash>` (branch `<branch>`) |
| Topologie de référence | `<file.m4>` (path repo) |
| Firmware sof-imx8m.ri md5 board | `<md5>` |
| Topology .tplg md5 board | `<md5>` |
| Kernel Image md5 board | `<md5>` (si applicable) |
| DTB md5 board | `<md5>` (si applicable) |

## Devices ALSA + audio

| Device | Card | Rôle | Format |
|---|---|---|---|

## Tests réalisés (Claude — automatisés sur board)

| # | Test | Commande | Attendu | Résultat |
|---|---|---|---|---|

## Test utilisateur (réel, in-the-loop)

| Champ | Valeur |
|---|---|
| Test utilisateur effectué | OUI / NON |
| Type de test | <description (loopback live, écoute audio, FFT, réglage effets, etc.)> |
| Résultat | <constat utilisateur — clips, plantages, latence ressentie, qualité audio> |
| Commentaires | <explications, contexte, conditions d'usage> |

## Logs significatifs

```
<dmesg / output critique>
```

## Conclusion

<verdict + commentaire>

## Référence (autres docs/spec liées)

- Spec : `<file.md>`
- Investigation critic : job `<id>`
- Fiches précédentes : `TESTS_<x>.md`
