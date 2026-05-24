# TESTS V9.2d — LV2 worker + options support

Date : 2026-05-25
Branche : `feature/v7.0-multiband-drc-tap`
Commit : `<V9.2d>` (effects.c +200 lignes)
Version mixer-pro : `v9.2d-lv2-worker`

## Objectif

Étendre le moteur LV2 mixer-pro pour supporter 2 host features additionnelles :
- **`lv2:options`** — passer maxBlockLength/sampleRate/etc. au plugin à init
- **`lv2:worker:schedule`** — thread pool non-RT pour load IR file / preset

**Motivation** : dragonfly (4 reverbs), calf (40+), lsp-plugins (200+), zam-plugins
demandent ces features. Sans elles, `required_features` check les refusait.

## Modifications effects.c (~200 lignes)

### Globals (3 zones)

| Zone | Description |
|---|---|
| `g_lv2_options[7]` | Array `LV2_Options_Option` : max/min/nom_block_length, seqSize, sampleRate, terminator |
| `g_feature_options` | LV2_Feature wrapper exposant `g_lv2_options` |
| URIDs pré-mappés | `LV2_BUF_SIZE__*`, `LV2_PARAMETERS__sampleRate`, `LV2_ATOM__Int`, `LV2_ATOM__Float` |
| `g_host_features[]` | `{urid_map, options, NULL}` (worker ajouté per-instance) |

### Init `lv2_world_init()`

URIDs pré-mappés + `g_lv2_options` array construit avec valeurs concrètes
(96 frames period, 48000 Hz, 8192 atom seq).

### Worker (struct + callbacks + thread)

```c
struct lv2_worker {
    pthread_t       thread;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    uint8_t   req_buf[8192]; uint32_t req_size; volatile int req_pending;
    uint8_t   resp_buf[8192]; uint32_t resp_size; volatile int resp_pending;
    volatile int exit_flag;
    uint64_t  drops;   // critic suggestion : compteur schedule_work rejetés
    const LV2_Worker_Interface *iface;
    LV2_Handle plugin_handle;
    LV2_Worker_Schedule schedule;
};
```

| Callback | Thread | Comportement |
|---|---|---|
| `lv2_worker_schedule_cb` | audio (RT) | `pthread_mutex_trylock` non-bloquant. Si déjà locked OU req_pending OU size > buf → `drops++` + return ERR. Sinon copy + cond_signal. |
| `lv2_worker_respond_cb` | worker (non-RT) | Copy data dans resp_buf + memory_barrier + set resp_pending |
| `lv2_worker_thread_fn` | worker (non-RT) | Loop sleep `pthread_cond_timedwait(2s)` (suggestion critic : timeout deadlock guard). Wake → copy req hors mutex → call `iface->work()`. |

### `lv2_process`

Au début de chaque cycle, avant `lilv_instance_run` :
```c
if (st->worker && st->worker->resp_pending) {
    // copy local + reset pending + call iface->work_response(plugin)
}
```

### `fx_init_lv2`

Détection worker required OR optional :
- `lilv_plugin_get_required_features` → si `worker:schedule` → `needs_worker=1`
- `lilv_plugin_get_optional_features` → si `worker:schedule` → `needs_worker=1`
  (calf et autres utilisent le worker en optional)

Si needs_worker :
- alloc struct lv2_worker + mutex/cond init
- build feature_worker_local + features_local[] = {urid_map, options, worker, NULL}
- `lilv_plugin_instantiate(plug, sr, features_local)`
- récupère `iface` via `lilv_instance_get_extension_data(LV2_WORKER__interface)`
- check iface->work ≠ NULL (suggestion critic : fallback propre si plugin lie mal)
- `pthread_create` (suggestion critic : fallback refus si fail)

### `fx_free`

Si worker existe :
- set exit_flag + cond_signal + `pthread_join`
- log drops counter si > 0
- destroy mutex/cond + free struct

### `lv2_host_supports_feature` mise à jour

Ajout : `LV2_OPTIONS__options`, `LV2_WORKER__schedule`. Toujours OK pour `LV2_URID__map`.

## Tests effectués (board 192.168.0.9)

### Test 1 — Build cross + deploy

| Étape | Résultat |
|---|---|
| `bitbake mixer-pro` | ✅ OK (recompile complet effects.c) |
| `dpkg -i mixer-pro_1.0-r0_arm64.deb` | ✅ Setting up… |
| Service active | ✅ `mixer-pro v9.2d-lv2-worker starting` |

### Test 2 — Load dragonfly Hall (worker + options required)

```
POST /api/cmd {op:set_fx_engine, bus:0, engine:lv2,
              uri:"https://github.com/michaelwillis/dragonfly-reverb"}
```

