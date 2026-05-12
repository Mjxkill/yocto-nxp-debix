// SPDX-License-Identifier: GPL-2.0-only
/*
 * tac5212.c - TI TAC5212 Audio Codec driver
 *
 * Copyright (C) 2026
 *
 * TI TAC5212: stereo ADC (119dB) + quad DAC (120dB) audio codec.
 * Supports TDM, I2S, LJ formats. I2C control interface.
 * Multiple devices share a TDM bus; one device acts as bus controller.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <sound/soc.h>
#include <sound/tlv.h>
#include <sound/pcm_params.h>

#include "tac5212.h"

struct tac5212_priv {
	struct regmap *regmap;
	struct device *dev;
	unsigned int fmt;
	unsigned int tdm_slots;
	unsigned int slot_width;
	unsigned int base_slot;
	bool is_bus_closest;	/* true for TAC0 (closest to host on shared DOUT) */
	bool needs_reset;

	/* V7.0-E7.4 : cached 20-byte coef blobs for the 12 ADC + 12 DAC
	 * programmable biquads. Chip registers are effectively write-only ;
	 * cache mirrors last value written so ALSA reads return what was set.
	 */
	struct tac5212_bq_blob adc_bq[TAC5212_N_BIQUADS];
	struct tac5212_bq_blob dac_bq[TAC5212_N_BIQUADS];

	/* V7.0-E7.4.b : cached paged-coef blobs for the ADC HPF custom IIR,
	 * ADC Aux Mixer, AGC, AGC rate, DRC and ADSR engines.
	 */
	u8 hpf_iir_d1[TAC5212_HPF_IIR_SIZE];
	u8 aux_mix[TAC5212_AUX_MIX_SIZE];
	u8 agc_coefs[TAC5212_AGC_COEFS_SIZE];
	u8 agc_rate[TAC5212_AGC_RATE_SIZE];
	u8 drc_coefs[TAC5212_DRC_COEFS_SIZE];
	u8 adsr_coefs[TAC5212_ADSR_COEFS_SIZE];

	struct mutex paged_lock;	/* serialise page-switched I2C accesses */
};

static const struct reg_default tac5212_reg_defaults[] = {
	{ TAC5212_PAGE_CFG,		0x00 },
	{ TAC5212_SW_RESET,		0x00 },
	{ TAC5212_DEV_MISC_CFG,		0x00 },
	{ TAC5212_MISC_CFG,		0x00 },
	{ TAC5212_MISC_CFG1,		0x15 },
	{ TAC5212_DAC_CFG_A0,		0x35 },
	{ TAC5212_MISC_CFG0,		0x00 },
	{ TAC5212_GPIO1_CFG0,		0x32 },
	{ TAC5212_GPIO2_CFG0,		0x00 },
	{ TAC5212_GPO1_CFG0,		0x00 },
	{ TAC5212_GPI_CFG,		0x00 },
	{ TAC5212_GPO_GPI_VAL,		0x00 },
	{ TAC5212_INTF_CFG0,		0x00 },
	{ TAC5212_INTF_CFG1,		0x52 },
	{ TAC5212_INTF_CFG2,		0x80 },
	{ TAC5212_INTF_CFG3,		0x00 },
	{ TAC5212_INTF_CFG4,		0x00 },
	{ TAC5212_INTF_CFG5,		0x00 },
	{ TAC5212_INTF_CFG6,		0x00 },
	{ TAC5212_ASI_CFG0,		0x40 },
	{ TAC5212_ASI_CFG1,		0x00 },
	{ TAC5212_PASI_CFG0,		0x30 },
	{ TAC5212_PASI_TX_CFG0,		0x00 },
	{ TAC5212_PASI_TX_CFG1,		0x01 },
	{ TAC5212_PASI_TX_CFG2,		0x00 },
	{ TAC5212_PASI_TX_CH1_CFG,	0x20 },
	{ TAC5212_PASI_TX_CH2_CFG,	0x21 },
	{ TAC5212_PASI_TX_CH3_CFG,	0x02 },
	{ TAC5212_PASI_TX_CH4_CFG,	0x03 },
	{ TAC5212_PASI_TX_CH5_CFG,	0x04 },
	{ TAC5212_PASI_TX_CH6_CFG,	0x05 },
	{ TAC5212_PASI_TX_CH7_CFG,	0x06 },
	{ TAC5212_PASI_TX_CH8_CFG,	0x07 },
	{ TAC5212_PASI_RX_CFG0,		0x01 },
	{ TAC5212_PASI_RX_CFG1,		0x00 },
	{ TAC5212_PASI_RX_CH1_CFG,	0x20 },
	{ TAC5212_PASI_RX_CH2_CFG,	0x21 },
	{ TAC5212_PASI_RX_CH3_CFG,	0x02 },
	{ TAC5212_PASI_RX_CH4_CFG,	0x03 },
	{ TAC5212_PASI_RX_CH5_CFG,	0x04 },
	{ TAC5212_PASI_RX_CH6_CFG,	0x05 },
	{ TAC5212_PASI_RX_CH7_CFG,	0x06 },
	{ TAC5212_PASI_RX_CH8_CFG,	0x07 },
	{ TAC5212_CLK_CFG0,		0x00 },
	{ TAC5212_CLK_CFG1,		0x00 },
	{ TAC5212_CLK_CFG2,		0x40 },
	{ TAC5212_CNT_CLK_CFG0,		0x00 },
	{ TAC5212_CNT_CLK_CFG1,		0x00 },
	{ TAC5212_CNT_CLK_CFG2,		0x20 },
	{ TAC5212_CNT_CLK_CFG3,		0x00 },
	{ TAC5212_CNT_CLK_CFG4,		0x00 },
	{ TAC5212_CNT_CLK_CFG5,		0x00 },
	{ TAC5212_CNT_CLK_CFG6,		0x00 },
	{ TAC5212_INT_CFG,		0x00 },
	{ TAC5212_DAC_FLT_CFG,		0x54 },
	{ TAC5212_ADC_DAC_MISC_CFG,	0x00 },
	{ TAC5212_IADC_CFG,		0x5C },
	{ TAC5212_VREF_MICBIAS_CFG,	0x00 },
	{ TAC5212_PWR_TUNE_CFG0,	0x00 },
	{ TAC5212_PWR_TUNE_CFG1,	0x00 },
	{ TAC5212_ADC_CH1_CFG0,		0x00 },
	{ TAC5212_ADC_CH1_CFG2,		0xA1 },
	{ TAC5212_ADC_CH1_CFG3,		0x80 },
	{ TAC5212_ADC_CH1_CFG4,		0x00 },
	{ TAC5212_ADC_CH2_CFG0,		0x00 },
	{ TAC5212_ADC_CH2_CFG2,		0xA1 },
	{ TAC5212_ADC_CH2_CFG3,		0x80 },
	{ TAC5212_ADC_CH2_CFG4,		0x00 },
	{ TAC5212_ADC_CFG1,		0x00 },
	{ TAC5212_OUT1X_CFG0,		0x20 },
	{ TAC5212_OUT1X_CFG1,		0x20 },
	{ TAC5212_OUT1X_CFG2,		0x20 },
	{ TAC5212_DAC_CH1A_CFG0,	0xC9 },
	{ TAC5212_DAC_CH1A_CFG1,	0x80 },
	{ TAC5212_DAC_CH1B_CFG0,	0xC9 },
	{ TAC5212_DAC_CH1B_CFG1,	0x80 },
	{ TAC5212_OUT2X_CFG0,		0x20 },
	{ TAC5212_OUT2X_CFG1,		0x20 },
	{ TAC5212_OUT2X_CFG2,		0x20 },
	{ TAC5212_DAC_CH2A_CFG0,	0xC9 },
	{ TAC5212_DAC_CH2A_CFG1,	0x80 },
	{ TAC5212_DAC_CH2B_CFG0,	0xC9 },
	{ TAC5212_DAC_CH2B_CFG1,	0x80 },
	{ TAC5212_DSP_CFG0,		0x18 },
	{ TAC5212_DSP_CFG1,		0x18 },
	{ TAC5212_CH_EN,		0xCC },
	{ TAC5212_DYN_PUPD_CFG,		0x00 },
	{ TAC5212_PWR_CFG,		0x00 },
};

