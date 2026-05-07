#!/usr/bin/env python3
"""
V6.0 always-on pipeline support: kernel-side ASoC patches K0+K2+K4+K5+K1.

Patches applied (idempotent, additive):

  K0  include/uapi/sound/sof/tokens.h
        Add SOF_TKN_PIPE_ALWAYS_ON (224) after SOF_TKN_SCHED_USE_CHAIN_DMA.

  K0  include/sound/sof/header.h
        Add SOF_IPC_TPLG_PIPE_TRIGGER (0x014) after SOF_IPC_TPLG_PIPE_COMPLETE.

  K0  include/sound/sof/topology.h
        Add struct sof_ipc_pipe_trigger after struct sof_ipc_pipe_ready.

  K4  sound/soc/sof/sof-audio.h
        Add `bool always_on;` to struct snd_sof_widget (after dynamic_pipeline_widget).

  K2  sound/soc/sof/ipc3-topology.c
        Add SOF_TKN_PIPE_ALWAYS_ON entry to pipeline_tokens[].

  K5  sound/soc/sof/ipc3-topology.c
        Static helper sof_ipc3_send_pipe_trigger(sdev, pipeline_id, cmd) that
        builds and sends SOF_IPC_TPLG_PIPE_TRIGGER.

  K1  sound/soc/sof/ipc3-topology.c
        At end of sof_ipc3_set_up_all_pipelines (non-verify path), iterate
        widgets, for each scheduler widget with always_on=1, send
        PRE_START + START via the K5 helper.

Usage: python3 apply-v6-always-on.py <kernel-source-dir>
"""
import sys
from pathlib import Path

KSRC = Path(sys.argv[1])

# Purge: remove any prior V6.0 patches before re-applying. This makes the
# patcher robust against re-runs on already-patched trees (e.g. Yocto work
# dir reused across builds), and ensures the LATEST script content always
# wins regardless of what was applied previously.
PURGE_FILES = [
    "include/uapi/sound/sof/tokens.h",
    "include/sound/sof/header.h",
    "include/sound/sof/topology.h",
    "sound/soc/sof/sof-audio.h",
    "sound/soc/sof/ipc3-topology.c",
]
def _strip(text: str, start_re: str, end_re: str) -> str:
    """Remove every block matching start_re...end_re inclusive."""
    import re
    pattern = re.compile(start_re + r".*?" + end_re, re.DOTALL)
    return pattern.sub("", text)

for rel in PURGE_FILES:
    p = KSRC / rel
    if not p.exists():
        continue
    text = p.read_text()
    orig_len = len(text)
    # Token uapi
    text = _strip(text, r"\n/\* V6\.0: pipeline always-on attribute[^\n]*\n",
                  r"#define SOF_TKN_PIPE_ALWAYS_ON\s+224\n")
    # IPC cmd (single line)
    import re
    text = re.sub(r"#define SOF_IPC_TPLG_PIPE_TRIGGER\s+SOF_CMD_TYPE\(0x014\)\n", "", text)
    # Struct (entire block)
    text = _strip(text, r"\n/\* V6\.0: trigger pipeline by[^\n]*\n",
                  r"struct sof_ipc_pipe_trigger \{[^}]*\}\s*__packed;\n")
    # Widget always_on field
    text = _strip(text,
                  r"\n\t/\* V6\.0: pipeline parsed as always-on[^\n]*\n",
                  r"\tbool always_on;\n")
    # Token table entry (K2)
    text = _strip(text,
                  r"\t\{SOF_TKN_PIPE_ALWAYS_ON, SND_SOC_TPLG_TUPLE_TYPE_BOOL, get_token_u16,\n",
                  r"\t\toffsetof\(struct snd_sof_widget, always_on\)\},\n")
    # K5 helper
    text = _strip(text,
                  r"/\* V6\.0: kernel-side firmware trigger for always-on pipelines \(K5\)\.\n",
                  r"static int sof_ipc3_send_pipe_trigger\(struct snd_sof_dev[^{]*\{[^}]*?\n\treturn ret;\n\}\n\n")
    # K1 trigger block
    text = _strip(text,
                  r"\n\n\t/\* V6\.0: trigger always-on pipelines now that all widgets are\n",
                  r"\t\}\n\t\}\n")
    if len(text) != orig_len:
        p.write_text(text)
        print(f"[purge] {rel}: removed {orig_len - len(text)} bytes of prior V6.0 patches")


