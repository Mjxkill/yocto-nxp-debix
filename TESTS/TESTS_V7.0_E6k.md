# Test Fiche : V7.0 — E6.k (mixer-pro — N_PERIODS=2 seul) — **KO REVERT**

**Date** : 2026-05-11
**Statut** : **KO — non commité** (working tree restored)
**Tag git** : aucun (échec)

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E6.k N_PERIODS=2 SEUL (sans drainage NONBLOCK ni blocking) |
| Préalable | E6.h logic mixer-pro.c conservée 1:1 |
| Branche yocto | `feature/v7.0-multiband-drc-tap` |
| Pas de modif SOF/kernel/DT/firmware | 1 define dans mixer-pro.h |

## Hypothèse testée

> Garder la logique play_thread blocking E6.h (pop 1 / cycle) mais réduire l'ALSA
> buffer de 8 ms (N_PERIODS=4) à 4 ms (N_PERIODS=2). Gain théorique : -4 ms
> directs sur le `play_delay_frames` (288 → 192). Ring stable à 6-8 ms inchangé.
> Total mixer attendu : 10-12 ms (vs 14 ms E6.h).

## Implémentation

Diff minimal :
```c
-#define N_PERIODS       4       /* 8 ms */
+#define N_PERIODS       2       /* 4 ms */
```

Plus bump version `"v7.0-e6k"`. **Aucun changement** sur `mixer-pro.c`.

## Mesures empiriques (DSP-only, --no-uac2 --no-phone, board reboot propre)

### Démarrage initial

| t | frames | Δ Hz | xrun | Δ xrun /s | ring_fill | play_delay | latency µs | drops |
|---|---|---|---|---|---|---|---|---|
| 5s | 229056 | — | 2 | — | 384 | 192 | 4000 | 2208 |
| 8s | 373344 | 48 096 | 2 | 0 | 96 | 192 | 4000 | 2208 |
| 13s | 613440 | 48 019 | 3 | 0.2 | 384 | 192 | 4000 | 4416 |
| 18s | 853728 | 48 058 | 3 | 0 | 384 | 192 | 4000 | 4416 |

À ce stade : prometteur (xrun delta ~0, drops bornés à init).

### Steady long terme

| t | frames | Δ Hz | xrun | Δ xrun | ring_fill | drops | Δ drops /s |
|---|---|---|---|---|---|---|---|
| ~28s | 2345856 | — | 36 | — | 672 | 88224 | — |
| ~38s | 2825952 | 48 010 | 36 | 0 | **768 (max)** | 88416 | 19 |
| ~48s | 3306144 | 48 019 | 36 | 0 | **768 (max)** | 88608 | 19 |

**Régression après warmup** : ring saturé au MAX (768 frames) + drops continus
19/s steady (pas init seulement).

## Verdict

| Métrique | E6.h ref | E6.k mesuré (steady) | Verdict |
|---|---|---|---|
| Throughput | 48224 Hz | 48019 Hz | ✓ |
| Δ xrun /s | 0 | 0 | ✓ |
| **ring_fill steady** | **288-384 (6-8 ms)** | **672-768 (14-16 ms)** | **❌ régression +8 ms** |
| play_delay | 288 (6 ms) | 192 (4 ms) | ✓ -2 ms |
| **Latence côté play (ring + ALSA)** | **12-14 ms** | **18-20 ms** | **❌ régression +6 ms** |
| Δ ring_drops /s steady | 0 | **19/s continu** | **❌** |

## Cause racine

Réduire `N_PERIODS` de 4 à 2 transfère le tampon de ALSA vers le ring SPSC.

- En E6.h : ALSA buffer 8 ms peut absorber les jitters de scheduling → ring reste à
  6-8 ms steady.
- En E6.k : ALSA buffer 4 ms se remplit plus vite → `snd_pcm_writei` blocking
  attend plus tôt → audio_thread continue à push → ring se remplit jusqu'au
  maximum (768 frames = 16 ms = N_RING_PERIODS × PERIOD_FRAMES).

L'équilibre dynamique se stabilise au **maximum du ring**, pas à 6-8 ms comme
en E6.h. Drops continus parce que prefill initial dépasse RING_FRAMES dans
les premières secondes.

Effet net :
- ring_fill : +8 ms (régression)
- play_delay : -2 ms (gain ALSA)
- **Total côté play : +6 ms régression**

## Leçon apprise

**Le ring SPSC se remplit pour combler la réduction de l'ALSA buffer.** Le ring
et l'ALSA buffer sont communicants dans un système producteur/consommateur
self-régulé à 48 kHz. Réduire un sans modifier l'autre rééquilibre par le bas
le total = inchangé ou pire.

Pour vraiment réduire la latence userspace, il faudrait :
1. Limiter le ring SPSC (`N_RING_PERIODS=2`) — mais drops massifs sur recover SOF
2. Forcer le drain plus vite que la production — impossible avec writei blocking,
   et NONBLOCK ne marche pas sur SOF (E6.j KO)
3. Une autre voie architecturale (mixer DSP SOF, ou kernel patch)

## Action exécutée

```bash
ssh root@192.168.0.9 'pkill -9 mixer-pro'
git restore meta-local/recipes-audio/mixer-pro/files/mixer-pro.h
```

Pas commité — modif `mixer-pro.h` annulée en working tree.

## Conclusion

E6.k **régresse vs E6.h**. Le tampon total cap+play est conservatif : on ne peut
pas le réduire en touchant un seul paramètre. La cible < 10 ms acoustique
nécessite une autre approche (architecture) — voir investigation E6.l à lancer.