static bool tac5212_readable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case TAC5212_PAGE_CFG ... TAC5212_MISC_CFG0:
	case TAC5212_GPIO1_CFG0 ... TAC5212_GPO_GPI_VAL:
	case TAC5212_INTF_CFG0 ... TAC5212_INTF_CFG6:
	case TAC5212_ASI_CFG0 ... TAC5212_ASI_CFG1:
	case TAC5212_PASI_CFG0 ... TAC5212_PASI_RX_CH8_CFG:
	case TAC5212_CLK_CFG0 ... TAC5212_CLK_DET_STS3:
	case TAC5212_INT_CFG:
	case TAC5212_DAC_FLT_CFG:
	case TAC5212_ADC_DAC_MISC_CFG:
	case TAC5212_IADC_CFG ... TAC5212_PWR_TUNE_CFG1:
	case TAC5212_ADC_CH1_CFG0:
	case TAC5212_IADC_CH_CFG:
	case TAC5212_ADC_CH1_CFG2 ... TAC5212_ADC_CH1_CFG4:
	case TAC5212_ADC_CH2_CFG0:
	case TAC5212_ADC_CH2_CFG2 ... TAC5212_ADC_CH2_CFG4:
	case TAC5212_ADC_CFG1:
	case TAC5212_OUT1X_CFG0 ... TAC5212_DAC_CH1B_CFG1:
	case TAC5212_OUT2X_CFG0 ... TAC5212_DAC_CH2B_CFG1:
	case TAC5212_DSP_CFG0 ... TAC5212_DSP_CFG1:
	case TAC5212_CH_EN ... TAC5212_PWR_CFG:
	case TAC5212_DEV_STS0 ... TAC5212_DEV_STS1:
	case TAC5212_I2C_CKSUM:
		return true;
	default:
		return false;
	}
}

static bool tac5212_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case TAC5212_AVDD_IOVDD_STS:
	case TAC5212_CLK_ERR_STS0 ... TAC5212_CLK_DET_STS3:
	case TAC5212_DEV_STS0:
	case TAC5212_DEV_STS1:
	case TAC5212_I2C_CKSUM:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config tac5212_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = TAC5212_MAX_REG,
	.reg_defaults = tac5212_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(tac5212_reg_defaults),
	.readable_reg = tac5212_readable_reg,
	.volatile_reg = tac5212_volatile_reg,
	.cache_type = REGCACHE_NONE,
};

/* ADC volume: register value 0=mute, 1=-80dB ... 161=0dB ... 255=+47dB
 * Step = 0.5 dB, range = -80 to +47 dB, mute at 0 */
static const DECLARE_TLV_DB_MINMAX(tac5212_adc_vol_tlv, -8000, 4700);

/* DAC volume: similar range */
static const DECLARE_TLV_DB_MINMAX(tac5212_dac_vol_tlv, -10000, 2700);

/* Input source selection: Analog differential vs PDM microphone */
static const char * const tac5212_input_src_text[] = { "Analog", "PDM" };

static SOC_ENUM_SINGLE_DECL(tac5212_ch1_input_enum,
			    TAC5212_INTF_CFG4, 7, tac5212_input_src_text);
static SOC_ENUM_SINGLE_DECL(tac5212_ch2_input_enum,
			    TAC5212_INTF_CFG4, 6, tac5212_input_src_text);

static const struct snd_kcontrol_new tac5212_ch1_input_mux =
	SOC_DAPM_ENUM("CH1 Input Mux", tac5212_ch1_input_enum);
static const struct snd_kcontrol_new tac5212_ch2_input_mux =
	SOC_DAPM_ENUM("CH2 Input Mux", tac5212_ch2_input_enum);

/* ADC input configuration enums */
static const char * const tac5212_adc_insrc_text[] = {
	"Differential", "Single-Ended", "SE Mux INxP", "SE Mux INxM"
};
static SOC_ENUM_SINGLE_DECL(tac5212_adc_ch1_insrc_enum,
	TAC5212_ADC_CH1_CFG0, 6, tac5212_adc_insrc_text);
static SOC_ENUM_SINGLE_DECL(tac5212_adc_ch2_insrc_enum,
	TAC5212_ADC_CH2_CFG0, 6, tac5212_adc_insrc_text);

static const char * const tac5212_adc_imp_text[] = {
	"5kOhm", "10kOhm", "40kOhm"
};
static SOC_ENUM_SINGLE_DECL(tac5212_adc_ch1_imp_enum,
	TAC5212_ADC_CH1_CFG0, 4, tac5212_adc_imp_text);
static SOC_ENUM_SINGLE_DECL(tac5212_adc_ch2_imp_enum,
	TAC5212_ADC_CH2_CFG0, 4, tac5212_adc_imp_text);

/* ADC decimation filter enum */
static const char * const tac5212_deci_filt_text[] = {
	"Linear Phase", "Low Latency", "Ultra-Low Latency"
};
static SOC_ENUM_SINGLE_DECL(tac5212_adc_deci_filt_enum,
	TAC5212_DSP_CFG0, 6, tac5212_deci_filt_text);

/* ADC HPF cutoff enum */
static const char * const tac5212_hpf_text[] = {
	"Custom IIR", "1 Hz", "12 Hz", "96 Hz"
};
static SOC_ENUM_SINGLE_DECL(tac5212_adc_hpf_enum,
	TAC5212_DSP_CFG0, 4, tac5212_hpf_text);

/* ADC biquad config enum */
static const char * const tac5212_bq_cfg_text[] = {
	"Disabled", "1 Biquad/Ch", "2 Biquads/Ch", "3 Biquads/Ch"
};
static SOC_ENUM_SINGLE_DECL(tac5212_adc_bq_cfg_enum,
	TAC5212_DSP_CFG0, 2, tac5212_bq_cfg_text);

/* DAC interpolation filter enum */
static SOC_ENUM_SINGLE_DECL(tac5212_dac_intx_filt_enum,
	TAC5212_DSP_CFG1, 6, tac5212_deci_filt_text);

/* DAC HPF cutoff enum */
static SOC_ENUM_SINGLE_DECL(tac5212_dac_hpf_enum,
	TAC5212_DSP_CFG1, 4, tac5212_hpf_text);

/* DAC biquad config enum */
static SOC_ENUM_SINGLE_DECL(tac5212_dac_bq_cfg_enum,
	TAC5212_DSP_CFG1, 2, tac5212_bq_cfg_text);

/* Output drive configuration enum */
static const char * const tac5212_out_drive_text[] = {
	"Line 300 Ohm", "Headphone 16 Ohm", "4 Ohm", "High DR/SNR"
};
static SOC_ENUM_SINGLE_DECL(tac5212_out1p_drive_enum,
	TAC5212_OUT1X_CFG1, 6, tac5212_out_drive_text);
static SOC_ENUM_SINGLE_DECL(tac5212_out1m_drive_enum,
	TAC5212_OUT1X_CFG2, 6, tac5212_out_drive_text);
static SOC_ENUM_SINGLE_DECL(tac5212_out2p_drive_enum,
	TAC5212_OUT2X_CFG1, 6, tac5212_out_drive_text);
static SOC_ENUM_SINGLE_DECL(tac5212_out2m_drive_enum,
	TAC5212_OUT2X_CFG2, 6, tac5212_out_drive_text);

/* Output level control enum */
static const char * const tac5212_out_lvl_text[] = {
	"Reserved", "Reserved", "+12 dB", "+6 dB", "0 dB", "-6 dB", "-12 dB"
};
static SOC_ENUM_SINGLE_DECL(tac5212_out1p_lvl_enum,
	TAC5212_OUT1X_CFG1, 3, tac5212_out_lvl_text);