def patch_once(path: Path, marker: str, anchor: str, insert: str, mode: str = "after") -> bool:
    """Idempotent text insertion. Returns True if patched, False if already present."""
    text = path.read_text()
    if marker in text:
        print(f"[skip] {path.relative_to(KSRC)}: marker '{marker[:40]}...' already present")
        return False
    if anchor not in text:
        sys.exit(f"[fail] {path.relative_to(KSRC)}: anchor '{anchor[:60]}...' not found")
    if mode == "after":
        new_text = text.replace(anchor, anchor + insert, 1)
    elif mode == "before":
        new_text = text.replace(anchor, insert + anchor, 1)
    else:
        sys.exit(f"[fail] unknown mode {mode}")
    path.write_text(new_text)
    print(f"[ok]   {path.relative_to(KSRC)}: inserted '{marker[:40]}...'")
    return True


# ---- K0a: include/uapi/sound/sof/tokens.h ----
patch_once(
    KSRC / "include/uapi/sound/sof/tokens.h",
    marker="SOF_TKN_PIPE_ALWAYS_ON",
    anchor="#define SOF_TKN_SCHED_USE_CHAIN_DMA\t\t209\n",
    insert="\n/* V6.0: pipeline always-on attribute (kernel triggers post-DAI_CONFIG) */\n"
           "#define SOF_TKN_PIPE_ALWAYS_ON\t\t\t224\n",
)

# ---- K0b: include/sound/sof/header.h ----
patch_once(
    KSRC / "include/sound/sof/header.h",
    marker="SOF_IPC_TPLG_PIPE_TRIGGER",
    anchor="#define SOF_IPC_TPLG_PIPE_COMPLETE\t\tSOF_CMD_TYPE(0x013)\n",
    insert="#define SOF_IPC_TPLG_PIPE_TRIGGER\t\tSOF_CMD_TYPE(0x014)\n",
)

# ---- K0c: include/sound/sof/topology.h ----
patch_once(
    KSRC / "include/sound/sof/topology.h",
    marker="struct sof_ipc_pipe_trigger",
    anchor="struct sof_ipc_pipe_free {\n\tstruct sof_ipc_cmd_hdr hdr;\n\tuint32_t comp_id;\n}  __packed;\n",
    insert="\n/* V6.0: trigger pipeline by scheduler comp_id - SOF_IPC_TPLG_PIPE_TRIGGER.\n"
           " * On PRE_START, params drive pipeline_params() on the firmware side\n"
           " * (mirroring the PCM hw_params flow). Layout MUST match firmware\n"
           " * src/include/ipc/topology.h struct sof_ipc_pipe_trigger.\n"
           " */\n"
           "struct sof_ipc_pipe_trigger {\n"
           "\tstruct sof_ipc_cmd_hdr hdr;\n"
           "\tuint32_t pipeline_id;     /* scheduler comp_id */\n"
           "\tuint32_t cmd;             /* COMP_TRIGGER_* */\n"
           "\tuint32_t rate;            /* sample rate */\n"
           "\tuint32_t channels;        /* channel count */\n"
           "\tuint32_t frame_fmt;       /* enum sof_ipc_frame */\n"
           "\tuint32_t direction;       /* SOF_IPC_STREAM_PLAYBACK/CAPTURE */\n"
           "}  __packed;\n",
)

# ---- K4: sound/soc/sof/sof-audio.h ----
# Insert `bool always_on;` after `bool dynamic_pipeline_widget;` line.
patch_once(
    KSRC / "sound/soc/sof/sof-audio.h",
    marker="bool always_on;",
    anchor="\tbool dynamic_pipeline_widget;\n",
    insert="\n\t/* V6.0: pipeline parsed as always-on (kernel triggers post topology load) */\n"
           "\tbool always_on;\n",
)

# ---- K2: sound/soc/sof/ipc3-topology.c — pipeline_tokens[] entry ----
# Add SOF_TKN_PIPE_ALWAYS_ON to the pipeline_tokens table (multiline anchor:
# we replace the original block with an extended one).
text = (KSRC / "sound/soc/sof/ipc3-topology.c").read_text()
old_block = ("static const struct sof_topology_token pipeline_tokens[] = {\n"
             "\t{SOF_TKN_SCHED_DYNAMIC_PIPELINE, SND_SOC_TPLG_TUPLE_TYPE_BOOL, get_token_u16,\n"
             "\t\toffsetof(struct snd_sof_widget, dynamic_pipeline_widget)},\n"
             "\n"
             "};\n")
