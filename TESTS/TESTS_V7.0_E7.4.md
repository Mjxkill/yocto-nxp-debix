# Test Fiche : V7.0 — E7.4 (TAC5212 programmable biquads via ALSA kcontrols)

**Date** : 2026-05-12
**Statut** : **À VALIDER** — code compilé + déployé, test board en attente
**Commits** :
- meta-local kernel `tac5212.c/h` : `13379c88` (branche `feature/audio-platform-v2`)
- meta-local mixer-gui-http : `b5ddeb93`

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E7.4 effets TAC5212 réglables (biquads RBJ) |
| Préalable | E7.3b (sidebar effets + ALSA kcontrols proxy) commit `0d0f844f` |
| SOF firmware | md5 `743e2e249cc3ea6a291b975a614514f0` (E7.2, inchangé) |
| GUI version | `v7.0-e7.4` |
| Kernel Image | md5 `e32b7bcaec429f392fc286c3f32761e9` |
| snd-soc-tac5212.ko | md5 `49a3ee90c58dfec110c215626b08f3e9` |
| mixer-gui-http bin | md5 `b9172c7f86921f69c359de513ee8c6f1` |
| index.html | md5 `b453674779d65ccddbe4161064bdb083` |
| Topology | `sof-imx8mp-tac5212.tplg` (V7.0 commit `a535405ca`, inchangée) |

## Travaux exécutés

### 1. `tac5212.c/h` — 24 SOC_BYTES_EXT kcontrols biquad

- `struct tac5212_bq_blob { u8 bytes[20]; }` exposé dans `tac5212.h`
- `struct tac5212_priv` : caches `adc_bq[12]` + `dac_bq[12]` + `mutex paged_lock`
- Helper `tac5212_paged_write_buf(priv, page, reg, buf, count)` sérialisé : flip `PAGE_SEL` → write loop → restore page 0, sous mutex
- Callbacks ALSA standard (`snd_ctl_elem_value` + `value.bytes.data[]`) :
  `tac5212_bq_blob_info` (TYPE_BYTES, count=20), `_get`, `_put`
- Encodage `private_value` : bit 4 = chain ADC(0)/DAC(1), bits 0..3 = biquad idx 0..11
- 24 entrées via macro `TAC5212_BQ_BLOB("ADC|DAC BQ<k> Coefs", PV(chain, idx))` insérées en fin du tableau `tac5212_controls[]`
- Cache init unity all-pass `{0x7F,0xFF,0xFF,0xFF, 0…}` + `mutex_init(&priv->paged_lock)` dans `tac5212_i2c_probe` avant register_component

### 2. `mixer-gui-http.c` — proxy bytes 256-char value

- `amixer_value_safe` limite 64 → 256 chars (pour 20 octets décimal `"B0,B1,…,B19"`)
- Buffer `value` du POST `/api/alsa/set` 64 → 256 chars
- Buffer `reply` 128 → 320 chars
- Version `v7.0-e7.4`

### 3. `index.html` — RBJ math + UI biquad sur les 8 voies

- Math RBJ Cookbook in-browser (`rbjBiquadBlob(type, fHz, q, gainDb, fs)`) — types Bypass / LPF / HPF / BPF / Notch / Peak / LowShelf / HighShelf / AllPass
- Normalisation `a0=1` + convention TI `D1=-a1, D2=-a2`, conversion Q1.31 BE clamp ±1.0
- State `biquadParams` keyed par nom kcontrol complet (`TAC<n> ADC|DAC BQ<k> Coefs`) ; persisté `localStorage`
- `parseBiquadName(name)` → `{ tac, chain, idx }` ou `null`
- `alsaGroupsForStrip()` : nouveau groupe `TAC ADC|DAC biquads` (12 entrées triées BQ1..BQ12) séparé du groupe `shared`, pour les 8 voies (TAC mapping `floor(strip/2)` × `CH(strip%2)+1`)
- UI : dropdown Type + sliders Hz (20-22000) / Q (0.1-10) / Gain (-24..+24 dB, visible Peak/Shelf seulement)
- CSS `.bq-form`/`.bq-grid` cohérent thème glassmorphism existant

