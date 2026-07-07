# Test Fiche : V8.1 — UAC2 isolation par threads dédiés

**Date** : 2026-05-14
**Statut** : **PENDING USER VALIDATION** — code compile-clean, à valider board
**Commit** : (à committer)

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V8.1 — résilience UAC2 ↔ DSP |
| Préalable | V8.0-E1 GO (UAC2 activé) + WirePlumber persistence |
| mixer-pro version | `v8.1-uac2-isolated` |
| Diff | `mixer-pro.c` +200 lignes (2 threads + ring SPSC dédiés) |

## Cause racine diagnostiquée

Au démarrage avec UAC2 activé, mixer-pro entre en cycle de XRUN cascade :
- `audio_thread` appelle `snd_pcm_start(play_dsp)` → descend DPCM trigger → tac5212_trigger → **`msleep(5ms)`**
- Pendant ces 5 ms, le DMA SAI7 a déjà commencé à pousser des samples → **cap_dsp + cap_uac2 partent en XRUN immédiat** (buffer 8 ms saturé par 2 sources)
- Le recover ALSA relance le start → re-msleep → re-XRUN — boucle infinie
- Avec `--no-uac2` (V7.0), 1 seule source absorbait les 5 ms → pas de cascade

Stack trace audio_thread observée :
```
msleep+0x2c
tac5212_trigger+0x80   ← driver TAC5212 .trigger callback
snd_soc_pcm_dai_trigger+0x1fc
dpcm_be_dai_trigger+0x534
dpcm_fe_dai_do_trigger+0x94
snd_pcm_do_start+0x44
```

## Architecture V8.1

**Principe utilisateur** : "Règle 1 : un ALSA ne doit pas perturber un autre ALSA. Libère tout avec les threads qui poussent les data au mixer. Si il y a rien à pousser, c'est la valeur 0."

| Thread | Prio | Rôle V8.1 |
|---|---|---|
| `audio_thread` | RT 80 | DSP cap (master clock) + Phone cap NONBLOCK + mix + push ring DSP play |
| `play_thread` | RT 81 | Pop ring DSP play → write DSP play BLOCKING |
| **`cap_uac2_thread` NEW** | RT 80 | Own hw:UAC2Gadget,0 cap BLOCKING → push ring_uac2_cap |
| **`play_uac2_thread` NEW** | RT 80 | Pop ring_uac2_play → write hw:UAC2Gadget,0 play BLOCKING |
| `control_thread` | OTHER | inchangé |
| `analyzer_thread` | RT 60 | inchangé |

## Ring SPSC dédié par direction UAC2

```c
#define UAC2_RING_PERIODS  8           /* = 16 ms tolerance jitter */
#define UAC2_RING_FRAMES   (PERIOD_FRAMES * UAC2_RING_PERIODS) = 768
#define UAC2_CH            8

typedef struct {
    int32_t      buf[UAC2_RING_FRAMES * UAC2_CH];   /* 24 KB */
    atomic_uint  wr, rd;
    atomic_ulong drops;
    atomic_ulong xruns;
} uac2_ring_t;

static uac2_ring_t g_ring_uac2_cap;    /* producteur cap_uac2_thread, consommateur audio_thread */
static uac2_ring_t g_ring_uac2_play;   /* producteur audio_thread, consommateur play_uac2_thread */
```

Hot-path lockfree, indices `_Atomic` avec acquire/release ordering. Pattern éprouvé sur le ring DSP existant depuis E6.g Phase 2.

## Modifications fichier `mixer-pro.c`

1. **Nouvelles structs + helpers** (lignes ~135-280) :
   - `uac2_ring_t` × 2
   - `uac2_ring_pop_period(ring, out)` : copie 1 period ou silence si vide
   - `uac2_ring_push_period(ring, in)` : copie 1 period ou drop oldest si full
2. **Nouveau `cap_uac2_thread`** : own le PCM, snd_pcm_readi BLOCKING infinite loop, push ring
3. **Nouveau `play_uac2_thread`** : prefill (N_PERIODS-1) zeros, pop ring, snd_pcm_writei BLOCKING infinite loop
4. **`audio_thread`** : retrait des appels UAC2 directs
   - Plus de `snd_pcm_nonblock(cap_uac2)` ni `snd_pcm_nonblock(play_uac2)` (owned ailleurs)
   - Plus de prefill UAC2 play / start UAC2 cap (faits dans threads dédiés)
   - `snd_pcm_readi(cap_uac2)` → `uac2_ring_pop_period(&g_ring_uac2_cap, cap_uac2_buf)`
   - `snd_pcm_writei(play_uac2)` → `uac2_ring_push_period(&g_ring_uac2_play, play_uac2_buf)`