new_block = ("static const struct sof_topology_token pipeline_tokens[] = {\n"
             "\t{SOF_TKN_SCHED_DYNAMIC_PIPELINE, SND_SOC_TPLG_TUPLE_TYPE_BOOL, get_token_u16,\n"
             "\t\toffsetof(struct snd_sof_widget, dynamic_pipeline_widget)},\n"
             "\t{SOF_TKN_PIPE_ALWAYS_ON, SND_SOC_TPLG_TUPLE_TYPE_BOOL, get_token_u16,\n"
             "\t\toffsetof(struct snd_sof_widget, always_on)},\n"
             "\n"
             "};\n")
if "SOF_TKN_PIPE_ALWAYS_ON" not in text:
    if old_block not in text:
        sys.exit("[fail] ipc3-topology.c: pipeline_tokens[] anchor not found for K2")
    text = text.replace(old_block, new_block, 1)
    (KSRC / "sound/soc/sof/ipc3-topology.c").write_text(text)
    print("[ok]   sound/soc/sof/ipc3-topology.c: K2 pipeline_tokens[] extended")
else:
    print("[skip] sound/soc/sof/ipc3-topology.c: K2 already applied")

# ---- K5 + K1: helper + trigger call at end of sof_ipc3_set_up_all_pipelines ----
text = (KSRC / "sound/soc/sof/ipc3-topology.c").read_text()
helper_marker = "sof_ipc3_send_pipe_trigger"
if helper_marker not in text:
    # Insert helper just before sof_ipc3_set_up_all_pipelines.
    helper_anchor = "static int sof_ipc3_set_up_all_pipelines(struct snd_sof_dev *sdev, bool verify)\n"
    if helper_anchor not in text:
        sys.exit("[fail] ipc3-topology.c: set_up_all_pipelines anchor not found for K5")
    helper_code = (
        "/* V6.0: kernel-side firmware trigger for always-on pipelines (K5).\n"
        " * Sends SOF_IPC_TPLG_PIPE_TRIGGER. The IPC carries the scheduler\n"
        " * widget's comp_id (NOT topology pipeline_id) because firmware-side\n"
        " * ipc_get_pipeline_by_id() actually looks up by comp_id despite its\n"
        " * macro name. cmd values match firmware src/include/sof/audio/component.h:\n"
        " *   COMP_TRIGGER_STOP=0, COMP_TRIGGER_START=1, COMP_TRIGGER_PRE_START=7\n"
        " *\n"
        " * On PRE_START the rate/channels/frame_fmt/direction params drive\n"
        " * pipeline_params() firmware-side before pipeline_prepare() (otherwise\n"
        " * dai_*_params() never runs and dai_common_prepare() returns -EINVAL).\n"
        " */\n"
        "static int sof_ipc3_send_pipe_trigger(struct snd_sof_dev *sdev,\n"
        "\t\t\t\t      u32 sched_comp_id, u32 cmd,\n"
        "\t\t\t\t      u32 rate, u32 channels, u32 frame_fmt,\n"
        "\t\t\t\t      u32 direction)\n"
        "{\n"
        "\tstruct sof_ipc_pipe_trigger msg;\n"
        "\tint ret;\n"
        "\n"
        "\tmemset(&msg, 0, sizeof(msg));\n"
        "\tmsg.hdr.size = sizeof(msg);\n"
        "\tmsg.hdr.cmd = SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_PIPE_TRIGGER;\n"
        "\tmsg.pipeline_id = sched_comp_id;\n"
        "\tmsg.cmd = cmd;\n"
        "\tmsg.rate = rate;\n"
        "\tmsg.channels = channels;\n"
        "\tmsg.frame_fmt = frame_fmt;\n"
        "\tmsg.direction = direction;\n"
        "\n"
        "\tdev_dbg(sdev->dev, \"V6.0: pipe_trigger sched_comp_id %u cmd %u rate %u ch %u fmt %u dir %u\\n\",\n"
        "\t\tsched_comp_id, cmd, rate, channels, frame_fmt, direction);\n"
        "\tret = sof_ipc_tx_message_no_reply(sdev->ipc, &msg, sizeof(msg));\n"
        "\tif (ret < 0)\n"
        "\t\tdev_err(sdev->dev, \"V6.0: pipe_trigger sched_comp_id %u cmd %u failed: %d\\n\",\n"
        "\t\t\tsched_comp_id, cmd, ret);\n"
        "\treturn ret;\n"
        "}\n"
        "\n"
    )
    text = text.replace(helper_anchor, helper_code + helper_anchor, 1)
    print("[ok]   sound/soc/sof/ipc3-topology.c: K5 helper inserted")