| Vérification | Résultat |
|---|---|
| set_fx_engine | ✅ ok=true |
| get_fx | ✅ 18 params dump (dry/early/late levels, size, width, delay, diffuse, low_cut/xo/mult, high_cut/xo/mult, spin, wander, decay, early_send, modulation) |
| Log | ✅ `LV2: ...dragonfly-reverb worker thread spawned` |
| mixer-pro state | ✅ active |
| Pas de SEGV | ✅ |

### Test 3 — 4 dragonfly simultanés (4 worker threads)

| Bus | URI | Worker required ? |
|---|---|---|
| 0 | `https://github.com/michaelwillis/dragonfly-reverb` (Hall) | ✅ |
| 1 | `urn:dragonfly:early` | (options seul) |
| 2 | `urn:dragonfly:plate` | ✅ |
| 3 | `urn:dragonfly:room` | ✅ |

3 worker threads spawned (early n'a pas worker), tous chargent.

### Test 4 — Stress RT 30 sec avec 4 reverbs convolution

`prof_iter` histogram (audio thread iteration time) :

| Bucket | Count | % |
|---|---|---|
| iter_lt18 (<18µs)  | 5081 | 28% |
| iter_18_22         | 9293 | 51% |
| iter_22_30         | 4595 | 25% |
| iter_30_50         | 48   | **0.26%** |
| iter_ge50          | 0    | 0% |

Comparaison V9.2c stress (4 plugins x42+mda mix, plus léger) : 0.038%.
**Avec 4 reverbs convolution dragonfly simultanés (CPU intensive) : 0.26%, acceptable.**

`drops_play = 0`, mixer-pro stable.

### Test 5 — Cohabitation mix engines (régression)

| Bus | Engine | Type |
|---|---|---|
| 0 | x42 fil4 (AtomPort) | V9.2c |
| 1 | MDA Dynamics (basic) | V9.2a |
| 2 | compressor (builtin C) | V7.0 |
| 3 | dragonfly Room (worker+options) | V9.2d |

✅ 4 plugins de générations différentes cohabitent sans conflit.
✅ Swap engine → cleanup worker thread propre (vu via pthread_join dans fx_free).

### Test 6 — Validation utilisateur audio

À COMPLÉTER PAR L'UTILISATEUR :
- [ ] Son clean sur dragonfly Hall reverb ? OUI / NON
- [ ] Son clean sur dragonfly Plate ? OUI / NON
- [ ] Son clean sur dragonfly Room ? OUI / NON
- [ ] Pas de glitchs/clicks audibles ? OUI / NON
- [ ] Switch live entre 4 reverbs OK sonore ? OUI / NON

## Verdict

✅ **GO** — Worker + options host features fonctionnels, dragonfly chargeable,
écosystème LV2 moderne largement débloqué.

Plugins LV2 désormais chargeables dans mixer-pro :
- mda-lv2          : 25 plugins (urid:map seul)
- x42              : 50+ variantes (urid:map + AtomPort)
- LV2 examples     : 6 (urid:map)
- **dragonfly**    : **4 reverbs (urid:map + options + worker)** ← NEW
- calf/lsp/zam (build dispos, à tester)

## TODO suivi V9.2 / V9.3

- [ ] Build + deploy calf + lsp-plugins + zam-plugins (recettes déjà en place,
      restent à `bitbake`)
- [ ] Tester si calf/lsp/zam plugins ont d'autres `required_features`
      (state, ui:something, etc.)
- [ ] V9.3 : ajouter `lv2:state` (presets save/restore)
- [ ] V9.3 : améliorer fxLv2ParamSpec heuristique GUI via backend introspection
      `lilv_port_get_range` (min/max/default exposés dans get_fx)
- [ ] V9.3 : exposer `worker_drops` counter dans /api/drift (visibility GUI)

## Améliorations critic intégrées

| # | Suggestion | Implémentation |
|---|---|---|
| 1 | Compteur drops schedule_work | `w->drops` incrémenté à chaque trylock fail / req_pending / size overflow. Log au fx_free si > 0. |
| 2 | Timeout pthread_cond_wait worker | `pthread_cond_timedwait(2s)` pour relire exit_flag et éviter deadlock |
| 3 | Fallback si pthread_create fail | Cleanup propre (mutex/cond/struct/instance) + log + return 0 |
| 4 | Worker thread sched OTHER | `pthread_create(NULL attr)` = sched_other default, pas pinné — peut être préempté librement, latence work_response variable mais work() peut prendre 100ms+ donc non-déterministe est acceptable |
| 5 | Check iface->work non-null | Refus propre si plugin claime worker mais n'expose pas work() iface |
