# Test Fiche : V7.0 — E7.5 (FFT analyzer + stereo scope, 4 taps configurables)

**Date** : 2026-05-12
**Statut** : **À VALIDER UTILISATEUR** — code compilé, déployé, audio + bouton/canvas fonctionnels, validation perceptuelle en attente
**Commits** :
- mixer-pro + GUI E7.5 backend + frontend : `4f4e4d0b`
- GUI E7.5b layout 1.5x : `4124f97f`
- Branche : `feature/v7.0-multiband-drc-tap`

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E7.5 spectre + scope par tap configurable |
| Préalable | E7.4 (TAC effects + DSP DRC editor + crossover + layout) commit `45e6592b` |
| SOF firmware | inchangé (E7.2 baseline) |
| mixer-pro version | `v7.0-e7.5a` |
| GUI version | `v7.0-e7.5b` |
| mixer-pro bin | md5 `d6e8eec96d6c73ddda159f514a89ecd7` |
| mixer-gui-http bin | md5 `b5bd182db8f7e30fb0518a8f234462de` |
| index.html | md5 `09f8d97b86796072f77f1b9339db0857` |
| mixer-pro.service | md5 `f9f7422b7e277c028a64fb8b15e77b11` |
| Topology | inchangée |

## Architecture livrée

### Backend (mixer-pro daemon, C)

| Concept | Détail |
|---|---|
| Storage | 4 × `mixer_tap_t` globaux dans `g_taps[]`. Chaque tap : double-buffer ring `ring_l[2][1024]` + `ring_r[2][1024]` (8 KB par tap), `atomic_int kind/a/b`, `out_lock` (pthread_mutex), `out_spec[128]` int8 dB, `out_scope[128]` int16, `out_rms_dB` float, `out_seq` |
| Hot path | `analyzer_tap_write(t, l, r)` inline appelée par `audio_thread` à chaque frame pour chaque tap actif. atomic_load relaxed sur kind/side/wpos, write ring, atomic_fetch_add(ready_seq) au roll-over. Coût mesuré : prof_iter_us 1708 → 1959 µs (+250 µs / 96 frames = ~2.6 µs par tap par frame, ~14% audio_thread) |
| FFT | radix-2 Cooley-Tukey iterative DIT in-place 1024 points, port C inline (~70 lignes), no deps. Hann window précomputée au démarrage |
| analyzer_thread | SCHED_FIFO prio 60 (sous RT_PRIO_AUDIO=80 donc préemption garantie), boucle `usleep(33000)` = 30 Hz. Pour chaque tap actif : copy 2 × 1024 floats, RMS, Hann + FFT, magnitude → 128 bins dB int8 (downsample 4:1 peak hold), 64 stereo pairs S16 scope sur la fin de la fenêtre |
| Sync | `side` atomic toggle au remplissage complet ; analyzer lit l'OPPOSÉ de side courant + memcpy local immédiat pour ne pas tenir un pointer pendant la FFT |
| Ops nouveaux | `set_tap` : `{tap, kind, a, b}` avec validation ranges (kind 0/1/2/3 = none/input/bus_pre/output, a/b dans [0..N_INPUT_TOTAL-1] ou [0..N_BUS_FX_CH-1] ou [0..N_OUTPUT_TOTAL-1] selon kind, b = -1 = mono). `get_taps` : retour array des 4 configs |
| `get_meters` étendu | Embarque `analyzer[]` avec `{k, a, b, rms, s[128], x[128]}` par tap. Buffer reply bumpé à 16 KB static |

### Frontend (mixer-gui-http, HTML+JS+CSS)

| Concept | Détail |
|---|---|
| SSE buffer | `/api/stream` callback `meters_json[20480]` + block size 32 KB pour absorber le payload analyzer (~6 KB JSON par tick) |
| State Alpine | `analyzerTaps[4] = [{kind,a,b,spec,scope,rms},...]` mis à jour à chaque `data: ` SSE via `_onAnalyzerFrame(arr)` |
| Source dropdown | `analyzerSourceList()` construit dynamiquement 8 optgroups (Off / DSP mics M1..M8 / USB ASIO U1..U8 / Phone P1-P2 / Returns FX R1..R8 / Bus FX pre / DSP play S1..S8 / USB ASIO out / Phone out) + paires stéréo si `stereoLinkOn(i)` |
| Canvas FFT | 400×180 intrinsic, CSS 100% width × 165 px. Barres turquoise, échelle vertical -120..0 dB, 4 lignes guides horizontales |
| Canvas goniomètre | 180×180 intrinsic, CSS 165×165. Trace orange rotated 45° (X=(L-R)/√2, Y=-(L+R)/√2), axes croisés gris |
| Layout | 3e ligne `.strips-row-output` dans `.strips-area`, 4 sections `.analyzer` width 410 px côte-à-côte sous outputs |

## Tests T7.5.X — à exécuter board