else:
    print("[skip] sound/soc/sof/ipc3-topology.c: K5 helper already present")

# ---- K1: trigger call at end of set_up_all_pipelines (non-verify path) ----
trigger_marker = "V6.0: trigger always-on pipelines"
if trigger_marker not in text:
    # The function ends at "return 0;\n}\n" right after the complete-pipeline switch.
    # Find the *first* "return 0;\n}\n" after sof_ipc3_set_up_all_pipelines header.
    func_start = text.find("static int sof_ipc3_set_up_all_pipelines(struct snd_sof_dev *sdev, bool verify)\n")
    if func_start < 0:
        sys.exit("[fail] ipc3-topology.c: K1 cannot find function start")
    # End of function = first "\treturn 0;\n}\n" after func_start.
    end_idx = text.find("\treturn 0;\n}\n", func_start)
    if end_idx < 0:
        sys.exit("[fail] ipc3-topology.c: K1 cannot find function end (return 0;})")
    trigger_block = (
        "\n"
        "\t/* V6.0: trigger always-on pipelines now that all widgets are\n"
        "\t * setup, DAI_CONFIG sent, and PIPE_COMPLETE acked. Failures here\n"
        "\t * are non-fatal for topology load — log and continue, since\n"
        "\t * existing PCM paths remain usable even if the always-on hook\n"
        "\t * fails for an experimental scheduler.\n"
        "\t *\n"
        "\t * Params for V6.0 minimal: SAI7 TDM 8x32 ASYNC, 48kHz, S32_LE,\n"
        "\t * direction=CAPTURE (anchor on SAI RX comp_dai). The walk goes\n"
        "\t * downstream through the loopback buffer to SAI TX. Hardcoded\n"
        "\t * here for now; future versions can derive from snd_sof_dai\n"
        "\t * config attached to the DAI widget anchored by the scheduler.\n"
        "\t */\n"
        "\tif (!verify) {\n"
        "\t\tstruct snd_sof_widget *aw;\n"
        "\t\tint trig_ret;\n"
        "\n"
        "\t\tlist_for_each_entry(aw, &sdev->widget_list, list) {\n"
        "\t\t\tif (aw->id != snd_soc_dapm_scheduler || !aw->always_on)\n"
        "\t\t\t\tcontinue;\n"
        "\t\t\ttrig_ret = sof_ipc3_send_pipe_trigger(sdev, aw->comp_id,\n"
        "\t\t\t\t\t\t\t      7 /* COMP_TRIGGER_PRE_START */,\n"
        "\t\t\t\t\t\t\t      48000, 8,\n"
        "\t\t\t\t\t\t\t      2 /* SOF_IPC_FRAME_S32_LE */,\n"
        "\t\t\t\t\t\t\t      1 /* SOF_IPC_STREAM_CAPTURE */);\n"
        "\t\t\tif (trig_ret < 0) {\n"
        "\t\t\t\tdev_warn(sdev->dev, \"V6.0: PRE_START failed comp_id %d (%d) — continuing\\n\",\n"
        "\t\t\t\t\t aw->comp_id, trig_ret);\n"
        "\t\t\t\tcontinue;\n"
        "\t\t\t}\n"
        "\t\t\ttrig_ret = sof_ipc3_send_pipe_trigger(sdev, aw->comp_id,\n"
        "\t\t\t\t\t\t\t      1 /* COMP_TRIGGER_START */,\n"
        "\t\t\t\t\t\t\t      0, 0, 0, 0);\n"
        "\t\t\tif (trig_ret < 0) {\n"
        "\t\t\t\tdev_warn(sdev->dev, \"V6.0: START failed comp_id %d (%d) — continuing\\n\",\n"
        "\t\t\t\t\t aw->comp_id, trig_ret);\n"
        "\t\t\t\tcontinue;\n"
        "\t\t\t}\n"
        "\t\t\tdev_info(sdev->dev, \"V6.0: pipeline %d (comp_id %d) started (always-on)\\n\",\n"
        "\t\t\t\t aw->pipeline_id, aw->comp_id);\n"
        "\t\t}\n"
        "\t}\n"
    )
    text = text[:end_idx] + trigger_block + text[end_idx:]
    print("[ok]   sound/soc/sof/ipc3-topology.c: K1 always-on trigger inserted")
else:
    print("[skip] sound/soc/sof/ipc3-topology.c: K1 already present")

(KSRC / "sound/soc/sof/ipc3-topology.c").write_text(text)
print("done.")