static SOC_ENUM_SINGLE_DECL(tac5212_out1m_lvl_enum,
	TAC5212_OUT1X_CFG2, 3, tac5212_out_lvl_text);
static SOC_ENUM_SINGLE_DECL(tac5212_out2p_lvl_enum,
	TAC5212_OUT2X_CFG1, 3, tac5212_out_lvl_text);
static SOC_ENUM_SINGLE_DECL(tac5212_out2m_lvl_enum,
	TAC5212_OUT2X_CFG2, 3, tac5212_out_lvl_text);

/* VREF full-scale enum */
static const char * const tac5212_vref_text[] = {
	"2.75V", "2.5V", "1.375V"
};
static SOC_ENUM_SINGLE_DECL(tac5212_vref_enum,
	TAC5212_VREF_MICBIAS_CFG, 0, tac5212_vref_text);

/* MICBIAS output enum */
static const char * const tac5212_micbias_text[] = {
	"VREF", "VREF/2", "Reserved", "Bypass AVDD"
};
static SOC_ENUM_SINGLE_DECL(tac5212_micbias_enum,
	TAC5212_VREF_MICBIAS_CFG, 2, tac5212_micbias_text);

/* Fine gain calibration TLV: 0=-0.8dB, 8=0dB, 15=+0.7dB; step=0.1dB */
static const DECLARE_TLV_DB_MINMAX(tac5212_fgain_tlv, -80, 70);

/* ============================================================
 * V7.0-E7.4 : Programmable biquad coefficient writes
 * Datasheet TAC5212 SLASF23A, §8.2.1 (P8), §8.2.2 (P9), §8.2.5
 * (P15), §8.2.6 (P16). Each biquad = 5×Q1.31 coefs (N0..D2)
 * stored as 4 BE bytes each ; stride 0x14 within its page.
 *
 * Coefficient computation (RBJ from Hz/Q/gain) is done in
 * userspace (no FPU in kernel) — the driver only stores 20
 * raw bytes per biquad and writes them via I2C with page
 * switching. Each biquad exposed as one SOC_BYTES_EXT control.
 */

/* Write `count` bytes starting at `reg` on the selected `page`.
 * Switches to page, writes, restores page 0. Serialised by
 * priv->paged_lock to keep page state coherent. */
static int tac5212_paged_write_buf(struct tac5212_priv *priv, u8 page,
				   u8 reg, const u8 *buf, size_t count)
{
	int ret;
	size_t i;

	mutex_lock(&priv->paged_lock);
	ret = regmap_write(priv->regmap, TAC5212_PAGE_SEL, page);
	if (ret)
		goto out;
	for (i = 0; i < count; i++) {
		ret = regmap_write(priv->regmap, reg + i, buf[i]);
		if (ret)
			goto out_restore;
	}
out_restore:
	regmap_write(priv->regmap, TAC5212_PAGE_SEL, 0);
out:
	mutex_unlock(&priv->paged_lock);
	return ret;
}

/* private_value encoding for the biquad-coef byte controls :
 *   bits 0-3  : biquad index 0..11
 *   bit  4    : 0 = ADC chain, 1 = DAC chain
 */
#define TAC5212_BQ_BLOB_PV(is_dac, idx)	((((u32)(is_dac)) << 4) | ((u32)(idx) & 0xf))
#define TAC5212_BQ_BLOB_PV_DAC(v)	(((v) >> 4) & 0x1)
#define TAC5212_BQ_BLOB_PV_IDX(v)	((v) & 0xf)

static int tac5212_bq_blob_info(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BYTES;
	uinfo->count = 20;
	return 0;
}

static int tac5212_bq_blob_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *cmp = snd_soc_kcontrol_component(kcontrol);
	struct tac5212_priv *priv = snd_soc_component_get_drvdata(cmp);
	u32 pv = (u32)kcontrol->private_value;
	bool is_dac = TAC5212_BQ_BLOB_PV_DAC(pv);
	int idx = TAC5212_BQ_BLOB_PV_IDX(pv);
	const u8 *src = is_dac ? priv->dac_bq[idx].bytes
			       : priv->adc_bq[idx].bytes;

	memcpy(ucontrol->value.bytes.data, src, 20);
	return 0;
}

static int tac5212_bq_blob_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *cmp = snd_soc_kcontrol_component(kcontrol);
	struct tac5212_priv *priv = snd_soc_component_get_drvdata(cmp);
	u32 pv = (u32)kcontrol->private_value;
	bool is_dac = TAC5212_BQ_BLOB_PV_DAC(pv);
	int idx = TAC5212_BQ_BLOB_PV_IDX(pv);
	u8 *dst = is_dac ? priv->dac_bq[idx].bytes
			 : priv->adc_bq[idx].bytes;
	u8 page;
	int slot;
	int ret;

	if (idx < 6) {
		page = is_dac ? TAC5212_PAGE_DAC_BQ_1_6
			      : TAC5212_PAGE_ADC_BQ_1_6;
		slot = idx;
	} else {
		page = is_dac ? TAC5212_PAGE_DAC_BQ_7_12
			      : TAC5212_PAGE_ADC_BQ_7_12;
		slot = idx - 6;
	}

	ret = tac5212_paged_write_buf(priv, page, TAC5212_BQ_OFFSET(slot),
				      ucontrol->value.bytes.data, 20);
	if (ret)
		return ret;
	memcpy(dst, ucontrol->value.bytes.data, 20);
	return 0;
}

#define TAC5212_BQ_BLOB(name_, pv_)					\
{									\
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = name_,		\
	.info = tac5212_bq_blob_info,					\
	.get = tac5212_bq_blob_get,					\
	.put = tac5212_bq_blob_put,					\
	.private_value = pv_,						\
}

/* ============================================================
 * V7.0-E7.4.b : generic paged-coef blob descriptors
 * ------------------------------------------------------------
 * Each blob entry describes one programmable coefficient region :
 * its kcontrol name (DT prefix "TACn" prepended by ALSA), page,
 * byte offset within the page, byte count, and offset of the
 * cache mirror within `struct tac5212_priv`. All access through
 * the same generic info/get/put callbacks, the kcontrol's
 * `private_value` indexes the descriptor table.
 *
 * Datasheet references :
 *   §8.2.4   — Page 11 : ADC HPF custom IIR + Aux Mixer
 *   §8.2.12  — Page 27 : AGC level/gain/hysteresis coefs
 *   §8.2.13  — Page 28 : AGC rate + DRC + ADSR coefs
 */
struct tac5212_paged_blob_desc {
	const char	*name;
	u8		page;
	u8		offset;
	u8		size;
	size_t		priv_off;
};

static const struct tac5212_paged_blob_desc tac5212_paged_blobs[] = {
	{ "ADC HPF IIR D1 Coefs", TAC5212_PAGE_ADC_IIR_AUX,
	  TAC5212_HPF_IIR_OFFSET, TAC5212_HPF_IIR_SIZE,
	  offsetof(struct tac5212_priv, hpf_iir_d1) },
	{ "ADC Aux Mixer Coefs",  TAC5212_PAGE_ADC_IIR_AUX,
	  TAC5212_AUX_MIX_OFFSET, TAC5212_AUX_MIX_SIZE,
	  offsetof(struct tac5212_priv, aux_mix) },
	{ "AGC Coefs",            TAC5212_PAGE_AGC_COEFS,
	  TAC5212_AGC_COEFS_OFFSET, TAC5212_AGC_COEFS_SIZE,
	  offsetof(struct tac5212_priv, agc_coefs) },
	{ "AGC Rate Coefs",       TAC5212_PAGE_AGC_RATE_DRC_ADSR,
	  TAC5212_AGC_RATE_OFFSET, TAC5212_AGC_RATE_SIZE,
	  offsetof(struct tac5212_priv, agc_rate) },
	{ "DRC Coefs",            TAC5212_PAGE_AGC_RATE_DRC_ADSR,
	  TAC5212_DRC_COEFS_OFFSET, TAC5212_DRC_COEFS_SIZE,
	  offsetof(struct tac5212_priv, drc_coefs) },
	{ "ADSR Coefs",           TAC5212_PAGE_AGC_RATE_DRC_ADSR,
	  TAC5212_ADSR_COEFS_OFFSET, TAC5212_ADSR_COEFS_SIZE,
	  offsetof(struct tac5212_priv, adsr_coefs) },
};
#define TAC5212_N_PAGED_BLOBS ARRAY_SIZE(tac5212_paged_blobs)