## Tests T7.4.X — à exécuter board

| Test | Description | Cible | Résultat |
|---|---|---|---|
| T7.4.1 | Build kernel + tac5212.ko | `bitbake linux-imx` OK | ✓ exit 0 |
| T7.4.2 | Deploy Image+dtb + reboot | board back up, ALSA cards 3+4+5 (softac5212tdm/sofADCIn/sofprobes) | ✓ |
| T7.4.3 | `amixer -c softac5212tdm contents` | 24 lignes `TAC0 … BQ<k> Coefs` type=BYTES count=20 | ✓ numid 49-72 |
| T7.4.4 | Read default blob | cget numid=49 retourne `0x7f,0xff,0xff,0xff,0x00 × 16` | ✓ unity all-pass |
| T7.4.5 | Write+Read arbitrary 20 bytes ADC BQ1 | round-trip byte-perfect | ✓ `0x12…0xcc` → match |
| T7.4.6 | Effet audible LPF | sweep sinus 20Hz-20kHz capture, atténuation > -10 dB au-dessus de 1.5 kHz | _à remplir_ |
| T7.4.7 | GUI sidebar voie 1 (M1) | section "TAC ADC biquads" visible avec 12 cards BQ1..BQ12 | _à remplir_ |
| T7.4.8 | GUI changer BQ1 → Peak 500Hz +6dB | slider bouge, amixer cset envoyé, cget board confirme bytes | _à remplir_ |
| T7.4.9 | Persistence localStorage | reload page → params restored | _à remplir_ |
| T7.4.10 | GUI voies 3-8 (TAC1-3 si câblés) | sidebar montre groupes correspondants ou cset échoue silencieusement si TAC absent | _à remplir_ |
| T7.4.11 | Régression cap+play loopback | son toujours OK avec BQ_CFG=2'b10 par défaut | _à remplir_ |

## Commandes board (rappel)

```bash
# Deploy kernel (sur la board)
# Sur PC : scp Image + tac5212.dtb vers /boot

# Re-init TAC après boot
/usr/bin/tac-reset

# Lister les nouveaux kcontrols
amixer -c softac5212tdm contents | grep -i 'BQ.*Coefs'

# Lire le blob courant de la BQ1 ADC (utiliser le numid retourné par contents)
amixer -c softac5212tdm cget numid=<X>

# Écrire un LPF 1000 Hz Q=0.707 manuellement
# (en pratique passe par le GUI ; pour debug brut :
#  les 20 octets sont N0|N1|N2|D1|D2 chacun en 4 bytes BE Q1.31)

# Démarrer le GUI
systemctl restart mixer-gui-http
# Puis http://192.168.0.9:8080/
```

## Validation utilisateur

> _en attente_

→ **À VALIDER**.

## Bénéfices E7.4 vs E7.3

| Métrique | E7.3 | E7.4 |
|---|---|---|
| Filtres TAC | enums fixes (HPF 1/12/96 Hz, BQ_CFG slots) | **24 biquads programmables** (12 ADC + 12 DAC) avec freq continue 20 Hz-22 kHz, Q 0.1-16, gain ±24 dB |
| Math RBJ | — | implémentée userspace (kernel sans FPU) ; types LPF/HPF/BPF/Notch/Peak/Shelf/AllPass |
| Persistence | params perdus | localStorage par nom kcontrol |
| Couverture voies | 1 et 2 seulement | 8 voies (TAC0-3 × CH1-2) |

## Limites connues

- Q1.31 clamp ±1.0 → certaines configs très basse fréquence ou forte Q peuvent clipper les coefs (test sonore à faire ; alternative future Q1.30 si nécessaire)
- Seul TAC0 est câblé physiquement sur la board actuelle → voies 3-8 acceptent l'UI mais I2C write peut faire un timeout/error silencieux
- Pas encore : AGC (pages 12-13), DRC, limiter, foldback, VAD details → suite E7.4.x ou E7.5

## Suite

- **E7.4.b** : ADC AGC + first-order IIR HPF custom (P11) si nécessaire
- **E7.4.c** : DRC DAC + limiter + foldback
- **E7.5** : spectre + phase FFT canvas (live)
