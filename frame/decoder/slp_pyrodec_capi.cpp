// Steam Link Pyrowave - C API of the Frame decoder library (libslp_pyrodec.so), used by the
// Vulkan layer inside vrlink (which is plain C and stays free of C++ / newer glibc symbols).
// SPDX-License-Identifier: MIT
#include "slp_pyrodec_capi.h"
#include "slp_pyro_dec.hpp"

#include <cstdio>
#include <cstring>
#include <new>
#include <string>

using namespace slp;

static void copy_err(const std::string &e, char *err, size_t errlen)
{
	if (err && errlen)
		snprintf(err, errlen, "%s", e.c_str());
}

extern "C" {

SLPD_API slpd_decoder *slpd_create(int width, int height, int bits, char *err, size_t errlen)
{
	return slpd_create2(width, height, bits, 0, err, errlen);
}

SLPD_API slpd_decoder *slpd_create2(int width, int height, int bits, int mode, char *err, size_t errlen)
{
	Decoder *d = new (std::nothrow) Decoder();
	if (!d)
		return nullptr;
	std::string e;
	if (!d->init(width, height, bits, &e, mode))
	{
		copy_err(e, err, errlen);
		delete d;
		return nullptr;
	}
	copy_err(d->device_name(), err, errlen); // on success: device name
	return reinterpret_cast<slpd_decoder *>(d);
}

SLPD_API void slpd_destroy(slpd_decoder *dec)
{
	delete reinterpret_cast<Decoder *>(dec);
}

SLPD_API int slpd_import(slpd_decoder *dec, int dmabuf_fd, uint64_t modifier, const uint64_t offset[2],
                         const uint64_t pitch[2], uint64_t size, char *err, size_t errlen)
{
	TargetDesc t;
	t.dmabuf_fd = dmabuf_fd;
	t.modifier = modifier;
	t.plane_offset[0] = offset[0];
	t.plane_offset[1] = offset[1];
	t.plane_pitch[0] = pitch[0];
	t.plane_pitch[1] = pitch[1];
	t.alloc_size = size;
	std::string e;
	int r = reinterpret_cast<Decoder *>(dec)->import_target(t, &e);
	if (r < 0)
		copy_err(e, err, errlen);
	return r;
}

SLPD_API void slpd_release(slpd_decoder *dec, int target)
{
	reinterpret_cast<Decoder *>(dec)->release_target(target);
}

SLPD_API int slpd_decode(slpd_decoder *dec, const void *frame, size_t size, int target, slpd_timing *timing,
                         char *err, size_t errlen)
{
	DecodeTiming t;
	std::string e;
	bool ok = reinterpret_cast<Decoder *>(dec)->decode_into(static_cast<const uint8_t *>(frame), size, target, &t, &e);
	if (timing)
	{
		timing->cpu_ms = t.cpu_ms;
		timing->gpu_decode_ms = t.gpu_decode_ms;
		timing->gpu_pack_copy_ms = t.gpu_pack_copy_ms;
		timing->complete = t.complete ? 1 : 0;
	}
	if (!ok)
		copy_err(e, err, errlen);
	return ok ? 0 : -1;
}

SLPD_API int slpd_decode2(slpd_decoder *dec, const void *frame, size_t size, int target, slpd_timing2 *timing,
                          char *err, size_t errlen)
{
	DecodeTiming t;
	std::string e;
	bool ok = reinterpret_cast<Decoder *>(dec)->decode_into(static_cast<const uint8_t *>(frame), size, target, &t, &e);
	if (timing)
	{
		timing->cpu_ms = t.cpu_ms;
		timing->gpu_decode_ms = t.gpu_decode_ms;
		timing->gpu_pack_copy_ms = t.gpu_pack_copy_ms;
		timing->gpu_finish_ms = t.gpu_finish_ms;
		timing->gpu_copy_ms = t.gpu_copy_ms;
		timing->complete = t.complete ? 1 : 0;
		timing->direct = t.luma_direct ? 2 : t.direct ? 1 : 0;
	}
	if (!ok)
		copy_err(e, err, errlen);
	return ok ? 0 : -1;
}

SLPD_API int slpd_submit(slpd_decoder *dec, const void *frame, size_t size, int target, char *err, size_t errlen)
{
	std::string e;
	int tk = reinterpret_cast<Decoder *>(dec)->submit(static_cast<const uint8_t *>(frame), size, target, &e);
	if (tk < 0)
		copy_err(e, err, errlen);
	return tk;
}

SLPD_API int slpd_wait(slpd_decoder *dec, int ticket, slpd_timing2 *timing, char *err, size_t errlen)
{
	DecodeTiming t;
	std::string e;
	bool ok = reinterpret_cast<Decoder *>(dec)->wait(ticket, &t, &e);
	if (timing)
	{
		timing->cpu_ms = t.cpu_ms;
		timing->gpu_decode_ms = t.gpu_decode_ms;
		timing->gpu_pack_copy_ms = t.gpu_pack_copy_ms;
		timing->gpu_finish_ms = t.gpu_finish_ms;
		timing->gpu_copy_ms = t.gpu_copy_ms;
		timing->complete = t.complete ? 1 : 0;
		timing->direct = t.luma_direct ? 2 : t.direct ? 1 : 0;
	}
	if (!ok)
		copy_err(e, err, errlen);
	return ok ? 0 : -1;
}

SLPD_API void slpd_set_dither(slpd_decoder *dec, int on)
{
	reinterpret_cast<Decoder *>(dec)->set_dither(on != 0);
}

SLPD_API int slpd_set_postfx2(slpd_decoder *dec, float deblock_wide, float deblock_inset, float sharpen, float r0, float r1,
                              float wide_step)
{
	Decoder *d = reinterpret_cast<Decoder *>(dec);
	d->set_postfx2(deblock_wide, deblock_inset, sharpen, r0, r1, wide_step);
	return d->postfx_available() ? 1 : 0;
}

SLPD_API void slpd_set_postfx_mode(slpd_decoder *dec, int mode)
{
	reinterpret_cast<Decoder *>(dec)->set_postfx_mode(mode);
}

SLPD_API void slpd_set_postfx_seam(slpd_decoder *dec, int coarse, int bpp_scale)
{
	reinterpret_cast<Decoder *>(dec)->set_postfx_seam(coarse, bpp_scale != 0);
}

SLPD_API void slpd_set_postfx_temporal(slpd_decoder *dec, float amount, float clamp_width)
{
	if (dec)
		reinterpret_cast<Decoder *>(dec)->set_postfx_temporal(amount, clamp_width);
}

SLPD_API void slpd_set_ramp(slpd_decoder *dec, const float *r16)
{
	if (dec && r16)
		reinterpret_cast<Decoder *>(dec)->set_ramp(r16);
}

SLPD_API void slpd_set_postfx_blur(slpd_decoder *dec, float px, float contrast)
{
	reinterpret_cast<Decoder *>(dec)->set_postfx_blur(px, contrast);
}

SLPD_API int slpd_set_postfx(slpd_decoder *dec, float deblock, float sharpen, float r0, float r1)
{
	Decoder *d = reinterpret_cast<Decoder *>(dec);
	d->set_postfx(deblock, sharpen, r0, r1);
	return d->postfx_available() ? 1 : 0;
}

SLPD_API int slpd_full_colour(slpd_decoder *dec)
{
	return reinterpret_cast<Decoder *>(dec)->full_colour() ? 1 : 0;
}

SLPD_API int slpd_rgba_export(slpd_decoder *dec, int target, slpd_rgba *out, char *err, size_t errlen)
{
	if (!out)
		return -1;
	RgbaExport e;
	std::string es;
	if (!reinterpret_cast<Decoder *>(dec)->rgba_export(target, &e, &es))
	{
		copy_err(es, err, errlen);
		return -1;
	}
	out->fd = e.fd;
	out->width = e.width;
	out->height = e.height;
	out->vk_format = e.vk_format;
	out->modifier = e.modifier;
	out->offset = e.offset;
	out->row_pitch = e.row_pitch;
	out->size = e.size;
	return 0;
}

SLPD_API void slpd_set_target_output(slpd_decoder *dec, int target, int out)
{
	reinterpret_cast<Decoder *>(dec)->set_target_output(target, out);
}

SLPD_API int slpd_target_output(slpd_decoder *dec, int target)
{
	return reinterpret_cast<Decoder *>(dec)->target_output(target);
}

SLPD_API int slpd_frame_chroma(const void *frame, size_t size)
{
	FrameHeader h;
	const uint32_t *s;
	const uint8_t *p;
	if (!parse_frame(static_cast<const uint8_t *>(frame), size, &h, &s, &p))
		return -1;
	return h.chroma == 1 ? 1 : 0;
}

// v7.22: version of this library (the layer logs it)
SLPD_API const char *slpd_version(void)
{
	return "7.22";
}

// v7.21 power options and per-pass GPU times
SLPD_API void slpd_set_merge(slpd_decoder *dec, int on)
{
	reinterpret_cast<Decoder *>(dec)->set_merge(on != 0);
}

SLPD_API void slpd_set_fast_pyramid(slpd_decoder *dec, int on)
{
	reinterpret_cast<Decoder *>(dec)->set_fast_pyramid(on != 0);
}

SLPD_API void slpd_set_rgba_compressed(int on)
{
	Decoder::set_default_rgba_compressed(on != 0);
}

SLPD_API uint64_t slpd_rgba_modifier(slpd_decoder *dec, int target)
{
	return reinterpret_cast<Decoder *>(dec)->rgba_modifier(target);
}

SLPD_API int slpd_pass_report(slpd_decoder *dec, double ms[6], double *total_ms)
{
	return reinterpret_cast<Decoder *>(dec)->pass_report(ms, total_ms);
}

SLPD_API void slpd_alias_status(slpd_decoder *dec, char *buf, size_t len)
{
	copy_err(reinterpret_cast<Decoder *>(dec)->alias_status(), buf, len);
}

SLPD_API int slpd_frame_info(const void *frame, size_t size, uint32_t *width, uint32_t *height, uint32_t *num_packets,
                             uint32_t *seq)
{
	FrameHeader h;
	const uint32_t *s;
	const uint8_t *p;
	if (!parse_frame(static_cast<const uint8_t *>(frame), size, &h, &s, &p))
		return -1;
	if (width) *width = h.width;
	if (height) *height = h.height;
	if (num_packets) *num_packets = h.num_packets;
	if (seq) *seq = h.seq;
	return 0;
}
}