static int tac5212_paged_blob_info(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_info *uinfo)
{
	int idx = (int)kcontrol->private_value;

	if (idx < 0 || idx >= (int)TAC5212_N_PAGED_BLOBS)
		return -EINVAL;
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BYTES;
	uinfo->count = tac5212_paged_blobs[idx].size;
	return 0;
}

static int tac5212_paged_blob_get(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *cmp = snd_soc_kcontrol_component(kcontrol);
	struct tac5212_priv *priv = snd_soc_component_get_drvdata(cmp);
	int idx = (int)kcontrol->private_value;
	const struct tac5212_paged_blob_desc *d;
	const u8 *src;

	if (idx < 0 || idx >= (int)TAC5212_N_PAGED_BLOBS)
		return -EINVAL;
	d = &tac5212_paged_blobs[idx];
	src = (const u8 *)priv + d->priv_off;
	memcpy(ucontrol->value.bytes.data, src, d->size);
	return 0;
}

static int tac5212_paged_blob_put(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *cmp = snd_soc_kcontrol_component(kcontrol);
	struct tac5212_priv *priv = snd_soc_component_get_drvdata(cmp);
	int idx = (int)kcontrol->private_value;
	const struct tac5212_paged_blob_desc *d;
	u8 *dst;
	int ret;

	if (idx < 0 || idx >= (int)TAC5212_N_PAGED_BLOBS)
		return -EINVAL;
	d = &tac5212_paged_blobs[idx];
	dst = (u8 *)priv + d->priv_off;
	ret = tac5212_paged_write_buf(priv, d->page, d->offset,
				      ucontrol->value.bytes.data, d->size);
	if (ret)
		return ret;
	memcpy(dst, ucontrol->value.bytes.data, d->size);
	return 0;
}

#define TAC5212_PAGED_BLOB(name_, idx_)					\
{									\
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = name_,		\
	.info = tac5212_paged_blob_info,				\
	.get = tac5212_paged_blob_get,					\
	.put = tac5212_paged_blob_put,					\
	.private_value = idx_,						\
}

/* NOTE : Limiter / AGC enable bits / DRC enable bits / PLIM live on
 * Book 0 Page 1 (addresses 0x23, 0x24, 0x2B respectively) which
 * overlap with the PASI TX/RX channel config registers on Page 0.
 * Exposing them requires a paged single-byte helper not yet
 * implemented — deferred to E7.4.c.
 */

