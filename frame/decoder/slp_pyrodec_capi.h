// Steam Link Pyrowave - C API of libslp_pyrodec.so (Frame-side Pyrowave decoder).
// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SLPD_API __attribute__((visibility("default")))

typedef struct slpd_decoder slpd_decoder;

typedef struct slpd_timing
{
	double cpu_ms;
	double gpu_decode_ms;
	double gpu_pack_copy_ms;
	int complete;
} slpd_timing;

// Private Vulkan device + Pyrowave 4:2:0 decoder for width x height. bits 8: NV12 client surfaces (Q08C),
// 10: P010 (Q10C). On success err holds the GPU name.
SLPD_API slpd_decoder *slpd_create(int width, int height, int bits, char *err, size_t errlen);
SLPD_API void slpd_destroy(slpd_decoder *dec);
// Imports a client P010 surface (dma-buf, explicit DRM modifier, 2 planes). The fd is dup()ed. Returns target or -1.
SLPD_API int slpd_import(slpd_decoder *dec, int dmabuf_fd, uint64_t modifier, const uint64_t offset[2],
                         const uint64_t pitch[2], uint64_t size, char *err, size_t errlen);
SLPD_API void slpd_release(slpd_decoder *dec, int target);
// Decodes one SLPW frame (or re-presents the last picture for a repeat frame) into target; waits for the GPU.
SLPD_API int slpd_decode(slpd_decoder *dec, const void *frame, size_t size, int target, slpd_timing *timing,
                         char *err, size_t errlen);
// Parses the SLPW header only. Returns 0 if valid.
SLPD_API int slpd_frame_info(const void *frame, size_t size, uint32_t *width, uint32_t *height, uint32_t *num_packets,
                             uint32_t *seq);

// v7.1 additions (the layer looks these up with dlsym and falls back to the calls above).
typedef struct slpd_timing2
{
	double cpu_ms;
	double gpu_decode_ms;
	double gpu_pack_copy_ms; // finish + copy
	double gpu_finish_ms;
	double gpu_copy_ms;
	int complete;
	int direct; // chroma written straight into the surface; 2 = luma too
} slpd_timing2;
// mode: 6 full colour (v7.19, see slpd_rgba_export), 0 dither pass + copies, 1 direct 8-bit (Pyrowave-side dither) + copies, 4 like 1 with chroma written straight
// into the surface, 5 like 4 with luma too (NV12 storage; falls back to 4). 2 and 3 are broken on the Frame's driver.
SLPD_API slpd_decoder *slpd_create2(int width, int height, int bits, int mode, char *err, size_t errlen);
SLPD_API int slpd_decode2(slpd_decoder *dec, const void *frame, size_t size, int target, slpd_timing2 *timing,
                          char *err, size_t errlen);
// Alias import status after the first slpd_import (for the log).
SLPD_API void slpd_alias_status(slpd_decoder *dec, char *buf, size_t len);

// v7.3 pipelined decode: slpd_submit records + submits and returns a ticket (or -1); slpd_wait blocks until that
// frame is on the surface. Two frames may be in flight; call submit from one thread and wait (in ticket order) from
// another. slpd_decode2 = submit + wait.
SLPD_API int slpd_submit(slpd_decoder *dec, const void *frame, size_t size, int target, char *err, size_t errlen);
SLPD_API int slpd_wait(slpd_decoder *dec, int ticket, slpd_timing2 *timing, char *err, size_t errlen);
typedef int (*pfn_slpd_submit)(slpd_decoder *, const void *, size_t, int, char *, size_t);
typedef int (*pfn_slpd_wait)(slpd_decoder *, int, slpd_timing2 *, char *, size_t);

// Dither on (default) / off. Direct 8-bit modes dither inside Pyrowave when the bundled libpyrowave supports it.
SLPD_API void slpd_set_dither(slpd_decoder *dec, int on);
typedef void (*pfn_slpd_set_dither)(slpd_decoder *, int);
// v7.5 luma post-filter (8-bit modes 0 / 1 / 4). deblock 0 = off, 1 = default (scaled by the frame's bits per pixel),
// up to 4. sharpen 0..1 on the gaze insets, full inside r0, fading to 0 at r1 (fractions of the tile half-size).
// Returns 1 if the filter is available in this decoder (8-bit surface, pipeline created), else 0.
SLPD_API int slpd_set_postfx(slpd_decoder *dec, float deblock, float sharpen, float r0, float r1);
typedef int (*pfn_slpd_set_postfx)(slpd_decoder *, float, float, float, float);
// v7.8: separate deblock strengths for the wide views and the insets; wide_step 1 or 2 (kernel reach 4 or 8 px).
SLPD_API int slpd_set_postfx2(slpd_decoder *dec, float deblock_wide, float deblock_inset, float sharpen, float r0, float r1,
                              float wide_step);
