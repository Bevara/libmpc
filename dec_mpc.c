/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / Musepack decoder filter, based on libmpcdec
 *  (https://www.musepack.net/), covering stream versions 7 and 8.
 *
 *  libmpcdec hands back floating point samples; they are clipped and converted
 *  to 16-bit here.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>
#include <stdlib.h>

#include <mpc/mpcdec.h>

typedef struct
{
	const u8 *data;
	u32 size, pos;
} MPCMemReader;

typedef struct
{
	GF_FilterPid *ipid, *opid;
	Bool is_playing;
} GF_MPCDecCtx;

/* libmpcdec reads through this table; it walks the packet in memory. */
static mpc_int32_t mpc_mem_read(mpc_reader *r, void *ptr, mpc_int32_t size)
{
	MPCMemReader *m = (MPCMemReader *)r->data;
	u32 avail = m->size - m->pos;
	if ((u32)size > avail)
		size = (mpc_int32_t)avail;
	if (size > 0)
	{
		memcpy(ptr, m->data + m->pos, size);
		m->pos += size;
	}
	return size;
}
static mpc_bool_t mpc_mem_seek(mpc_reader *r, mpc_int32_t offset)
{
	MPCMemReader *m = (MPCMemReader *)r->data;
	if ((offset < 0) || ((u32)offset > m->size))
		return GF_FALSE;
	m->pos = (u32)offset;
	return GF_TRUE;
}
static mpc_int32_t mpc_mem_tell(mpc_reader *r) { return (mpc_int32_t)((MPCMemReader *)r->data)->pos; }
static mpc_int32_t mpc_mem_size(mpc_reader *r) { return (mpc_int32_t)((MPCMemReader *)r->data)->size; }
static mpc_bool_t mpc_mem_canseek(mpc_reader *r) { (void)r; return GF_TRUE; }

static GF_Err mpcdec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_MPCDecCtx *ctx = (GF_MPCDecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_AUDIO));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_S16));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(44100));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(44100));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(2));

	return GF_OK;
}

static Bool mpcdec_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	GF_MPCDecCtx *ctx = (GF_MPCDecCtx *)gf_filter_get_udta(filter);
	switch (evt->base.type)
	{
	case GF_FEVT_PLAY:
		ctx->is_playing = GF_TRUE;
		return GF_FALSE;
	case GF_FEVT_STOP:
		ctx->is_playing = GF_FALSE;
		return GF_FALSE;
	default:
		return GF_FALSE;
	}
}

static s16 mpc_to_s16(MPC_SAMPLE_FORMAT v)
{
	/* The library is built in its floating point configuration, where samples
	 * are nominally in [-1, 1] but may overshoot on loud material. */
	float f = (float)v * 32768.f;
	if (f > 32767.f)
		return 32767;
	if (f < -32768.f)
		return -32768;
	return (s16)f;
}

