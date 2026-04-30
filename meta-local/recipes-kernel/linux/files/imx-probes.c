// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// Copyright(c) 2026 Electrosens
//
// SOF probes support for NXP i.MX SOF platforms.
//
// On i.MX, the SDMA transfers are programmed entirely by the DSP
// firmware via SOF IPC. The kernel host_ops are minimal: stream tag
// allocation in startup, and pointer reporting from the standard
// compress runtime. The actual probe protocol (probe_point_add /
// probe_point_remove) is handled by sof-client-probes-ipc3.c.
//
// Vendor extension: SDMA on i.MX has no hardware DMA gateway like Intel
// HDA or AMD ACP, so the firmware cannot resolve the host buffer phys
// address from the stream_tag alone. We send an additional vendor IPC
// (SOF_IPC_PROBE_HOST_BUFFER_SET) from set_params() carrying the physical
// address and size of the compress runtime buffer; the firmware then
// patches its SDMA buffer descriptor destination field.

#include <linux/module.h>
#include <sound/soc.h>
#include <sound/sof/header.h>
#include "../sof-priv.h"
#include "../sof-client-probes.h"
#include "../sof-client.h"

/* Single extractor stream supported, tag 0 reserved for it */
#define IMX_PROBES_STREAM_TAG	0

/*
 * Vendor IPC matching the firmware-side definition in
 * sof/src/include/ipc/header.h and sof/src/include/ipc3/probe.h.
 *
 * SOF firmware encodes IPC cmd as: GLB_TYPE << 28 | CMD_TYPE << 16 | sub.
 * GLB_PROBE = 0x9, our HOST_BUFFER_SET CMD_TYPE = 0x009.
 */
#define SOF_IPC_GLB_TYPE_SHIFT			28
#define SOF_IPC_GLB_TYPE(x)			((x) << SOF_IPC_GLB_TYPE_SHIFT)
#define SOF_IPC_GLB_PROBE_CMD			SOF_IPC_GLB_TYPE(0x9)

#define SOF_IPC_CMD_TYPE_SHIFT			16
#define SOF_IPC_CMD_TYPE(x)			((x) << SOF_IPC_CMD_TYPE_SHIFT)
#define SOF_IPC_PROBE_HOST_BUFFER_SET		SOF_IPC_CMD_TYPE(0x009)

/*
 * struct sof_ipc_cmd_hdr is defined in <sound/sof/header.h>; we only
 * declare the vendor payload struct here.
 */
struct sof_ipc_probe_host_buffer_msg {
	struct sof_ipc_cmd_hdr hdr;
	uint32_t phys_addr_lo;
	uint32_t phys_addr_hi;
	uint32_t size;
} __packed;

static int imx_probes_compr_startup(struct sof_client_dev *cdev,
				    struct snd_compr_stream *cstream,
				    struct snd_soc_dai *dai, u32 *stream_id)
{
	*stream_id = IMX_PROBES_STREAM_TAG;
	return 0;
}

static int imx_probes_compr_shutdown(struct sof_client_dev *cdev,
				     struct snd_compr_stream *cstream,
				     struct snd_soc_dai *dai)
{
	return 0;
}

static int imx_probes_compr_set_params(struct sof_client_dev *cdev,
				       struct snd_compr_stream *cstream,
				       struct snd_compr_params *params,
				       struct snd_soc_dai *dai)
{
	/*
	 * Our HOST_BUFFER_SET vendor IPC needs the firmware-side probe
	 * extraction to be already initialised (probe_init() must have run
	 * so that _probe->ext_dma.stream_tag != PROBE_DMA_INVALID).
	 *
	 * sof_probes_compr_set_params() calls us BEFORE ipc->init(), so
	 * we defer the IPC to imx_probes_compr_trigger(START) where
	 * probe_init() has already executed.
	 */
	return 0;
}

static int imx_probes_send_host_buffer(struct sof_client_dev *cdev,
				       struct snd_compr_stream *cstream)
{
	struct snd_compr_runtime *rt = cstream->runtime;
	struct sof_ipc_probe_host_buffer_msg msg;
	dma_addr_t phys = rt ? rt->dma_addr : 0;
	int ret;

	if (!phys || !rt->buffer_size) {
		dev_warn(&cdev->auxdev.dev,
			 "imx-probes: no DMA buffer (phys=0x%llx size=%llu)\n",
			 (unsigned long long)phys,
			 (unsigned long long)(rt ? rt->buffer_size : 0));
		return 0;
	}

	memset(&msg, 0, sizeof(msg));
	msg.hdr.size = sizeof(msg);
	msg.hdr.cmd  = SOF_IPC_GLB_PROBE_CMD | SOF_IPC_PROBE_HOST_BUFFER_SET;
	msg.phys_addr_lo = lower_32_bits(phys);
	msg.phys_addr_hi = upper_32_bits(phys);
	msg.size = rt->buffer_size;

	dev_info(&cdev->auxdev.dev,
		 "imx-probes: TX HOST_BUFFER_SET cmd=0x%08x phys=0x%llx size=%u\n",
		 msg.hdr.cmd, (unsigned long long)phys, msg.size);

	ret = sof_client_ipc_tx_message_no_reply(cdev, &msg);
	if (ret < 0)
		dev_err(&cdev->auxdev.dev,
			"imx-probes: HOST_BUFFER_SET IPC failed: %d\n", ret);
	else
		dev_info(&cdev->auxdev.dev,
			 "imx-probes: host buffer set OK\n");
	return ret;
}

static int imx_probes_compr_trigger(struct sof_client_dev *cdev,
				    struct snd_compr_stream *cstream,
				    int cmd, struct snd_soc_dai *dai)
{
	if (cmd == SNDRV_PCM_TRIGGER_START)
		return imx_probes_send_host_buffer(cdev, cstream);
	return 0;
}

static int imx_probes_compr_pointer(struct sof_client_dev *cdev,
				    struct snd_compr_stream *cstream,
				    struct snd_compr_tstamp *tstamp,
				    struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_stream *pstream = &dai->driver->capture;

	tstamp->copied_total = cstream->runtime->total_bytes_transferred;
	tstamp->sampling_rate = snd_pcm_rate_bit_to_rate(pstream->rates);
	return 0;
}

static const struct sof_probes_host_ops imx_probes_ops = {
	.startup = imx_probes_compr_startup,
	.shutdown = imx_probes_compr_shutdown,
	.set_params = imx_probes_compr_set_params,
	.trigger = imx_probes_compr_trigger,
	.pointer = imx_probes_compr_pointer,
};

int imx_probes_register(struct snd_sof_dev *sdev)
{
	return sof_client_dev_register(sdev, "imx-probes", 0,
				       &imx_probes_ops,
				       sizeof(imx_probes_ops));
}
EXPORT_SYMBOL(imx_probes_register);

void imx_probes_unregister(struct snd_sof_dev *sdev)
{
	sof_client_dev_unregister(sdev, "imx-probes", 0);
}
EXPORT_SYMBOL(imx_probes_unregister);

MODULE_DESCRIPTION("SOF Probes support for NXP i.MX");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_IMPORT_NS(SND_SOC_SOF_CLIENT);