static const struct snd_kcontrol_new tac5212_controls[] = {
	/* === ADC Digital Volume (-80dB to +47dB, 0.5dB step) === */
	SOC_SINGLE_TLV("ADC1 Digital Volume", TAC5212_ADC_CH1_CFG2,
		       0, 255, 0, tac5212_adc_vol_tlv),
	SOC_SINGLE_TLV("ADC2 Digital Volume", TAC5212_ADC_CH2_CFG2,
		       0, 255, 0, tac5212_adc_vol_tlv),

	/* === ADC Fine Gain Calibration (-0.8 to +0.7dB) === */
	SOC_SINGLE_TLV("ADC1 Fine Gain", TAC5212_ADC_CH1_CFG3,
		       4, 15, 0, tac5212_fgain_tlv),
	SOC_SINGLE_TLV("ADC2 Fine Gain", TAC5212_ADC_CH2_CFG3,
		       4, 15, 0, tac5212_fgain_tlv),

	/* === ADC Phase Calibration (0-63 mod clock cycles) === */
	SOC_SINGLE("ADC1 Phase Calibration", TAC5212_ADC_CH1_CFG4, 2, 63, 0),
	SOC_SINGLE("ADC2 Phase Calibration", TAC5212_ADC_CH2_CFG4, 2, 63, 0),

	/* === ADC Input Configuration === */
	SOC_ENUM("ADC1 Input Config", tac5212_adc_ch1_insrc_enum),
	SOC_ENUM("ADC2 Input Config", tac5212_adc_ch2_insrc_enum),
	SOC_ENUM("ADC1 Input Impedance", tac5212_adc_ch1_imp_enum),
	SOC_ENUM("ADC2 Input Impedance", tac5212_adc_ch2_imp_enum),
	SOC_SINGLE("ADC1 Wide Bandwidth", TAC5212_ADC_CH1_CFG0, 0, 1, 0),
	SOC_SINGLE("ADC2 Wide Bandwidth", TAC5212_ADC_CH2_CFG0, 0, 1, 0),

	/* === ADC DSP === */
	SOC_ENUM("ADC Decimation Filter", tac5212_adc_deci_filt_enum),
	SOC_ENUM("ADC HPF Cutoff", tac5212_adc_hpf_enum),
	SOC_ENUM("ADC Biquad Config", tac5212_adc_bq_cfg_enum),
	SOC_SINGLE("ADC Soft-Step Disable", TAC5212_DSP_CFG0, 1, 1, 0),
	SOC_SINGLE("ADC DVOL Gang", TAC5212_DSP_CFG0, 0, 1, 0),
	SOC_SINGLE("ADC Channel Swap", TAC5212_DYN_PUPD_CFG, 1, 1, 0),
	SOC_SINGLE("ADC Data Invert", TAC5212_ADC_CFG1, 2, 1, 0),

	/* === DAC Digital Volume (-100dB to +27dB, 0.5dB step) === */
	SOC_SINGLE_TLV("DAC1A Digital Volume", TAC5212_DAC_CH1A_CFG0,
		       0, 255, 0, tac5212_dac_vol_tlv),
	SOC_SINGLE_TLV("DAC1B Digital Volume", TAC5212_DAC_CH1B_CFG0,
		       0, 255, 0, tac5212_dac_vol_tlv),
	SOC_SINGLE_TLV("DAC2A Digital Volume", TAC5212_DAC_CH2A_CFG0,
		       0, 255, 0, tac5212_dac_vol_tlv),
	SOC_SINGLE_TLV("DAC2B Digital Volume", TAC5212_DAC_CH2B_CFG0,
		       0, 255, 0, tac5212_dac_vol_tlv),

	/* === DAC Fine Gain Calibration === */
	SOC_SINGLE_TLV("DAC1A Fine Gain", TAC5212_DAC_CH1A_CFG1,
		       4, 15, 0, tac5212_fgain_tlv),
	SOC_SINGLE_TLV("DAC1B Fine Gain", TAC5212_DAC_CH1B_CFG1,
		       4, 15, 0, tac5212_fgain_tlv),
	SOC_SINGLE_TLV("DAC2A Fine Gain", TAC5212_DAC_CH2A_CFG1,
		       4, 15, 0, tac5212_fgain_tlv),
	SOC_SINGLE_TLV("DAC2B Fine Gain", TAC5212_DAC_CH2B_CFG1,
		       4, 15, 0, tac5212_fgain_tlv),

	/* === DAC DSP === */
	SOC_ENUM("DAC Interpolation Filter", tac5212_dac_intx_filt_enum),
	SOC_ENUM("DAC HPF Cutoff", tac5212_dac_hpf_enum),
	SOC_ENUM("DAC Biquad Config", tac5212_dac_bq_cfg_enum),
	SOC_SINGLE("DAC Soft-Step Disable", TAC5212_DSP_CFG1, 1, 1, 0),
	SOC_SINGLE("DAC DVOL Gang", TAC5212_DSP_CFG1, 0, 1, 0),
	SOC_SINGLE("DAC Channel Swap", TAC5212_DYN_PUPD_CFG, 0, 1, 0),
	SOC_SINGLE("DAC1 Wide Bandwidth", TAC5212_OUT1X_CFG1, 0, 1, 0),
	SOC_SINGLE("DAC2 Wide Bandwidth", TAC5212_OUT2X_CFG1, 0, 1, 0),

	/* === Output Stage === */
	SOC_ENUM("OUT1P Drive", tac5212_out1p_drive_enum),
	SOC_ENUM("OUT1M Drive", tac5212_out1m_drive_enum),
	SOC_ENUM("OUT2P Drive", tac5212_out2p_drive_enum),
	SOC_ENUM("OUT2M Drive", tac5212_out2m_drive_enum),
	SOC_ENUM("OUT1P Level", tac5212_out1p_lvl_enum),
	SOC_ENUM("OUT1M Level", tac5212_out1m_lvl_enum),
	SOC_ENUM("OUT2P Level", tac5212_out2p_lvl_enum),
	SOC_ENUM("OUT2M Level", tac5212_out2m_lvl_enum),

	/* === VREF / MICBIAS === */
	SOC_ENUM("VREF Full-Scale", tac5212_vref_enum),
	SOC_ENUM("MICBIAS Value", tac5212_micbias_enum),
	SOC_SINGLE("MICBIAS LDO Gain", TAC5212_VREF_MICBIAS_CFG, 4, 1, 0),
	SOC_SINGLE("MICBIAS Power", TAC5212_PWR_CFG, 5, 1, 0),

	/* === Activity Detection === */
	SOC_SINGLE("VAD Enable", TAC5212_PWR_CFG, 2, 1, 0),

	/* === V7.0-E7.4 : ADC programmable biquads (12) ===
	 * 20-byte blobs (5 × Q1.31 BE coefs : N0, N1, N2, D1, D2).
	 * RBJ coefficient math runs in userspace ; write raw bytes via
	 * `amixer cset numid=X --` then 20-byte hex.
	 */
	TAC5212_BQ_BLOB("ADC BQ1 Coefs",  TAC5212_BQ_BLOB_PV(0,  0)),
	TAC5212_BQ_BLOB("ADC BQ2 Coefs",  TAC5212_BQ_BLOB_PV(0,  1)),
	TAC5212_BQ_BLOB("ADC BQ3 Coefs",  TAC5212_BQ_BLOB_PV(0,  2)),
	TAC5212_BQ_BLOB("ADC BQ4 Coefs",  TAC5212_BQ_BLOB_PV(0,  3)),
	TAC5212_BQ_BLOB("ADC BQ5 Coefs",  TAC5212_BQ_BLOB_PV(0,  4)),
	TAC5212_BQ_BLOB("ADC BQ6 Coefs",  TAC5212_BQ_BLOB_PV(0,  5)),
	TAC5212_BQ_BLOB("ADC BQ7 Coefs",  TAC5212_BQ_BLOB_PV(0,  6)),
	TAC5212_BQ_BLOB("ADC BQ8 Coefs",  TAC5212_BQ_BLOB_PV(0,  7)),
	TAC5212_BQ_BLOB("ADC BQ9 Coefs",  TAC5212_BQ_BLOB_PV(0,  8)),
	TAC5212_BQ_BLOB("ADC BQ10 Coefs", TAC5212_BQ_BLOB_PV(0,  9)),
	TAC5212_BQ_BLOB("ADC BQ11 Coefs", TAC5212_BQ_BLOB_PV(0, 10)),
	TAC5212_BQ_BLOB("ADC BQ12 Coefs", TAC5212_BQ_BLOB_PV(0, 11)),

	/* === V7.0-E7.4 : DAC programmable biquads (12) === */
	TAC5212_BQ_BLOB("DAC BQ1 Coefs",  TAC5212_BQ_BLOB_PV(1,  0)),
	TAC5212_BQ_BLOB("DAC BQ2 Coefs",  TAC5212_BQ_BLOB_PV(1,  1)),
	TAC5212_BQ_BLOB("DAC BQ3 Coefs",  TAC5212_BQ_BLOB_PV(1,  2)),
	TAC5212_BQ_BLOB("DAC BQ4 Coefs",  TAC5212_BQ_BLOB_PV(1,  3)),
	TAC5212_BQ_BLOB("DAC BQ5 Coefs",  TAC5212_BQ_BLOB_PV(1,  4)),
	TAC5212_BQ_BLOB("DAC BQ6 Coefs",  TAC5212_BQ_BLOB_PV(1,  5)),
	TAC5212_BQ_BLOB("DAC BQ7 Coefs",  TAC5212_BQ_BLOB_PV(1,  6)),
	TAC5212_BQ_BLOB("DAC BQ8 Coefs",  TAC5212_BQ_BLOB_PV(1,  7)),
	TAC5212_BQ_BLOB("DAC BQ9 Coefs",  TAC5212_BQ_BLOB_PV(1,  8)),
	TAC5212_BQ_BLOB("DAC BQ10 Coefs", TAC5212_BQ_BLOB_PV(1,  9)),
	TAC5212_BQ_BLOB("DAC BQ11 Coefs", TAC5212_BQ_BLOB_PV(1, 10)),
	TAC5212_BQ_BLOB("DAC BQ12 Coefs", TAC5212_BQ_BLOB_PV(1, 11)),

	/* === V7.0-E7.4.b : programmable paged coefficient blobs === *
	 * (HPF custom IIR, ADC Aux Mixer, AGC, AGC rates, DRC, ADSR)
	 * RBJ / time-constant math runs in userspace ; the driver
	 * stores 4..36 raw bytes per blob and writes them via paged
	 * I2C (see `tac5212_paged_blobs[]` descriptor table).
	 */
	TAC5212_PAGED_BLOB("ADC HPF IIR D1 Coefs", 0),
	TAC5212_PAGED_BLOB("ADC Aux Mixer Coefs",  1),
	TAC5212_PAGED_BLOB("AGC Coefs",            2),
	TAC5212_PAGED_BLOB("AGC Rate Coefs",       3),
	TAC5212_PAGED_BLOB("DRC Coefs",            4),
	TAC5212_PAGED_BLOB("ADSR Coefs",           5),
};