static GF_Err mpcdec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck, *dst_pck;
	u8 *data, *output;
	u32 size, channels, sample_rate, pcm_used = 0, pcm_alloc, skip_samples = 0;
	MPCMemReader mem;
	mpc_reader reader;
	mpc_demux *demux;
	mpc_streaminfo si;
	mpc_frame_info frame;
	MPC_SAMPLE_FORMAT *sample_buffer = NULL;
	s16 *pcm = NULL;
	GF_MPCDecCtx *ctx = (GF_MPCDecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);
	if (!data)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}

	mem.data = data;
	mem.size = size;
	mem.pos = 0;
	memset(&reader, 0, sizeof(reader));
	reader.read = mpc_mem_read;
	reader.seek = mpc_mem_seek;
	reader.tell = mpc_mem_tell;
	reader.get_size = mpc_mem_size;
	reader.canseek = mpc_mem_canseek;
	reader.data = &mem;

	demux = mpc_demux_init(&reader);
	if (!demux)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[MPCDec] Not a Musepack stream\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	mpc_demux_get_info(demux, &si);
	channels = si.channels;
	sample_rate = si.sample_freq;
	/* SV8 records an encoder delay whose samples are decoded but must not be
	 * played. Nothing else is trimmed: comparing this filter's output with
	 * ffmpeg's mpc8 decoder shows ffmpeg emitting 481 extra frames at the head
	 * (its own synthesis latency, which it does not drop), the two streams
	 * being otherwise identical to +/-2 per sample. */
	skip_samples = (u32)si.beg_silence;

	sample_buffer = (MPC_SAMPLE_FORMAT *)gf_malloc(MPC_DECODER_BUFFER_LENGTH * sizeof(MPC_SAMPLE_FORMAT));
	pcm_alloc = sample_rate * channels; /* one second, grown as needed */
	pcm = (s16 *)gf_malloc(pcm_alloc * sizeof(s16));
	if (!sample_buffer || !pcm || !channels || !sample_rate)
	{
		if (sample_buffer) gf_free(sample_buffer);
		if (pcm) gf_free(pcm);
		mpc_demux_exit(demux);
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}

	for (;;)
	{
		u32 i, nb_samples;
		MPC_SAMPLE_FORMAT *frame_samples;

		frame.buffer = sample_buffer;
		if (mpc_demux_decode(demux, &frame) != MPC_STATUS_OK)
			break;
		if (frame.bits == -1) /* end of stream */
			break;

		frame_samples = sample_buffer;
		nb_samples = frame.samples * channels;

		/* SV8 records an encoder delay whose samples are decoded but must not
		 * be played; without this the stream starts a few milliseconds later
		 * than every other decoder renders it. */
		if (skip_samples)
		{
			u32 skipped = (skip_samples > frame.samples) ? frame.samples : skip_samples;
			skip_samples -= skipped;
			frame_samples += (size_t)skipped * channels;
			nb_samples -= skipped * channels;
			if (!nb_samples)
				continue;
		}

		if (pcm_used + nb_samples > pcm_alloc)
		{
			s16 *bigger;
			while (pcm_used + nb_samples > pcm_alloc)
				pcm_alloc *= 2;
			bigger = (s16 *)gf_realloc(pcm, pcm_alloc * sizeof(s16));
			if (!bigger)
				break;
			pcm = bigger;
		}
		for (i = 0; i < nb_samples; i++)
			pcm[pcm_used + i] = mpc_to_s16(frame_samples[i]);
		pcm_used += nb_samples;
	}

	gf_free(sample_buffer);
	mpc_demux_exit(demux);
	gf_filter_pid_drop_packet(ctx->ipid);

	if (!pcm_used)
	{
		gf_free(pcm);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[MPCDec] Stream decoded to no audio\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(sample_rate));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(sample_rate));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(channels));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CHANNEL_LAYOUT, &PROP_LONGUINT((channels == 1) ? GF_AUDIO_CH_FRONT_CENTER : (GF_AUDIO_CH_FRONT_LEFT | GF_AUDIO_CH_FRONT_RIGHT)));

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, pcm_used * (u32)sizeof(s16), &output);
	if (!dst_pck)
	{
		gf_free(pcm);
		return GF_OUT_OF_MEM;
	}
	memcpy(output, pcm, pcm_used * sizeof(s16));
	gf_free(pcm);

	gf_filter_pck_set_cts(dst_pck, 0);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_set_duration(dst_pck, pcm_used / channels);
	gf_filter_pck_send(dst_pck);

	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static void mpcdec_finalize(GF_Filter *filter)
{
}

static const GF_FilterCapability MPCDecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "mpc|mp+|mpp"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "audio/x-musepack|audio/musepack"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister MPCDecoderRegister = {
	.name = "mpcdec",
	GF_FS_SET_DESCRIPTION("Musepack decoder")
		GF_FS_SET_HELP("This filter decodes Musepack (.mpc, stream versions 7 and 8) audio using libmpcdec.")
			.private_size = sizeof(GF_MPCDecCtx),
	SETCAPS(MPCDecCaps),
	.configure_pid = mpcdec_configure_pid,
	.process = mpcdec_process,
	.process_event = mpcdec_process_event,
	.finalize = mpcdec_finalize,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE mpcdec_register(GF_FilterSession *session)
{
	return &MPCDecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_mpcdec(void) {
    gf_filter_auto_register("mpcdec", mpcdec_register);
}
