#
# Topology for i.MX8MP with 4x TAC5212 on SAI7
# 8-channel TDM capture with DRC (noise gate/compressor)
# + 8-channel TDM playback with volume
#

# Include topology builder
include(`utils.m4')
include(`dai.m4')
include(`pipeline.m4')
include(`sai.m4')
include(`pcm.m4')
include(`buffer.m4')

# Include TLV library
include(`common/tlv.m4')

# Include Token library
include(`sof/tokens.m4')

# Include DSP configuration
include(`platform/imx/imx8.m4')

#
# Pipelines:
# PCM0 <--- DRC <--- SAI7 (8ch capture with noise gate/compressor)
# PCM1 ---> Volume ---> SAI7 (8ch playback)
#

# Capture pipeline with DRC (Dynamic Range Compressor)
PIPELINE_PCM_ADD(sof/pipe-drc-capture.m4,
	1, 0, 8, s32le,
	1000, 0, 0,
	48000, 48000, 48000)

# Playback pipeline with volume
PIPELINE_PCM_ADD(sof/pipe-volume-playback.m4,
	2, 1, 8, s32le,
	1000, 0, 0,
	48000, 48000, 48000)

# Capture DAI - SAI7
DAI_ADD(sof/pipe-dai-capture.m4,
	1, SAI, 7, tac5212-hifi,
	PIPELINE_SINK_1, 2, s32le,
	1000, 0, 0, SCHEDULE_TIME_DOMAIN_TIMER)

# Playback DAI - SAI7
DAI_ADD(sof/pipe-dai-playback.m4,
	2, SAI, 7, tac5212-hifi,
	PIPELINE_SOURCE_2, 2, s32le,
	1000, 0, 0, SCHEDULE_TIME_DOMAIN_TIMER)

# PCM_DUPLEX_ADD(name, id, playback_pipeline, capture_pipeline)
PCM_DUPLEX_ADD(TAC5212, 0, PIPELINE_PCM_2, PIPELINE_PCM_1)

# SAI7 DAI configuration
DAI_CONFIG(SAI, 7, 0, tac5212-hifi,
	SAI_CONFIG(DSP_A, SAI_CLOCK(mclk, 12288000, codec_mclk_in),
		SAI_CLOCK(bclk, 12288000, codec_consumer),
		SAI_CLOCK(fsync, 48000, codec_consumer),
		SAI_TDM(8, 32, 255, 255),
		SAI_CONFIG_DATA(SAI, 7, 0)))