static const struct snd_soc_dapm_widget tac5212_dapm_widgets[] = {
	/* Analog inputs */
	SND_SOC_DAPM_INPUT("IN1P"),
	SND_SOC_DAPM_INPUT("IN1M"),
	SND_SOC_DAPM_INPUT("IN2P"),
	SND_SOC_DAPM_INPUT("IN2M"),

	/* PDM microphone inputs */
	SND_SOC_DAPM_INPUT("PDMDIN1"),
	SND_SOC_DAPM_INPUT("PDMDIN2"),

	/* Input source muxes: Analog vs PDM */
	SND_SOC_DAPM_MUX("CH1 Input Mux", SND_SOC_NOPM, 0, 0,
			  &tac5212_ch1_input_mux),
	SND_SOC_DAPM_MUX("CH2 Input Mux", SND_SOC_NOPM, 0, 0,
			  &tac5212_ch2_input_mux),

	/* ADC / DAC */
	/* ADC/DAC power managed in probe, not by DAPM (controller must
	 * keep clocks running even when no stream is active) */
	SND_SOC_DAPM_ADC("ADC1", "Capture", SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_ADC("ADC2", "Capture", SND_SOC_NOPM, 0, 0),

	SND_SOC_DAPM_DAC("DAC1", "Playback", SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_DAC("DAC2", "Playback", SND_SOC_NOPM, 0, 0),

	/* Analog outputs */
	SND_SOC_DAPM_OUTPUT("OUT1P"),
	SND_SOC_DAPM_OUTPUT("OUT1M"),
	SND_SOC_DAPM_OUTPUT("OUT2P"),
	SND_SOC_DAPM_OUTPUT("OUT2M"),
};

static const struct snd_soc_dapm_route tac5212_dapm_routes[] = {
	/* Analog inputs → mux */
	{ "CH1 Input Mux", "Analog", "IN1P" },
	{ "CH1 Input Mux", "Analog", "IN1M" },
	{ "CH2 Input Mux", "Analog", "IN2P" },
	{ "CH2 Input Mux", "Analog", "IN2M" },

	/* PDM inputs → mux */
	{ "CH1 Input Mux", "PDM", "PDMDIN1" },
	{ "CH2 Input Mux", "PDM", "PDMDIN2" },

	/* Mux → ADC */
	{ "ADC1", NULL, "CH1 Input Mux" },
	{ "ADC2", NULL, "CH2 Input Mux" },

	/* DAC → outputs */
	{ "OUT1P", NULL, "DAC1" },
	{ "OUT1M", NULL, "DAC1" },
	{ "OUT2P", NULL, "DAC2" },
	{ "OUT2M", NULL, "DAC2" },
};

static int tac5212_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_component *component = dai->component;
	struct tac5212_priv *priv = snd_soc_component_get_drvdata(component);

	priv->fmt = fmt;
	return 0;
}

static int tac5212_set_tdm_slot(struct snd_soc_dai *dai,
				unsigned int tx_mask, unsigned int rx_mask,
				int slots, int slot_width)
{
	struct snd_soc_component *component = dai->component;
	struct tac5212_priv *priv = snd_soc_component_get_drvdata(component);

	priv->tdm_slots = slots;
	priv->slot_width = slot_width;

	return 0;
}

/* SAI7 registers managed by SOF DSP firmware — no kernel patches */

static int tac5212_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct tac5212_priv *priv = snd_soc_component_get_drvdata(component);
	unsigned int base = priv->base_slot;

	unsigned int rate = params_rate(params);
	unsigned int wlen, fs_mode, dummy;
	int ret;

	/*
	 * On first stream open, do a SW reset with BCLK present.
	 * The PDM decoder needs BCLK active during init to avoid
	 * white noise. At probe time BCLK is not yet running.
	 */
	if (priv->needs_reset) {
		unsigned int saved_cfg4;

		priv->needs_reset = false;
		/* Save user's PDM_CH_SEL choice before reset */
		regmap_read(priv->regmap, TAC5212_INTF_CFG4, &saved_cfg4);
		saved_cfg4 &= (TAC5212_PDM_CH1_SEL | TAC5212_PDM_CH2_SEL);

		regmap_write(priv->regmap, TAC5212_SW_RESET,
			     TAC5212_SW_RESET_BIT);
		msleep(2);
		regmap_write(priv->regmap, TAC5212_DEV_MISC_CFG,
			     TAC5212_SLEEP_ENZ | TAC5212_SLEEP_EXIT_VREF_EN);
		msleep(10);
		regmap_write(priv->regmap, TAC5212_INTF_CFG1, 0x53);
		regmap_write(priv->regmap, TAC5212_INTF_CFG2,
			     TAC5212_PASI_DIN_EN);
		regmap_write(priv->regmap, TAC5212_ASI_CFG0,
			     TAC5212_SASI_DIS);
		regmap_update_bits(priv->regmap, TAC5212_MISC_CFG,
				   BIT(6), BIT(6));
		regmap_write(priv->regmap, TAC5212_PASI_TX_CFG0,
			     priv->is_bus_closest ? 0x48 : 0x40);
		regmap_write(priv->regmap, TAC5212_PASI_TX_CFG1, 0x01);
		regmap_write(priv->regmap, TAC5212_PASI_RX_CFG0, 0x01);
		regmap_write(priv->regmap, TAC5212_GPO1_CFG0, 0x41);
		regmap_write(priv->regmap, TAC5212_GPI_CFG, 0x02);
		regmap_write(priv->regmap, TAC5212_INTF_CFG4,
			     saved_cfg4 |
			     (0x03 << TAC5212_PDM_DIN1_SEL_SHIFT));
		regmap_write(priv->regmap, TAC5212_PASI_CFG0, 0x30);
		regmap_write(priv->regmap, TAC5212_CLK_CFG2,
			     TAC5212_AUTO_PLL_FR_ALLOW);
		regmap_write(priv->regmap, TAC5212_CH_EN,
			     TAC5212_IN_CH1_EN | TAC5212_IN_CH2_EN |
			     TAC5212_OUT_CH1_EN | TAC5212_OUT_CH2_EN);
		regmap_write(priv->regmap, TAC5212_PWR_CFG,
			     TAC5212_ADC_PDZ | TAC5212_DAC_PDZ |
			     TAC5212_MICBIAS_PDZ);
	}

	/* Clear latched clock errors */
	{
		unsigned int dummy;
		regmap_read(priv->regmap, TAC5212_CLK_ERR_STS0, &dummy);
		regmap_read(priv->regmap, TAC5212_CLK_ERR_STS1, &dummy);
	}

	/*
	 * FS_MODE=0: 48 kHz family (48/96/192 kHz)
	 * FS_MODE=1: 44.1 kHz family (44.1 kHz)
	 * SAI7 master generates BCLK; TAC PLL auto-locks on it.
	 */
	switch (rate) {
	case 44100:
		fs_mode = TAC5212_FS_MODE;
		break;
	case 48000:
	case 96000:
		fs_mode = 0;
		break;
	default:
		dev_err(priv->dev, "unsupported sample rate: %u\n", rate);
		return -EINVAL;
	}

	/* Determine word length */
	switch (priv->slot_width) {
	case 16:
		wlen = TAC5212_WLEN_16;
		break;
	case 20:
		wlen = TAC5212_WLEN_20;
		break;
	case 24:
		wlen = TAC5212_WLEN_24;
		break;
	case 32:
		wlen = TAC5212_WLEN_32;
		break;
	default:
		wlen = TAC5212_WLEN_32;
		break;
	}

	/* PASI_CFG0: TDM mode, word length */
	ret = regmap_update_bits(priv->regmap, TAC5212_PASI_CFG0,
				TAC5212_PASI_FORMAT_MASK | TAC5212_PASI_WLEN_MASK,
				(TAC5212_FMT_TDM << TAC5212_PASI_FORMAT_SHIFT) |
				(wlen << TAC5212_PASI_WLEN_SHIFT));
	if (ret)
		return ret;

	/* TX_EDGE (bit 7): add half-cycle delay for BCLK > 18.5 MHz (per AN sbaa383c) */
	ret = regmap_update_bits(priv->regmap, TAC5212_PASI_TX_CFG0,
				TAC5212_PASI_TX_EDGE,
				rate >= 96000 ? TAC5212_PASI_TX_EDGE : 0);
	if (ret)
		return ret;

	/* All TACs are targets: receive BCLK+FSYNC from SAI7 */
	ret = regmap_update_bits(priv->regmap, TAC5212_CNT_CLK_CFG2,
				TAC5212_PASI_CNT_CFG |
				TAC5212_FS_MODE,
				fs_mode);
	if (ret)
		return ret;

	/* TX slot assignments (ADC data out): CH1 and CH2 enabled */
	ret = regmap_write(priv->regmap, TAC5212_PASI_TX_CH1_CFG,
			   TAC5212_CH_EN_BIT | (base & TAC5212_CH_SLOT_MASK));
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, TAC5212_PASI_TX_CH2_CFG,
			   TAC5212_CH_EN_BIT | ((base + 1) & TAC5212_CH_SLOT_MASK));
	if (ret)
		return ret;

	/* Disable TX channels 3-8 (tri-state) */
	for (int i = TAC5212_PASI_TX_CH3_CFG; i <= TAC5212_PASI_TX_CH8_CFG; i++) {
		ret = regmap_write(priv->regmap, i, 0x00);
		if (ret)
			return ret;
	}

	/* RX slot assignments (DAC data in): CH1 and CH2 enabled */
	ret = regmap_write(priv->regmap, TAC5212_PASI_RX_CH1_CFG,
			   TAC5212_CH_EN_BIT | (base & TAC5212_CH_SLOT_MASK));
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, TAC5212_PASI_RX_CH2_CFG,
			   TAC5212_CH_EN_BIT | ((base + 1) & TAC5212_CH_SLOT_MASK));
	if (ret)
		return ret;

	/* Disable RX channels 3-8 */
	for (int i = TAC5212_PASI_RX_CH3_CFG; i <= TAC5212_PASI_RX_CH8_CFG; i++) {
		ret = regmap_write(priv->regmap, i, 0x00);
		if (ret)
			return ret;
	}

	/* CLK_CFG2: PLL enabled, auto fractional, source=BCLK */
	ret = regmap_write(priv->regmap, TAC5212_CLK_CFG2,
			   TAC5212_AUTO_PLL_FR_ALLOW);
	if (ret)
		return ret;

	/* Enable channels: IN_CH1, IN_CH2, OUT_CH1, OUT_CH2 */
	ret = regmap_write(priv->regmap, TAC5212_CH_EN,
			   TAC5212_IN_CH1_EN | TAC5212_IN_CH2_EN |
			   TAC5212_OUT_CH1_EN | TAC5212_OUT_CH2_EN);
	if (ret)
		return ret;

	/* Wait for PLL to lock on new BCLK frequency, then clear errors */
	msleep(10);
	{
		unsigned int dummy;
		regmap_read(priv->regmap, TAC5212_CLK_ERR_STS0, &dummy);
		regmap_read(priv->regmap, TAC5212_CLK_ERR_STS1, &dummy);
		regmap_read(priv->regmap, TAC5212_DEV_STS0, &dummy);
		regmap_read(priv->regmap, TAC5212_DEV_STS1, &dummy);
	}

	return 0;
}