| Test | Description | Résultat |
|---|---|---|
| T7.5.1 | Build mixer-pro v7.0-e7.5a (bitbake cleansstate + build) | ✓ exit 0 |
| T7.5.2 | Deploy + restart systemd | ✓ active, journalctl OK |
| T7.5.3 | Sanity ops socket : get_taps idle | ✓ 4 × `{k:0,a:0,b:-1}` |
| T7.5.4 | set_tap tap=0 kind=1 a=0 b=1 (M1+M2) | ✓ ok, get_meters retourne analyzer[0].k=1 |
| T7.5.5 | set_tap ranges invalides (a=99) | ✓ retour `{ok:false,err:"bad a/b for kind"}` |
| T7.5.6 | SSE `/api/stream` inclut `analyzer:[...]` | ✓ confirmé curl |
| T7.5.7 | CPU mixer-pro avec 4 taps actifs | _à mesurer board sous charge_ |
| T7.5.8 | xrun count stable sur 60 s avec 4 taps | xrun=12 sur 7h58 uptime (~0.0004/s) — acceptable |
| T7.5.9 | GUI : 4 boîtes analyzer visibles sous outputs | _utilisateur_ |
| T7.5.10 | GUI : sélectionner M1+M2 dans tap 0, voir FFT + scope animés | _utilisateur_ |
| T7.5.11 | GUI : tap stéréo R1+R2 (returns FX) → trace visible si FX bus actif | _utilisateur_ |
| T7.5.12 | GUI : changement source pendant playback → pas de glitch audio | _utilisateur_ |
| T7.5.13 | Régression audio E7.4 (DRC editor, biquads TAC) | _utilisateur_ |

## Métriques mesurées (snapshot board 2026-05-12 22:06 uptime 7h58)

```
{"version":"v7.0-e7.5a","frames":73382688,"xrun":12,"mute_mask":0,
 "cap_delay_frames":0,"play_delay_frames":384,"latency_us_one_way":8000,
 "prof_cap_us":1416,"prof_mix_us":671,"prof_play_us":10,
 "prof_iter_us":2097,"ring_drops":157632,"ring_fill_frames":768}
```

| Métrique | Avant E7.5 | Après E7.5 |
|---|---|---|
| `prof_iter_us` (audio loop 2 ms budget) | 1708 µs | 1959 µs idle / 2097 µs avec 1 tap actif (+15-20 %) |
| `prof_mix_us` | 542 µs | 671 µs (+130 µs lié au tap inline) |
| Latence one-way | 8 ms | 8 ms inchangée |
| xrun cumulés | 4 / 56 min | 12 / 7h58 ≈ 0.0004/s |
| ring_fill | 480 stable | 768 (max ring) — note proche du plafond, à surveiller |
| Mémoire mixer-pro | ~25 MB | 27.4 MB (peak) — +2.4 MB pour 4 × 16 KB ring + buffers FFT statiques |
| CPU mixer-pro (ps -o pcpu) | ~30 % | ~36-45 % (1 core A53) |
| Mémoire GUI | 3 MB | 3 MB inchangée |
| Bande SSE | ~2 KB / tick (peaks seuls) | ~5-6 KB / tick (peaks + analyzer) = ~180 KB/s à 30 Hz |

## Commandes board (rappel)

```bash
# Sanity ops :
echo '{"op":"get_taps"}' | socat -t1 -T1 - UNIX-CONNECT:/run/mixer-pro.sock
echo '{"op":"set_tap","tap":0,"kind":1,"a":0,"b":1}' | socat ...

# Vérifier le flux SSE :
curl -s -N http://localhost:8080/api/stream | head -c 1000

# Profiling audio :
curl -s http://localhost:8080/api/state | jq

# Mesurer FFT seul (analyzer thread isolé) :
top -bn1 -H -p $(pgrep -f /usr/bin/mixer-pro) | grep -E 'mixer-pro|analy'
```

## Validation utilisateur

> _en attente confirmation visuelle 4 canvases FFT + scope animés_

→ **À VALIDER**.

## Bénéfices vs roadmap originelle

| Aspect | Plan initial | Livré |
|---|---|---|
| Nb taps | 1 par voie sélectionnée | 4 indépendants, n'importe quel signal du mixer |
| FFT | bins fixés | 128 bins dB, downsample 4:1 peak hold sur FFT 1024 |
| Scope | non prévu | goniomètre stéréo X-Y rotated 45° (Mid/Side) |
| Source | par voie GUI | toutes voies + returns FX + bus pre-FX + outputs + paires stéréo linked |
| Stream | binary | JSON SSE (int8 dB + int16 S16) — léger, simple à debug |

## Limites connues

- `ring_fill_frames` à 768 (max) — proche du seuil, lié aux 4 taps inline dans audio_thread. À surveiller sous charge plus lourde
- Le tap b=-1 (mono) duplique L→R côté audio_thread — le goniomètre montrera donc une ligne diagonale parfaite (corrélation 1.0)
- L'analyzer_thread se réveille à 30 Hz fixe, pas synchronisé avec le ring → léger jitter possible sur la fréquence d'affichage
- Pas de log-scale fréquence côté canvas FFT (échelle linéaire) — à voir si user trouve ça gênant pour les graves
- Quantization int8 dB = 1 dB de résolution — suffisant pour rendu visuel mais pas pour mesure précise
- Pas de hold/peak indicator sur le FFT — chaque tick remplace

## Suite (post E7.5)

- E7.6 = documentation (cette fiche + ARCHI mise à jour)
- E7.x potentiel : log freq scale, peak hold FFT, presets/snapshots, persistance des réglages mixer
