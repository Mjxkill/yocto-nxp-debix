# V6.0 — Plan détaillé des patches à implémenter

**Branche** : `feature/v6-always-on-async` (créée 2026-05-06 depuis c87b07465)
**Architecture cible** :

```
PIPE 1 (always-on, démarre au boot DSP, ne s'arrête JAMAIS) :
  SAI RX 8ch → multiband_drc×8 + drc×8 + pga×8 (strips IN)
            → matrix_2x8 (16×8, src0 = ASIO play, src1 = mics)
            → multiband_drc×8 + pga×8 + drc×8 (strips OUT)
            → NPU_TAP (hook dai_dma_cb)
            → SAI TX 8ch

PIPE 2 (PCM cap = ASIO IN, on-demand piggyback sur PIPE 1) :
  tap intra-PIPE 1 entre strips IN et matrix → host buffer ALSA → PCM 0

PIPE 3 (PCM play = ASIO OUT, on-demand piggyback sur PIPE 1) :
  PCM 1 → host buffer ALSA → matrix_2x8 src0 (PIPE 1)
```

---

## A. Patches firmware DSP (SOF, fork Mjxkill, branche feature/v6-always-on-async)

### F1 — Flags d'attributs pipeline
**Fichier** : `src/include/sof/audio/pipeline.h`
**Type** : ajout de constantes
**Code** :
```c
/* Pipeline attributes (lecture champ uint32_t attributes existant) */
#define PIPELINE_ATTR_ALWAYS_ON    BIT(0)  /* auto-start après tplg load */
#define PIPELINE_ATTR_IGNORE_STOP  BIT(1)  /* ignore COMP_TRIGGER_STOP/PAUSE */
#define PIPELINE_ATTR_NO_HOST      BIT(2)  /* DAI-to-DAI, pas de PCM trigger */
```

### F2 — Auto-start après topology load
**Fichier** : `src/ipc/ipc3/handler.c` (dans `ipc_glb_tplg_pipe_complete`, après ligne 1364 environ)
**Type** : modification fonction existante
**Code** :
```c
ipc_pipe = ipc_get_comp_by_id(ipc, ipc_pipeline.comp_id);
if (ipc_pipe && ipc_pipe->pipeline) {
    struct pipeline *p = ipc_pipe->pipeline;

    if (p->attributes & PIPELINE_ATTR_ALWAYS_ON) {
        struct comp_dev *anchor = p->source_comp ? : p->sink_comp;

        ret = pipeline_prepare(p, anchor);
        if (ret < 0)
            tr_err(&ipc_tr, "always_on: prepare failed for ppl %d ret=%d",
                   p->pipeline_id, ret);
        else {
            ret = pipeline_trigger(p, anchor, COMP_TRIGGER_PRE_START);
            if (!ret)
                ret = pipeline_trigger(p, anchor, COMP_TRIGGER_START);
            tr_info(&ipc_tr, "always_on: ppl %d auto-started ret=%d",
                    p->pipeline_id, ret);
        }
    }
}
```

### F3 — Ignorer STOP/PAUSE sur pipeline always-on
**Fichier** : `src/audio/pipeline/pipeline-stream.c` (début de `pipeline_trigger_run`, après pipe_dbg)
**Type** : modification fonction existante
**Code** :
```c
if ((p->attributes & PIPELINE_ATTR_IGNORE_STOP) &&
    (cmd == COMP_TRIGGER_STOP || cmd == COMP_TRIGGER_PAUSE)) {
    pipe_info(p, "ppl %d always-on, ignoring STOP/PAUSE",
              p->pipeline_id);
    return PPL_STATUS_PATH_STOP;
}
```

