/**
 * @file av1/decode.c AV1 Decode
 *
 * Copyright (C) 2010 - 2016 Alfred E. Heggestad
 */

#include <string.h>
#include <re.h>
#include <re_av1.h>
#include <rem.h>
#include <baresip.h>
#include <aom/aom.h>
#include <aom/aom_decoder.h>
#include <aom/aomdx.h>
#include "av1.h"


enum {
	DECODE_MAXSZ = 524288,
};


struct viddec_state {
	aom_codec_ctx_t ctx;
	struct mbuf *mb;
	bool ctxup;
	bool started;
	uint16_t seq;
	unsigned w;
};


static void destructor(void *arg)
{
	struct viddec_state *vds = arg;

	if (vds->ctxup)
		aom_codec_destroy(&vds->ctx);

	mem_deref(vds->mb);
}


int av1_decode_update(struct viddec_state **vdsp, const struct vidcodec *vc,
		       const char *fmtp)
{
	struct viddec_state *vds;
	aom_codec_dec_cfg_t cfg = {
		.allow_lowbitdepth = 1
	};
	aom_codec_err_t res;
	int err = 0;
	(void)vc;
	(void)fmtp;

	if (!vdsp)
		return EINVAL;

	vds = *vdsp;

	if (vds)
		return 0;

	vds = mem_zalloc(sizeof(*vds), destructor);
	if (!vds)
		return ENOMEM;

	vds->mb = mbuf_alloc(1024);
	if (!vds->mb) {
		err = ENOMEM;
		goto out;
	}

	res = aom_codec_dec_init(&vds->ctx, &aom_codec_av1_dx_algo, &cfg, 0);
	if (res) {
		err = ENOMEM;
		goto out;
	}

	vds->ctxup = true;

 out:
	if (err)
		mem_deref(vds);
	else
		*vdsp = vds;

	return err;
}


static inline int16_t seq_diff(uint16_t x, uint16_t y)
{
	return (int16_t)(y - x);
}


static int copy_obu(struct mbuf *mb2, struct mbuf *mb, size_t size)
{
	struct av1_obu_hdr hdr;
	size_t end = mb->end;
	int err;

	mb->end = mb->pos + size;

	err = av1_obu_decode(&hdr, mb);
	if (err) {
		warning("av1: decode: could not decode OBU"
			" [%zu bytes]: %m\n", size, err);
		return err;
	}

#if 0
	debug("av1: decode: copy [%H]\n", av1_obu_print, &hdr);
#endif

	switch (hdr.type) {

	case OBU_SEQUENCE_HEADER:
	case OBU_FRAME_HEADER:
	case OBU_METADATA:
	case OBU_FRAME:
	case OBU_REDUNDANT_FRAME_HEADER:
		break;

	default:
		/* MUST be ignored by receivers. */
		warning("av1: decode: copy: unexpected obu type %u (%s)"
			" [x=%d, s=%d, size=%zu]\n",
			hdr.type, aom_obu_type_to_string(hdr.type),
			hdr.x, hdr.s, hdr.size);
		return EPROTO;
	}

	err = av1_obu_encode(mb2, hdr.type, true, hdr.size, mbuf_buf(mb));
	if (err)
		return err;

	mbuf_advance(mb, hdr.size);

	mb->end = end;

	return 0;
}


int av1_decode(struct viddec_state *vds, struct vidframe *frame,
	       bool *intra, bool marker, uint16_t seq, struct mbuf *mb)
{
	aom_codec_frame_flags_t flags;
	aom_codec_iter_t iter = NULL;
	aom_codec_err_t res;
	aom_image_t *img;
	struct av1_aggr_hdr hdr;
	struct mbuf *mb2 = NULL;
	size_t size;
	unsigned i;
	int err;

	if (!vds || !frame || !intra || !mb)
		return EINVAL;

	*intra = false;

	err = av1_aggr_hdr_decode(&hdr, mb);
	if (err)
		return err;

#if 0
	debug("av1: decode: header:  [%s]  [seq=%u, %4zu bytes]"
	      "  z=%u  y=%u  w=%u  n=%u\n",
	      marker ? "M" : " ",
	      seq, mbuf_get_left(mb),
	      hdr.z, hdr.y, hdr.w, hdr.n);
#endif

	if (hdr.n) {
		info("av1: new coded video sequence\n");

		/* Note: if N equals 1 then Z must equal 0. */
		if (hdr.z) {
			warning("av1: Note: if N equals 1 then"
				" Z must equal 0.\n");
		}
	}

	if (!hdr.z) {

		/* save the W obu count */
		vds->w = hdr.w;

		mbuf_rewind(vds->mb);
		vds->started = true;
	}
	else {
		if (!vds->started)
			return 0;

		if (seq_diff(vds->seq, seq) != 1) {
			mbuf_rewind(vds->mb);
			vds->started = false;
			return 0;
		}
	}

	vds->seq = seq;

	err = mbuf_write_mem(vds->mb, mbuf_buf(mb), mbuf_get_left(mb));
	if (err)
		goto out;

	if (!marker) {

		if (vds->mb->end > DECODE_MAXSZ) {
			warning("av1: decode buffer size exceeded\n");
			err = ENOMEM;
			goto out;
		}

		return 0;
	}

	vds->mb->pos = 0;

	mb2 = mbuf_alloc(1024);

	/* prepend Temporal Delimiter */
	err = av1_obu_encode(mb2, OBU_TEMPORAL_DELIMITER, true, 0, NULL);
	if (err)
		goto out;

	if (vds->w) {

		for (i=0; i<(vds->w-1); i++) {

			err = av1_leb128_decode(vds->mb, &size);
			if (err)
				goto out;

			err = copy_obu(mb2, vds->mb, size);
			if (err)
				goto out;
		}

		/* last OBU element MUST NOT be preceded by a length field */
		size = vds->mb->end - vds->mb->pos;

		err = copy_obu(mb2, vds->mb, size);
		if (err)
			goto out;
	}
	else {
		while (mbuf_get_left(vds->mb) >= 2) {

			/* each OBU element MUST be preceded by length field */
			err = av1_leb128_decode(vds->mb, &size);
			if (err)
				goto out;

			err = copy_obu(mb2, vds->mb, size);
			if (err)
				goto out;
		}
	}

	res = aom_codec_decode(&vds->ctx, mb2->buf,
			       (unsigned int)mb2->end, NULL);
	if (res) {
		warning("av1: decode error [w=%u, %zu bytes]: %s (%s)\n",
			hdr.w,
			mb2->end,
			aom_codec_err_to_string(res),
			aom_codec_error_detail(&vds->ctx));
		err = EPROTO;
		goto out;
	}

	img = aom_codec_get_frame(&vds->ctx, &iter);
	if (!img) {
		debug("av1: no picture\n");
		goto out;
	}

	res = aom_codec_control(&vds->ctx, AOMD_GET_FRAME_FLAGS, &flags);
	if (res == AOM_CODEC_OK) {
		if (flags & AOM_FRAME_IS_KEY)
			*intra = true;
	}

	if (img->fmt != AOM_IMG_FMT_I420) {
		warning("av1: bad pixel format (%i)\n", img->fmt);
		goto out;
	}

	for (i=0; i<3; i++) {
		frame->data[i]     = img->planes[i];
		frame->linesize[i] = img->stride[i];
	}

	frame->size.w = img->d_w;
	frame->size.h = img->d_h;
	frame->fmt    = VID_FMT_YUV420P;

 out:
	mbuf_rewind(vds->mb);
	vds->started = false;

	mem_deref(mb2);

	return err;
}