// v7.9: 0 = seam deblock (default), 1 = smoothing deblock (v7.8).
SLPD_API void slpd_set_postfx_mode(slpd_decoder *dec, int mode);
typedef void (*pfn_slpd_set_postfx_mode)(slpd_decoder *, int);
// v7.10: seam grids (0 = 16 px, 1 = +32, 2 = +32 +64) and whether strength follows bits per pixel (1) or not (0).
SLPD_API void slpd_set_postfx_seam(slpd_decoder *dec, int coarse, int bpp_scale);
typedef void (*pfn_slpd_set_postfx_seam)(slpd_decoder *, int, int);
/* v7.13: contrast-preserving blur of the wide views after decoding: strength px (0 = off), contrast 0..3. */
SLPD_API void slpd_set_postfx_blur(slpd_decoder *dec, float px, float contrast);
typedef void (*pfn_slpd_set_postfx_blur)(slpd_decoder *, float, float);
// v7.14: shared edge ramp (see Decoder::set_ramp), 16 floats
SLPD_API void slpd_set_ramp(slpd_decoder *dec, const float *r16);
typedef void (*pfn_slpd_set_ramp)(slpd_decoder *, const float *);
// v7.17: headset stability (see Decoder::set_postfx_temporal)
SLPD_API void slpd_set_postfx_temporal(slpd_decoder *dec, float amount, float clamp_width);
typedef void (*pfn_slpd_set_postfx_temporal)(slpd_decoder *, float, float);
// v7.19 full colour (create with mode 6). The shared 10-bit RGBA image of a target (R = Cr, G = Y, B = Cb):
typedef struct slpd_rgba
{
	int fd;                 // dma-buf, owned by the caller (close it)
	uint32_t width, height;
	uint32_t vk_format;     // VK_FORMAT_A2B10G10R10_UNORM_PACK32
	uint64_t modifier;      // 0 = DRM_FORMAT_MOD_LINEAR
	uint64_t offset, row_pitch, size;
} slpd_rgba;
SLPD_API int slpd_full_colour(slpd_decoder *dec);   // 1 if the decoder is in full-colour mode
SLPD_API int slpd_rgba_export(slpd_decoder *dec, int target, slpd_rgba *out, char *err, size_t errlen);   // 0 = ok
// out: 0 NV12 surface only, 1 RGBA image only, 2 both. Any thread; applies from the next submit for that target.
SLPD_API void slpd_set_target_output(slpd_decoder *dec, int target, int out);
SLPD_API int slpd_target_output(slpd_decoder *dec, int target);
// Chroma of an SLPW frame: 0 = 4:2:0, 1 = 4:4:4, -1 = not a valid frame.
SLPD_API int slpd_frame_chroma(const void *frame, size_t size);
// v7.21 power options. merge (default 1): fold the wide-view blur and inset sharpening into the full-colour pack on
// RGBA-only frames. fast_pyramid (default 1): bilinear taps for the blur pyramid. rgba_compressed (process-wide, set
// before slpd_create2; default 0): try a UBWC shared image. slpd_rgba_modifier: what a target's image got.
SLPD_API void slpd_set_merge(slpd_decoder *dec, int on);
SLPD_API void slpd_set_fast_pyramid(slpd_decoder *dec, int on);
SLPD_API void slpd_set_rgba_compressed(int on);
SLPD_API uint64_t slpd_rgba_modifier(slpd_decoder *dec, int target);
// Averages since the last call: GPU ms per pass (finish, seam deblock, blur pyramid, blur apply, deblock/sharpen,
// pack/copies) and of the whole submission. Returns the number of frames.
SLPD_API int slpd_pass_report(slpd_decoder *dec, double ms[6], double *total_ms);
SLPD_API const char *slpd_version(void);   // v7.22, e.g. "7.22"
typedef void (*pfn_slpd_set_merge)(slpd_decoder *, int);
typedef void (*pfn_slpd_set_fast_pyramid)(slpd_decoder *, int);
typedef void (*pfn_slpd_set_rgba_compressed)(int);
typedef uint64_t (*pfn_slpd_rgba_modifier)(slpd_decoder *, int);
typedef int (*pfn_slpd_pass_report)(slpd_decoder *, double *, double *);
typedef int (*pfn_slpd_full_colour)(slpd_decoder *);
typedef int (*pfn_slpd_rgba_export)(slpd_decoder *, int, slpd_rgba *, char *, size_t);
typedef void (*pfn_slpd_set_target_output)(slpd_decoder *, int, int);
typedef int (*pfn_slpd_target_output)(slpd_decoder *, int);
typedef int (*pfn_slpd_frame_chroma)(const void *, size_t);
typedef int (*pfn_slpd_set_postfx2)(slpd_decoder *, float, float, float, float, float, float);
typedef slpd_decoder *(*pfn_slpd_create2)(int, int, int, int, char *, size_t);
typedef int (*pfn_slpd_decode2)(slpd_decoder *, const void *, size_t, int, slpd_timing2 *, char *, size_t);
typedef void (*pfn_slpd_alias_status)(slpd_decoder *, char *, size_t);
typedef slpd_decoder *(*pfn_slpd_create)(int, int, int, char *, size_t);
typedef void (*pfn_slpd_destroy)(slpd_decoder *);
typedef int (*pfn_slpd_import)(slpd_decoder *, int, uint64_t, const uint64_t *, const uint64_t *, uint64_t, char *, size_t);
typedef void (*pfn_slpd_release)(slpd_decoder *, int);
typedef int (*pfn_slpd_decode)(slpd_decoder *, const void *, size_t, int, slpd_timing *, char *, size_t);
typedef int (*pfn_slpd_frame_info)(const void *, size_t, uint32_t *, uint32_t *, uint32_t *, uint32_t *);

#ifdef __cplusplus
}
#endif