### F4 — Nouveau IPC `SOF_IPC_PIPE_TRIGGER`
**Fichiers** : `src/include/ipc/header.h` + `src/ipc/ipc3/handler.c`
**Type** : nouveau message IPC + nouveau handler
**Code header** :
```c
/* Nouveau cmd IPC3 — trigger pipeline par pipeline_id (pas comp_id) */
#define SOF_IPC_PIPE_TRIGGER (SOF_IPC_GLB_PIPE | 0x100)

struct sof_ipc_pipe_trigger {
    struct sof_ipc_cmd_hdr hdr;
    uint32_t pipeline_id;
    uint32_t cmd;  /* COMP_TRIGGER_START / STOP / RESET */
} __packed;
```
**Code handler** :
```c
static int ipc_pipe_trigger(uint32_t header)
{
    struct sof_ipc_pipe_trigger msg;
    struct ipc_comp_dev *ipc_pipe;
    struct pipeline *p;
    int ret;

    ret = ipc_get_payload(&msg, sizeof(msg));
    if (ret < 0)
        return ret;

    ipc_pipe = ipc_get_pipeline_by_id(ipc_get(), msg.pipeline_id);
    if (!ipc_pipe || !ipc_pipe->pipeline)
        return -EINVAL;

    p = ipc_pipe->pipeline;
    return pipeline_trigger(p, p->source_comp ? : p->sink_comp, msg.cmd);
}

/* Dispatch dans ipc_glb_pipe_message() */
case SOF_IPC_PIPE_TRIGGER:
    return ipc_pipe_trigger(header);
```

### F5 — `module_adapter_set_state` : tolérer multi-source intra-pipeline
**Fichier** : `src/audio/module_adapter/module_adapter_ipc3.c` (modification branche `num_of_sources > 1` ligne 142+)
**Type** : modification fonction existante
**Code** :
```c
if (mod->num_of_sources > 1) {
    bool sources_active;
    bool all_intra;
    int ret;

    sources_active = module_source_status_count(dev, COMP_STATE_ACTIVE) ||
                     module_source_status_count(dev, COMP_STATE_PAUSED);

    /* NEW: si toutes sources sont intra-pipeline, traiter comme single-source */
    all_intra = module_sources_all_intra_pipeline(dev);
    if (all_intra)
        return comp_set_state(dev, cmd);

    /* Fix-B existant pour cross-pipeline */
    if ((cmd == COMP_TRIGGER_STOP || cmd == COMP_TRIGGER_PRE_START) &&
        sources_active && dev->state == COMP_STATE_ACTIVE) {
        return PPL_STATUS_PATH_STOP;
    }

    ret = comp_set_state(dev, cmd);
    if (ret == COMP_STATUS_STATE_ALREADY_SET)
        return PPL_STATUS_PATH_STOP;

    return ret;
}
```
**Helper à créer** dans `module_adapter_ipc3.c` :
```c
static bool module_sources_all_intra_pipeline(struct comp_dev *dev)
{
    struct list_item *blist;
    struct comp_buffer *cb;

    list_for_item(blist, &dev->bsource_list) {
        cb = container_of(blist, struct comp_buffer, sink_list);
        if (cb->source && cb->source->pipeline != dev->pipeline)
            return false;
    }
    return true;
}
```

### F6 — `matrix_2x8` silence-on-empty (output-driven garanti)
**Fichier** : `src/audio/matrix_2x8/matrix_2x8.c`
**Type** : modification fonction `matrix_2x8_process`
**Code** (à insérer après calcul `nb_frames`, avant la boucle de mix) :
```c
/* V6.0: garantie output-driven — si toutes sources vides ET nb_frames>0,
 * remplir le sink avec du silence. Sans ça, le SAI TX DMA stalle au
 * cold-boot quand B0 (host) et B5 (cross-pipeline) sont vides au 1er tick.
 */
{
    bool all_empty = true;
    int s;

    for (s = 0; s < num_input_buffers; s++) {
        if (input_buffers[s].size > 0) {
            all_empty = false;
            break;
        }
    }
    if (all_empty && nb_frames > 0) {
        audio_stream_set_zero(sink_stream, nb_frames * sink_frame_bytes);
        output_buffers[0].size = nb_frames * sink_frame_bytes;
        return 0;
    }
}
```

---

## B. Patches kernel ASoC (Linux, dans le BSP NXP)

### K1 — `sound/soc/sof/imx/imx8m.c` — trigger always-on au probe
**Type** : modification fonction `imx8m_post_fw_run`
**Code** :
```c
static int imx8m_post_fw_run(struct snd_sof_dev *sdev)
{
    int ret = sof_post_fw_run(sdev);
    if (ret) return ret;

    return sof_trigger_always_on_pipelines(sdev);
}
```