5. **`main()`** : `pthread_create(cap_uac2_thread) + pthread_create(play_uac2_thread)` si `!g_skip_uac2`
6. **`MIXER_VERSION`** : `v8.0-e1` → `v8.1-uac2-isolated`

DSP path totalement inchangé (audio_thread + play_thread + ring_dsp). Si V8.1 régresse côté DSP, c'est purement à cause des nouveaux threads (suspects pour debug).

## Résilience attendue

| Scénario | V7/V8.0 actuel | V8.1 |
|---|---|---|
| tac5212_trigger msleep 5 ms au start | Cascade XRUN DSP + UAC2 | Seul DSP delayed ; cap_uac2 lit en // ; ring se remplit |
| USB unplug à chaud | mixer-pro die ou xrun cascade | cap_uac2 voit -ENODEV, recover, retry. DSP intact |
| Host PipeWire suspend (iso errors flood) | XRUN propagation DSP play | play_uac2 NONBLOCK loop, drops, DSP play continu |
| xrun cap_dsp ponctuel | xrun cap_uac2 propagé | recover local cap_dsp, cap_uac2 indépendant |
| Bitwig joue audio vers Debix | XRUN cascade infinie observée | Signal arrive dans U1..U8 du GUI mixer-pro |

## Tests T8.1.X à exécuter sur board

| Test | Procédure | Résultat attendu |
|---|---|---|
| T8.1.1 boot clean | reboot board | mixer-pro UP en < 5 s, version `v8.1-uac2-isolated` dans logs |
| T8.1.2 logs démarrage | `journalctl -u mixer-pro` | `cap_uac2_thread : SCHED_FIFO prio 80` + `play_uac2_thread : ... prio 80` |
| T8.1.3 PCMs cap status | board : `cat /proc/asound/card2/pcm0c/sub0/status` | state: **RUNNING** (pas XRUN persistant) |
| T8.1.4 PCMs play status | idem card 2 pcm0p, card 3 pcm0c/p | tous **RUNNING** ou PREPARED transition court |
| T8.1.5 iso errors USB | board : `dmesg --since="1 minute ago" \| grep "iso_complete status(-18)" \| wc -l` | < 10 / min (selon stream actif côté host) |
| T8.1.6 Bitwig audio playback | PC : Bitwig joue un track via sink Debix Pro Audio | board : U1..U8 dans GUI mixer-pro montrent du signal |
| T8.1.7 Routing U → S | GUI : strip U1 sidebar → master U1 → S1 à 0 dB | speaker S1 (TAC0 L) sort le son |
| T8.1.8 mic record back to PC | M1 (mic DSP 1) → master → U1 (UAC2 out 1) à 0 dB, host : `arecord -D hw:1,0 -c 8 -f S32_LE -r 48000 -d 3 /tmp/back.wav` | wav 8ch capture le mic 1 |
| T8.1.9 USB unplug à chaud | débrancher câble USB pendant Bitwig joue | mixer-pro reste UP, DSP intact, GUI U1..U8 passent à silence |
| T8.1.10 USB replug | rebrancher câble | cap_uac2_thread recover, signal U1..U8 revient |

## Risques connus

1. **Drift d'horloges** : DSP = SAI7 PLL (board), UAC2 = host PC USB SOF. Asynchrones. Pour un stream long (> 5 min), peut générer 1-2 drops/min audibles si drift > 50 ppm. Acceptable en V8.1 ; ASRC libsamplerate en V8.2 si dérangeant.
2. **Latence UAC2 augmentée** : était ~5 ms (NONBLOCK in audio_thread). V8.1 : 8 ms ring + 2 ms period = ~10-12 ms UAC2 path. DSP path inchangé < 10 ms.
3. **Ring drops** : exposés via compteurs `g_ring_uac2_cap.drops` et `xruns`. Pas encore exposés via JSON (TODO V8.2).

## Validation utilisateur

> "OK / KO — précisions"

→ **À renseigner après reboot board + test Bitwig**

## Suite

- Si T8.1.6 GO → on commit + push, sprint suivant **E2 loads CPU/DSP** comme planifié
- Si T8.1.6 KO → diag avec stack traces threads, ne pas reverter
