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

#include <linux/module.h>
#include <sound/soc.h>
#include "../sof-priv.h"
#include "../sof-client-probes.h"
#include "../sof-client.h"

/* Single extractor stream supported, tag 0 reserved for it */
#define IMX_PROBES_STREAM_TAG	0

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
	 * Compress runtime allocates the DMA buffer; firmware learns
	 * its address via the standard SOF probes IPC (PROBE_DMA_ADD).
	 */
	return 0;
}

static int imx_probes_compr_trigger(struct sof_client_dev *cdev,
				    struct snd_compr_stream *cstream,
				    int cmd, struct snd_soc_dai *dai)
{
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