static int tac5212_mute_stream(struct snd_soc_dai *dai, int mute, int stream)
{
	struct snd_soc_component *component = dai->component;
	struct tac5212_priv *priv = snd_soc_component_get_drvdata(component);

	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		/* Mute/unmute via DAC digital volume (0=mute, 0xC9=0dB) */
		unsigned int val = mute ? 0x00 : 0xC9;

		regmap_write(priv->regmap, TAC5212_DAC_CH1A_CFG0, val);
		regmap_write(priv->regmap, TAC5212_DAC_CH2A_CFG0, val);
	}


	return 0;
}

static int tac5212_trigger(struct snd_pcm_substream *substream, int cmd,
			   struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct tac5212_priv *priv = snd_soc_component_get_drvdata(component);
	unsigned int dummy;

	if (cmd == SNDRV_PCM_TRIGGER_START ||
	    cmd == SNDRV_PCM_TRIGGER_RESUME ||
	    cmd == SNDRV_PCM_TRIGGER_PAUSE_RELEASE) {
		/* Clear any clock errors latched during PLL lock transient. */
		msleep(50);
		regmap_read(priv->regmap, TAC5212_CLK_ERR_STS0, &dummy);
		regmap_read(priv->regmap, TAC5212_CLK_ERR_STS1, &dummy);
	}

	return 0;
}

static const struct snd_soc_dai_ops tac5212_dai_ops = {
	.set_fmt	= tac5212_set_fmt,
	.set_tdm_slot	= tac5212_set_tdm_slot,
	.hw_params	= tac5212_hw_params,
	/* mute_stream removed: DAC volumes are managed by tac-reset and
	 * user-space mixer controls. The automatic mute/unmute on stream
	 * start/stop was breaking duplex loopback — every playback
	 * prepare/recovery cycle would zero the DAC volume registers,
	 * silencing output until manually restored via i2c.
	 */
	.trigger	= tac5212_trigger,
};

static struct snd_soc_dai_driver tac5212_dai = {
	.name = "tac5212-hifi",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_44100 |
			SNDRV_PCM_RATE_48000 |
			SNDRV_PCM_RATE_96000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE |
			   SNDRV_PCM_FMTBIT_S20_3LE |
			   SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
	},
	.capture = {
		.stream_name = "Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_44100 |
			SNDRV_PCM_RATE_48000 |
			SNDRV_PCM_RATE_96000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE |
			   SNDRV_PCM_FMTBIT_S20_3LE |
			   SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &tac5212_dai_ops,
	.symmetric_rate = 1,
};