### K2 — `sound/soc/sof/ipc3.c` — nouveau helper
**Type** : nouvelle fonction
**Code** :
```c
int sof_trigger_always_on_pipelines(struct snd_sof_dev *sdev)
{
    struct snd_sof_widget *swidget;
    struct sof_ipc_pipe_trigger msg = {
        .hdr.size = sizeof(msg),
        .hdr.cmd = SOF_IPC_GLB_PIPE | SOF_IPC_PIPE_TRIGGER,
    };
    int ret;

    list_for_each_entry(swidget, &sdev->widget_list, list) {
        struct snd_sof_pipeline *spipe;

        if (swidget->id != snd_soc_dapm_scheduler)
            continue;

        spipe = swidget->spipe;
        if (!spipe || !spipe->always_on)
            continue;

        msg.pipeline_id = swidget->pipeline_id;
        msg.cmd = COMP_TRIGGER_START;

        ret = sof_ipc_tx_message_no_reply(sdev->ipc, &msg, sizeof(msg));
        if (ret < 0) {
            dev_err(sdev->dev, "always_on trigger ppl %d failed %d\n",
                    swidget->pipeline_id, ret);
            return ret;
        }
        dev_info(sdev->dev, "always_on: pipeline %d triggered\n",
                 swidget->pipeline_id);
    }
    return 0;
}
```

### K3 — `sound/soc/sof/topology.c` — parser le token
**Code** (ajout dans le tableau `pipeline_tokens` ou similaire) :
```c
SOF_TOPOLOGY_TOKEN(SOF_TKN_PIPE_ALWAYS_ON, SND_SOC_TPLG_TUPLE_TYPE_BOOL,
                   get_token_uint32_t,
                   offsetof(struct snd_sof_pipeline, always_on))
```

### K4 — `sound/soc/sof/pcm.c` — ne pas propager STOP au DAI always-on
**Code** dans `sof_pcm_trigger` :
```c
if (cmd == SNDRV_PCM_TRIGGER_STOP || cmd == SNDRV_PCM_TRIGGER_PAUSE_PUSH) {
    /* skip si pipeline associée flagged always-on (ne jamais arrêter le DAI) */
    if (spcm->stream[substream->stream].pipeline_always_on) {
        dev_dbg(component->dev, "ppl always-on, skip STOP IPC\n");
        return 0;
    }
}
```

### K5 — `include/sound/sof.h` — flag dans struct
**Code** (ajout dans `struct snd_sof_pipeline`) :
```c
struct snd_sof_pipeline {
    /* existing fields... */
    bool always_on;  /* NEW: pipeline démarre au probe DSP, ignore STOP */
};
```

---

## C. Patches topology m4 (dans Mjxkill/sof tools/topology/)

