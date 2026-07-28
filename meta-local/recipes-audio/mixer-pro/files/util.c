// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * util — logging + helpers ALSA (voir util.h).
 * Code déplacé tel quel depuis mixer-pro.c (V14.0 étape 0, extraction pure).
 */
#include <stdarg.h>
#include <stdio.h>

#include "util.h"

void mlog(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

int pcm_open(struct alsa_pcm *p, const char *name, int channels,
	     snd_pcm_stream_t dir)
{
	int err;
	snd_pcm_hw_params_t *hw;

	p->name = name;
	p->channels = channels;
	p->is_capture = (dir == SND_PCM_STREAM_CAPTURE);

	err = snd_pcm_open(&p->pcm, name, dir, 0);
	if (err < 0) {
		mlog("open(%s, %s): %s", name,
		     p->is_capture ? "capture" : "playback", snd_strerror(err));
		return err;
	}

	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(p->pcm, hw);
	snd_pcm_hw_params_set_access(p->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(p->pcm, hw, SND_PCM_FORMAT_S32_LE);
	snd_pcm_hw_params_set_channels(p->pcm, hw, channels);
	unsigned rate = SAMPLE_RATE;
	snd_pcm_hw_params_set_rate_near(p->pcm, hw, &rate, NULL);
	snd_pcm_uframes_t period = PERIOD_FRAMES, buffer = BUFFER_FRAMES;
	snd_pcm_hw_params_set_period_size_near(p->pcm, hw, &period, NULL);
	snd_pcm_hw_params_set_buffer_size_near(p->pcm, hw, &buffer);
	err = snd_pcm_hw_params(p->pcm, hw);
	if (err < 0) {
		mlog("hw_params(%s): %s", name, snd_strerror(err));
		return err;
	}

	/* sw_params : forcer start_threshold = 1 period pour que le PLAY démarre
	 * dès le 1er write (sinon auto-start au buffer plein → jamais en mode
	 * "write one period at a time"). Idem côté cap : avail_min = 1 period.
	 */
	snd_pcm_sw_params_t *sw;
	snd_pcm_sw_params_alloca(&sw);
	snd_pcm_sw_params_current(p->pcm, sw);
	snd_pcm_sw_params_set_start_threshold(p->pcm, sw,
		p->is_capture ? 1 : (snd_pcm_uframes_t)period);
	snd_pcm_sw_params_set_avail_min(p->pcm, sw, (snd_pcm_uframes_t)period);
	/* V8.26 — activer le HW timestamping pour mesurer drift précis
	 * via snd_pcm_status_get_audio_htstamp(). */
	snd_pcm_sw_params_set_tstamp_mode(p->pcm, sw, SND_PCM_TSTAMP_ENABLE);
	snd_pcm_sw_params_set_tstamp_type(p->pcm, sw, SND_PCM_TSTAMP_TYPE_MONOTONIC);
	err = snd_pcm_sw_params(p->pcm, sw);
	if (err < 0) {
		mlog("sw_params(%s): %s", name, snd_strerror(err));
		return err;
	}

	mlog("opened %s : %s %dch S32_LE @ %u Hz period=%lu buffer=%lu",
	     name, p->is_capture ? "cap" : "play", channels, rate,
	     period, buffer);
	return 0;
}

int pcm_recover(snd_pcm_t *pcm, int err)
{
	atomic_fetch_add(&g_st.xrun_count, 1);
	return snd_pcm_recover(pcm, err, 1);
}
