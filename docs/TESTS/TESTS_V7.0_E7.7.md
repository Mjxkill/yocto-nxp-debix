# Test Fiche : V7.0 — E7.7 (FX returns visibility + R1..R8 clickable + reproducibility infra)

**Date** : 2026-05-13
**Statut** : **GO** — visibilité routing returns OK, sidebar R1..R8 fonctionnelle, reproductibilité SOF firmware combée
**Commits** :
- `0216bd71 mixer-gui-http: E7.7a — show return destinations on each BUS FX card`
- `87f71fd1 mixer-gui-http: E7.7b — fix R1..R8 returns FX strips clickable`
- `6511f273 meta-local: sof-firmware-custom recipe — package SOF .ri + .tplg for repro`

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E7.7 polish UX + reproductibilité |
| Préalable | E7.6 GO (doc consolidée), E7.5 GO (analyzer) |
| GUI version | `v7.0-e7.7b` |
| mixer-pro version | `v7.0-e7.5a` (inchangé depuis E7.5) |
| SOF firmware md5 | `743e2e249cc3ea6a291b975a614514f0` (commit sof `4bea8e59d` poussé sur github Mjxkill/sof) |
| Topology md5 | `0363fd2806b9d2adfbd4f4cf994dc6e9` (sof-imx8mp-tac5212.tplg) |

## Travaux exécutés

### E7.7a — Returns destinations on each BUS FX card

User feedback : *"dans la table de mixage, je ne vois pas sur quelles voies les effets send sont retransmis"*. Les returns post-FX (R1..R8) étaient routables (master matrix) mais leur destination n'était pas visible d'un coup d'œil.

- **State JS** : `returnsRouting[4] = { L:[18 gains], R:[18 gains] }` par bus FX
- **Init** : `fetchAllReturnsRouting()` fait 8 `get_strip_routing` en parallèle (src=18..25)
- **Refresh** : hook `setMasterRoute` détecte si le strip courant est un return (idx 18..25) et appelle `refreshReturnRouting(src)` automatiquement
- **Helper** : `fxReturnDestinations(b)` renvoie `[{idx, label, side:'L'|'R'|'B'}]` avec seuil -60 dB
- **UI** : nouvelle ligne `.fx-routed-to` sur chaque fx-card listant les tags S1..S8 / U1..U8 / P1..P2 avec code couleur :
  - **vert** ('B') = L et R routés à la même sortie
  - **turquoise** + 'L' = uniquement L
  - **orange** + 'R' = uniquement R
- Pas de modification backend (`get_strip_routing` existait depuis E6.d pour tous src 0..25)

### E7.7b — fix R1..R8 returns FX strips clickable

Bug identifié pendant la validation E7.7a : *"quand je clique sur R1 ou R2 je n'ai pas de sidebar qui apparaît"*. Les strips RETURNS FX n'avaient simplement aucun `@click` (oubli depuis le layout initial).

- Ajout du même pattern de clic que DSP MICS / USB ASIO IN / PHONE :
  - `.strip @click.self="openStripDetail(i+17, 'R'+i, 'Return FX', false)"`
  - `.strip @dblclick=` idem
  - `.label` et `.sublabel` ont leur propre `@click`
  - `:class="...selectedStrip === i+17 ? 'selected'..."`
- Index mapping : R1..R8 = strips 18..25 dans `master_gain[N_INPUT_TOTAL][N_OUTPUT_TOTAL]`

### Reproductibilité SOF — recipe `sof-firmware-custom`

Découverte pendant l'audit reproductibilité : le `bitbake imx-image-full` ne produisait PAS le firmware SOF custom — le recipe vendor `sof-zephyr_2.10.0.bb` ship un tarball NXP avec WM8960/WM8962 et aucun TAC5212, et le firmware custom (commit sof `4bea8e59d`) était scp'd à la main.

- Nouveau recipe `meta-local/recipes-bsp/sof-firmware-custom/sof-firmware-custom_1.0.bb` vendore :
  - `sof-imx8m.ri` (283 KB, md5 `743e2e249cc3ea6a291b975a614514f0`)
  - `sof-imx8mp-tac5212.tplg` (21 KB, md5 `0363fd2806b9d2adfbd4f4cf994dc6e9`)
  - Install dans `/lib/firmware/imx/sof-zephyr-{xcc,gcc}/sof-imx8m.ri` (variants liés par le symlink `sof → sof-zephyr-xcc`)
- Companion bbappend `meta-local/recipes-kernel/linux-firmware/sof-zephyr_%.bbappend` retire les `.ri` vendor pour éviter le QA file-conflict
- `imx-image-full.bbappend` ajoute `sof-firmware-custom` à `IMAGE_INSTALL`
- `README.md` documente la procédure de rebuild firmware depuis `sof/` source tree
- SOF local `4bea8e59d` poussé sur github Mjxkill/sof (était 1 commit en avance)

## Tests T7.7.X — exécutés

| Test | Résultat |
|---|---|
| T7.7.1 clic sur R1 → sidebar ouvre | ✓ |
| T7.7.2 R3 sidebar montre master matrix vers S1..P2 | ✓ |
| T7.7.3 routing R1 → S1 → tag "S1 L" apparaît sur fx-card BUS FX 1 | ✓ |
| T7.7.4 routing R2 → S1 → tag passe à "S1" (vert L+R) | ✓ |
| T7.7.5 dégonfler routing à -∞ dB → tag disparaît | ✓ |
| T7.7.6 stéréo link R1+R2 → routage symétrique automatique | ✓ |
| T7.7.7 build mixer-gui-http v7.0-e7.7b OK | ✓ |
| T7.7.8 bitbake sof-firmware-custom OK | ✓ |
| T7.7.9 bbappend sof-zephyr retire .ri vendor de `${D}` | ✓ vérifié filesystem |
| T7.7.10 push sof feature/v7.0-multiband-drc-tap → github | ✓ |
| T7.7.11 push yocto-nxp-debix → github | ✓ |

## Validation utilisateur

> "ça marche, push" — 2026-05-13 (E7.7b)

→ **GO**.

## Limites connues

- Le `bitbake imx-image-full -c rootfs` échoue actuellement sur `kernel-module-imx-audio-tap` (package non trouvé) — dette pré-existante hors scope E7.7, à résoudre avant qu'une vraie reproduction from scratch fonctionne
- `mixerctl` binaire builde dans le recipe mixer-pro mais n'est pas déployé sur la carte (oubli, non critique fonctionnellement)
- Le système n'a pas encore de persistance des réglages (gains, sends, FX, taps, DRC) — ils sont volatiles à chaque reboot

## Suite envisageable

- E7.8 potentiel : persistance d'état (snapshot/restore au boot)
- E7.x : presets nommés (Live / Studio / Conf)
- Régler la dette `kernel-module-imx-audio-tap` pour reproductibilité complète