### T1 — Nouveau token `SOF_TKN_PIPE_ALWAYS_ON`
**Fichier** : `tools/topology/topology1/m4/sof/tokens.m4`
**Code** :
```m4
define(`SOF_TKN_PIPE_ALWAYS_ON', 224)
```

### T2 — Macro `PIPELINE_ALWAYS_ON_ADD`
**Fichier** : `tools/topology/topology1/m4/pipeline.m4`
**Code** : dérivée de `PIPELINE_PCM_ADD`, sans widget HOST + token always_on=1

### T3 — Pipeline DAI-to-DAI loopback avec strips
**Fichier** : `tools/topology/topology1/sof/pipe-dai-to-dai-loopback.m4` (nouveau)
**Contenu** : SAI RX → strips IN (multiband_drc×8 + drc×8 + pga×8) → matrix_2x8 → strips OUT (multiband_drc×8 + pga×8 + drc×8) → SAI TX, sans HOST

### T4 — Pipeline ASIO IN (host capture seulement)
**Fichier** : `tools/topology/topology1/sof/pipe-host-only-capture.m4` (nouveau)
**Contenu** : juste host capture + buffer, source = tap intra-PIPE 1 via SectionGraph

### T5 — Pipeline ASIO OUT (host playback seulement)
**Fichier** : `tools/topology/topology1/sof/pipe-host-only-playback.m4` (nouveau)
**Contenu** : juste host playback + buffer, sink = matrix_2x8 src0 dans PIPE 1 via SectionGraph

### T6 — Topologie complète V6.0
**Fichier** : `tools/topology/topology1/sof-imx8mp-tac5212-V6.0.m4` (nouveau)
**Contenu** : assemblage T3 + T4 + T5 + SectionGraph cross-pipeline + DAI_CONFIG SAI7 + PCM_CAPTURE_ADD/PCM_PLAYBACK_ADD

---

## Ordre d'application + tests (FINAL post-investigation 69bcdeed)

| Étape | Patches | Test | Critère |
|---|---|---|---|
| **0** | **multiband_drc per-channel** (porter pattern DRC D3 ea984a266 à multiband_drc_generic.c) | Tests existants drc D3 sur multiband_drc avec 8 configs blob | Audio passthrough OK, 8 configs indépendantes |
| 1.1 | F6 corrigé seul (variables `min_frames` + `consumed=0`) | Cold-boot E6b-pivot 1er aplay | I/O error disparaît (DMA TX alimenté en silence) |
| 1.2 | F5 + F6 | Cold-boot E6b-pivot + son | TAC5212 reçoit du son |
| 2.1 | F1 + F3 | E6a + flag always-on hardcodé | Pipeline marquée mais pas auto-démarrée |
| 2.2 | F1 + F2 + F3 | E6a + flag always-on | Pipeline démarre au boot DSP |
| 3.1 | F4 + K1-K5 | E6a + IPC variant | Trigger via kernel post-fw-ready |
| 4.1 | T1 + T2 + T3 + T6 (PIPE 1 seule) | Loopback hw mics → SAI TX sans aucun PCM | Mics audibles dans speakers |
| 4.2 | + T4 | + PCM cap ASIO IN piggyback | arecord capture sans casser le loopback |
| 4.3 | + T5 + K4 | + PCM play ASIO OUT piggyback | aplay injecte sans casser le loopback |
| 5.1 | NPU tap test | dai_dma_cb hook actif | Samples capturés post-effets-OUT |
| 5.2 | amixer cset | gains modifiés en temps réel | Pas de glitch |
| 5.3 | aplay+arecord simultané | full duplex via PCMs séparés | Tout fonctionne |

---

## Plans de repli

- **Plan B (V6.0a)** : E6a + effets en sortie (multiband_drc×8 + pga×8 + drc×8). Pas d'always-on, pas de mics tap.
- **Plan C** : firmware DSP custom sans SOF (Zephyr-only sur M7), reprend uniquement les modules d'effets.

---

## Questions à valider auprès de l'investigation critic

1. **F2 (auto-start dans `ipc_glb_tplg_pipe_complete`)** : est-ce le bon hook ? `pipeline_complete()` est-il sûr d'avoir tous les comps prêts à START à ce stade ?
2. **F4 (nouveau IPC)** : conflit avec un cmd existant ? La structure `sof_ipc_pipe_trigger` est-elle bien alignée IPC3 ?
3. **F5 (helper `module_sources_all_intra_pipeline`)** : `cb->source->pipeline` est-il fiable pour cross-pipeline (vs single comp_dev cross-pipe via SectionGraph) ?
4. **F6 (silence-on-empty)** : la condition `all_empty` est-elle suffisante ? Faut-il aussi vérifier `output_buffers[0].size != 0` côté host ?
5. **K1-K5 (kernel)** : compatibles avec la version SOF kernel utilisée par NXP L6.12.3 ? `struct snd_sof_pipeline` a-t-il le champ `spipe` ?
6. **T2 macro** : faut-il déclarer un widget scheduler explicite ou peut-on dériver `PIPELINE_PCM_ADD` ?
7. **T3 (pipe-dai-to-dai)** : 8 instances `multiband_drc` mono ou 1 instance 8ch ? Quel impact perf DSP ?
8. **Ordre étapes 1-5** : peut-on tester F6 sans toucher au reste ? (test isolation)