static int tac5212_component_probe(struct snd_soc_component *component)
{
	struct tac5212_priv *priv = snd_soc_component_get_drvdata(component);
	int ret;

	/* Software reset */
	ret = regmap_write(priv->regmap, TAC5212_SW_RESET,
			   TAC5212_SW_RESET_BIT);
	if (ret)
		return ret;
	msleep(2);

	/* Exit sleep mode */
	ret = regmap_write(priv->regmap, TAC5212_DEV_MISC_CFG,
			   TAC5212_SLEEP_ENZ | TAC5212_SLEEP_EXIT_VREF_EN);
	if (ret)
		return ret;
	msleep(10);

	/* INTF_CFG1: DOUT = Primary ASI DOUT (0x5), Hi-Z inactive slots (0x3) */
	ret = regmap_write(priv->regmap, TAC5212_INTF_CFG1, 0x53);
	if (ret)
		return ret;

	/* INTF_CFG2: enable DIN receive */
	ret = regmap_write(priv->regmap, TAC5212_INTF_CFG2,
			   TAC5212_PASI_DIN_EN);
	if (ret)
		return ret;

	/* ASI_CFG0: Primary ASI enabled, Secondary disabled */
	ret = regmap_write(priv->regmap, TAC5212_ASI_CFG0,
			   TAC5212_SASI_DIS);
	if (ret)
		return ret;

	/* MISC_CFG: ignore clock errors so TAC can init before BCLK is present */
	ret = regmap_update_bits(priv->regmap, TAC5212_MISC_CFG, BIT(6), BIT(6));
	if (ret)
		return ret;

	/*
	 * PASI_TX_CFG0 (per AN sbaa383c):
	 *   TX_FILL=1 (Hi-Z unused cycles), TX_LSB=0 (no half-cycle delay at 48kHz)
	 *   TAC0 (closest to host): TX_KEEPER=01 (bus keeper enabled) → 0x48
	 *   TAC1-3: TX_KEEPER=00 (bus keeper disabled) → 0x40
	 *   TX_EDGE set dynamically in hw_params for rates >= 96kHz
	 */
	ret = regmap_write(priv->regmap, TAC5212_PASI_TX_CFG0,
			   priv->is_bus_closest ? 0x48 : 0x40);
	if (ret)
		return ret;

	/* PASI_TX_CFG1: TX_OFFSET=1 to align with SAI dsp_a (FSE=1) */
	ret = regmap_write(priv->regmap, TAC5212_PASI_TX_CFG1, 0x01);
	if (ret)
		return ret;

	/* PASI_RX_CFG0: RX_OFFSET=1 to align DAC input with SAI dsp_a */
	ret = regmap_write(priv->regmap, TAC5212_PASI_RX_CFG0, 0x01);
	if (ret)
		return ret;

	/* GPO1: PDMCLK output, active drive */
	ret = regmap_write(priv->regmap, TAC5212_GPO1_CFG0, 0x41);
	if (ret)
		return ret;

	/* GPI1: enable as input (for PDM data) */
	ret = regmap_write(priv->regmap, TAC5212_GPI_CFG, 0x02);
	if (ret)
		return ret;

	/* INTF_CFG4: pre-configure PDM_DIN1=GPI1, default analog.
	 * DAPM mux controls PDM_CH_SEL bits when user switches. */
	ret = regmap_write(priv->regmap, TAC5212_INTF_CFG4,
			   (0x03 << TAC5212_PDM_DIN1_SEL_SHIFT));
	if (ret)
		return ret;

	/* PASI_CFG0: TDM mode, 32-bit word length */
	ret = regmap_write(priv->regmap, TAC5212_PASI_CFG0, 0x30);
	if (ret)
		return ret;

	/* Configure TDM slots at probe time so clocks start immediately */
	/* TX slot assignments (ADC data out) */
	ret = regmap_write(priv->regmap, TAC5212_PASI_TX_CH1_CFG,
			   TAC5212_CH_EN_BIT | (priv->base_slot & TAC5212_CH_SLOT_MASK));
	if (ret)
		return ret;
	ret = regmap_write(priv->regmap, TAC5212_PASI_TX_CH2_CFG,
			   TAC5212_CH_EN_BIT | ((priv->base_slot + 1) & TAC5212_CH_SLOT_MASK));
	if (ret)
		return ret;
	/* Disable TX channels 3-8 */
	for (int i = TAC5212_PASI_TX_CH3_CFG; i <= TAC5212_PASI_TX_CH8_CFG; i++)
		regmap_write(priv->regmap, i, 0x00);

	/* RX slot assignments (DAC data in) */
	ret = regmap_write(priv->regmap, TAC5212_PASI_RX_CH1_CFG,
			   TAC5212_CH_EN_BIT | (priv->base_slot & TAC5212_CH_SLOT_MASK));
	if (ret)
		return ret;
	ret = regmap_write(priv->regmap, TAC5212_PASI_RX_CH2_CFG,
			   TAC5212_CH_EN_BIT | ((priv->base_slot + 1) & TAC5212_CH_SLOT_MASK));
	if (ret)
		return ret;
	/* Disable RX channels 3-8 */
	for (int i = TAC5212_PASI_RX_CH3_CFG; i <= TAC5212_PASI_RX_CH8_CFG; i++)
		regmap_write(priv->regmap, i, 0x00);

	/* Clock: all TACs are targets, PLL auto-locks on BCLK from SAI7 */
	ret = regmap_write(priv->regmap, TAC5212_CLK_CFG2,
			   TAC5212_AUTO_PLL_FR_ALLOW);
	if (ret)
		return ret;
	ret = regmap_write(priv->regmap, TAC5212_CLK_CFG0, 0x00);

	/* Enable channels and power up */
	ret = regmap_write(priv->regmap, TAC5212_CH_EN,
			   TAC5212_IN_CH1_EN | TAC5212_IN_CH2_EN |
			   TAC5212_OUT_CH1_EN | TAC5212_OUT_CH2_EN);
	if (ret)
		return ret;
	ret = regmap_write(priv->regmap, TAC5212_PWR_CFG,
			   TAC5212_ADC_PDZ | TAC5212_DAC_PDZ |
			   TAC5212_MICBIAS_PDZ);
	if (ret)
		return ret;

	/* Clear any latched clock errors from boot (no BCLK yet) */
	{
		unsigned int dummy;
		regmap_read(priv->regmap, TAC5212_CLK_ERR_STS0, &dummy);
		regmap_read(priv->regmap, TAC5212_CLK_ERR_STS1, &dummy);
		regmap_read(priv->regmap, TAC5212_DEV_STS0, &dummy);
		regmap_read(priv->regmap, TAC5212_DEV_STS1, &dummy);
	}

	/* Request SW reset on first stream open (BCLK needed for PDM init) */
	priv->needs_reset = true;

	dev_info(priv->dev, "TAC5212 initialized (I2C 0x%02x, slots %u-%u)\n",
		 priv->base_slot / 2 + TAC5212_I2C_BASE_ADDR,
		 priv->base_slot, priv->base_slot + 1);

	return 0;
}

static void tac5212_component_remove(struct snd_soc_component *component)
{
	struct tac5212_priv *priv = snd_soc_component_get_drvdata(component);

	/* Power down ADC and DAC */
	regmap_write(priv->regmap, TAC5212_PWR_CFG, 0x00);
	/* Enter sleep mode */
	regmap_update_bits(priv->regmap, TAC5212_DEV_MISC_CFG,
			   TAC5212_SLEEP_ENZ, 0);
}

static const struct snd_soc_component_driver tac5212_component_driver = {
	.probe			= tac5212_component_probe,
	.remove			= tac5212_component_remove,
	.controls		= tac5212_controls,
	.num_controls		= ARRAY_SIZE(tac5212_controls),
	.dapm_widgets		= tac5212_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(tac5212_dapm_widgets),
	.dapm_routes		= tac5212_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(tac5212_dapm_routes),
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 1,
};

static int tac5212_i2c_probe(struct i2c_client *client)
{
	struct tac5212_priv *priv;
	struct device *dev = &client->dev;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->regmap = devm_regmap_init_i2c(client, &tac5212_regmap_config);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(dev, PTR_ERR(priv->regmap),
				     "failed to init regmap\n");

	mutex_init(&priv->paged_lock);

	/* Seed the biquad cache to chip hard-reset defaults : unity all-pass
	 * { N0 = 0x7FFFFFFF (Q1.31 ≈ +1.0), N1 = N2 = D1 = D2 = 0 }. ALSA
	 * reads will then return the same blob the chip currently holds. */
	{
		int i;
		for (i = 0; i < TAC5212_N_BIQUADS; i++) {
			priv->adc_bq[i].bytes[0] = 0x7F;
			priv->adc_bq[i].bytes[1] = 0xFF;
			priv->adc_bq[i].bytes[2] = 0xFF;
			priv->adc_bq[i].bytes[3] = 0xFF;
			priv->dac_bq[i].bytes[0] = 0x7F;
			priv->dac_bq[i].bytes[1] = 0xFF;
			priv->dac_bq[i].bytes[2] = 0xFF;
			priv->dac_bq[i].bytes[3] = 0xFF;
		}
	}

	/* Derive TDM base slot from I2C address */
	priv->base_slot = (client->addr - TAC5212_I2C_BASE_ADDR) * 2;

	/* TAC0 (0x50) is closest to host on shared DOUT — gets bus keeper */
	priv->is_bus_closest = (client->addr == TAC5212_I2C_BASE_ADDR);

	/* Defaults */
	priv->tdm_slots = 8;
	priv->slot_width = 32;

	i2c_set_clientdata(client, priv);

	/*
	 * Enable SAI7 clock gates for SOF DSP operation.
	 * When SAI7 is disabled in DT, Linux doesn't open the AudioMix
	 * clock gates. TAC0 DT node has SAI7 clocks declared; we activate
	 * them here and keep them enabled permanently (devm-managed).
	 */
	{
		struct clk_bulk_data *clks;
		int num_clks;

		num_clks = devm_clk_bulk_get_all(dev, &clks);
		if (num_clks > 0) {
			int ret_clk = clk_bulk_prepare_enable(num_clks, clks);

			if (ret_clk)
				dev_warn(dev, "failed to enable clocks: %d\n",
					 ret_clk);
			else
				dev_info(dev, "SAI7 clocks enabled (%d)\n",
					 num_clks);
		}
	}


	return devm_snd_soc_register_component(dev, &tac5212_component_driver,
					       &tac5212_dai, 1);
}

static const struct of_device_id tac5212_of_match[] = {
	{ .compatible = "ti,tac5212" },
	{ }
};
MODULE_DEVICE_TABLE(of, tac5212_of_match);

static const struct i2c_device_id tac5212_i2c_id[] = {
	{ "tac5212" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tac5212_i2c_id);

static struct i2c_driver tac5212_i2c_driver = {
	.driver = {
		.name = "tac5212",
		.of_match_table = tac5212_of_match,
	},
	.probe = tac5212_i2c_probe,
	.id_table = tac5212_i2c_id,
};
module_i2c_driver(tac5212_i2c_driver);

MODULE_DESCRIPTION("TI TAC5212 Audio Codec driver");
MODULE_LICENSE("GPL");
