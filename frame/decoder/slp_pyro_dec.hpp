// Steam Link Pyrowave - Frame-side GPU decoder core.
// SPDX-License-Identifier: MIT
//
// Owns a private Vulkan instance/device (not vrlink's), a Pyrowave decoder on it, and the
// intermediate planes. decode_into() turns one SLPW frame into the 10-bit 4:2:0 contents of a
// client surface that was imported from a dma-buf (vrlink's UBWC P010 decode image):
//   Pyrowave compute decode -> Y/Cb/Cr R16 planes -> chroma pack (Cb,Cr -> R16G16)
//   -> vkCmdCopyImage plane 0 / plane 1 into the imported surface -> CPU fence wait.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace slp
{
// ---- wire format (one SLPW frame per encoded picture) ----
// header (32 bytes, little endian), then num_packets x u32 packet sizes, then the packets back to back.
// v7.11 (PC backend v36): an optional 48-byte settings trailer follows the packets (parsers ignore bytes past the
// payload): u32 'SLPS' 0x53504c53, u16 version 1, u16 size 48, u32 generation, f32 deblock_wide, deblock_inset,
// sharpen, sharpen_r0, sharpen_r1, wide_step, u8 deblock_mode, deblock_coarse, deblock_bpp, flags (bit 0 post-filter
// on), f32 wide_blur px, f32 wide_blur_contrast (v7.13; zero = off, were reserved). The Frame layer applies it when the
// generation changes.
constexpr uint32_t kFrameMagic = 0x57504c53u; // "SLPW"
constexpr uint8_t kFrameVersion = 1;
struct FrameHeader
{
	uint32_t magic;
	uint8_t version;
	uint8_t chroma;        // 0 = 4:2:0, 1 = 4:4:4
	uint16_t header_size;  // sizeof(FrameHeader)
	uint32_t width;
	uint32_t height;
	uint32_t seq;
	uint32_t num_packets;
	uint32_t payload_bytes; // sum of packet sizes
	uint32_t reserved;
};
static_assert(sizeof(FrameHeader) == 32, "wire header must be 32 bytes");

// Returns false if the buffer is not a complete, consistent SLPW frame.
bool parse_frame(const uint8_t *data, size_t size, FrameHeader *hdr, const uint32_t **sizes, const uint8_t **payload);

// ---- decoder ----
struct TargetDesc
{
	int dmabuf_fd = -1;          // borrowed; the decoder dup()s it
	uint64_t modifier = 0;       // DRM format modifier (vrlink: 0x0500000000000001)
	uint64_t plane_offset[2] = {0, 0};
	uint64_t plane_pitch[2] = {0, 0};
	uint64_t alloc_size = 0;     // dma-buf size
	bool readable = false;       // also TRANSFER_SRC (self-test readback)
};

struct DecodeTiming
{
	double cpu_ms = 0;       // push packets .. fence signalled
	double gpu_decode_ms = 0; // GPU timestamps: Pyrowave decode
	double gpu_pack_copy_ms = 0; // GPU timestamps: finishing pass + 2 plane copies
	double gpu_finish_ms = 0;    //   of which the finishing pass (pack / 8-bit conversion)
	double gpu_copy_ms = 0;      //   of which the two plane copies into the client surface
	bool complete = false;   // all packets of the frame were present
	bool direct = false;     // chroma written straight into the surface (no chroma copy)
	bool luma_direct = false; // luma written straight into the surface too (no copies at all)
	double gpu_start_ms = 0;  // submit -> GPU start (queue wait behind the previous frame / other work)
	int output = 0;           // v7.19: TargetOutput used for this frame
	bool chroma444 = false;   // v7.19: the frame was 4:4:4
	// v7.21: GPU time per pass: 0 finish (4:2:0 modes), 1 seam deblock, 2 blur pyramid, 3 blur apply,
	// 4 deblock / sharpen, 5 pack and / or copies
	double pass_ms[6] = {};
};

// How the decoded picture reaches an 8-bit (NV12) client surface.
enum OutputMode
{
	kModeDither = 0,       // Pyrowave -> R16 planes -> dithered 8-bit planes -> 2 copies into the surface (v6 default)
	kModeDirect8 = 1,      // Pyrowave -> R8 planes (no dither) -> chroma pack -> 2 copies
	kModeDitherAlias = 2,  // like 0, but the dither pass writes the surface directly (no copies)
	kModeDirect8Alias = 3, // BROKEN on Turnip (R8 UBWC alias != NV12 luma layout): R8 alias for luma + R8G8 alias for chroma
	kModeDirect8ChromaDirect = 4, // direct 8-bit; chroma pack writes surface chroma (R8G8 alias); luma copied
	kModeDirect8StorageLuma = 5,  // like 4, but Pyrowave writes luma through PLANE_0 of an NV12 storage view (falls back to 4)
	kModeFullColour = 6,          // v7.19: 16-bit planes and filters; each target also gets a shared 10-bit RGBA image
	                              // (R = Cr, G = Y, B = Cb) that vrcompositor samples instead of the NV12 surface.
	                              // Decodes 4:2:0 and 4:4:4 frames (FrameHeader.chroma). NV12 output = mode 0 minus the
	                              // luma post-filter (used until vrcompositor has switched a surface over).
};

// v7.19 full colour: the shared image of one target (dma-buf, single plane, LINEAR).
struct RgbaExport
{
	int fd = -1;               // owned by the caller (a dup)
	uint32_t width = 0, height = 0;
	uint32_t vk_format = 0;    // VK_FORMAT_A2B10G10R10_UNORM_PACK32
	uint64_t modifier = 0;     // DRM_FORMAT_MOD_LINEAR
	uint64_t offset = 0, row_pitch = 0, size = 0;
};
// Per-target output in full-colour mode.
enum TargetOutput
{
	kOutNV12 = 0,   // the NV12 surface only (default)
	kOutRGBA = 1,   // the shared RGBA image only (vrcompositor reads it)
	kOutBoth = 2,   // both (while vrcompositor is switching that surface over)
};
// Mode 2 is broken on Turnip too (its R8 luma alias corrupts chroma). Direct 8-bit modes (1, 3, 4, 5) dither inside
// Pyrowave's final pass when the Pyrowave fork supports it (pyrowave_slp_has_output_dither) and dither is on.

class Decoder
{
public:
	Decoder();
	~Decoder();
	Decoder(const Decoder &) = delete;
	Decoder &operator=(const Decoder &) = delete;

	// width/height of the luma plane (1344x5376 for the Frame atlas). 4:2:0 only for now.
	// bits: 8 = NV12 client surfaces (Q08C), 10 = P010 (Q10C; GPU writes into UBWC P010 do not work on the Frame).
	// mode: OutputMode (8-bit only; 10-bit always uses the pack + copy path). Alias modes fall back to the
	// copy path per surface if the driver refuses the alias images (see alias_status()).
	bool init(int width, int height, int bits, std::string *err, int mode = kModeDither);
	std::string alias_status() const;
	bool pyrowave_dither_available() const;
	void set_dither(bool on);
	// v7.5 luma post-filter (8-bit modes with a luma copy: 0, 1, 4). deblock: 0 = off, 1 = default strength (scaled
	// by the frame's bits per pixel), up to 4. sharpen: 0..1 on the gaze insets, full inside r0 and fading to 0 at r1
	// (fractions of the tile half-size).
	void set_postfx(float deblock, float sharpen, float r0, float r1);
	// v7.8: deblock_wide on the wide views, deblock_inset on the insets (set_postfx = wide only, inset 0);
	// wide_step 1 or 2 = reach of the wide-view kernel (4 or 8 px).
	void set_postfx2(float deblock_wide, float deblock_inset, float sharpen, float r0, float r1, float wide_step);
	// v7.9: 0 = seam deblock (default: in place on the decoded plane, only across Pyrowave's 16-px block grid,
	// texture kept), 1 = smoothing deblock (v7.8 bilateral).
	void set_postfx_mode(int mode);
	// v7.10 seam options: coarse 0 = 16-px grid only, 1 = +32, 2 = +32 +64 (default); bpp_scale false = full strength
	// regardless of the frame's bits per pixel.
	void set_postfx_seam(int coarse, bool bpp_scale);
	// v7.12: use the light inset-only sharpen in seam mode (default on; the self-test turns it off for comparison).
	void set_postfx_light(bool on);
	// v7.13: contrast-preserving blur of the wide views after decoding: strength in px (0 = off, ~0.5..4), contrast
	// restoration 0..3. Runs after the seam deblock, in place.
	void set_postfx_blur(float px, float contrast);
	// v7.17: headset stability: blend last frame's wide-view result away from the gaze (0..1, 0 = off), clamped to
	// clamp_width x the local contrast. Any thread.
	void set_postfx_temporal(float amount, float clamp_width);
	// v7.14: shared edge ramp from the PC's SLPR block (16 floats: ramp 0..1, start deg, end deg, contrast overshoot
	// 0..1, then per eye inset centre cx, cy and projection ax, bx, ay, by in tile units). Any thread, per frame.
	void set_ramp(const float r[16]);
	bool postfx_blur_available() const;
	bool postfx_available() const;

	// v7.19 full colour (mode 6). rgba_export: a dup of the target's shared image fd + layout (false if the target
	// has none). set_target_output: TargetOutput, any thread, applies from the next submit for that target.
	bool full_colour() const;
	bool rgba_export(int index, RgbaExport *out, std::string *err);
	void set_target_output(int index, int out);
	int target_output(int index) const;
	// Self-test: the target's RGBA image (A2B10G10R10 words, width x height). v7.21 via_import: read it the way
	// vrcompositor does (export the dma-buf, import it with the reported modifier / layout, copy from that image).
	bool readback_rgba(int index, std::vector<uint32_t> &px, std::string *err, bool via_import = false);
	// v7.21 power options. merge (default on, any thread): fold the wide-view blur and the inset sharpening into the
	// RGBA pack on RGBA-only frames. fast_pyramid (default on, any thread): bilinear taps for the blur pyramid.
	// compressed (before init; default off): try a UBWC shared image instead of LINEAR.
	void set_merge(bool on);
	void set_fast_pyramid(bool on);
	static void set_default_rgba_compressed(bool on);
	uint64_t rgba_modifier(int index) const;   // modifier of a target's shared image (0 = LINEAR)
	// v7.21: averages since the last call: per-pass GPU ms (DecodeTiming::pass_ms order) and the whole submission;
	// returns the number of frames (0 = none).
	int pass_report(double ms[6], double *total_ms);

	// Imports a client surface; returns an index >= 0 or -1.
	int import_target(const TargetDesc &desc, std::string *err);
	void release_target(int index);

	// Decodes one SLPW frame into target `index` and waits for the GPU.
	bool decode_into(const uint8_t *frame, size_t size, int index, DecodeTiming *timing, std::string *err);
	// Pipelined form: submit() records and submits (returns a ticket, or -1), wait() blocks until that frame's GPU
	// work is done. Up to two frames in flight; submit() blocks while the slot of the frame two back is unwaited.
	// submit() and wait() may run on different threads (one submitter, one waiter, tickets waited in order).
	int submit(const uint8_t *frame, size_t size, int index, std::string *err);
	bool wait(int ticket, DecodeTiming *timing, std::string *err);

	// Self-test helpers.
	bool readback_target(int index, std::vector<uint16_t> &y, std::vector<uint16_t> &cbcr, std::string *err);
	// Reads the surface through a YCbCr sampler exactly like vrcompositor (identity model, full range, midpoint).
	bool readback_sampled(int index, std::vector<uint16_t> &y, std::vector<uint16_t> &cbcr, std::string *err);
	bool decode_cpu_reference(const uint8_t *frame, size_t size, std::vector<uint16_t> &y,
	                          std::vector<uint16_t> &cb, std::vector<uint16_t> &cr, std::string *err);
	// Allocates a surface the way vrlink does (same format/modifier/plane layout, exported as dma-buf).
	int create_client_like_surface(uint64_t modifier, TargetDesc *out_desc, std::string *err);

	// Self-test experiments on the current decoded planes. kind: 0 P010 linear (driver layout), 1 P010 UBWC
	// (vrlink layout), 2 NV12 UBWC (vrlink 8-bit layout), 3 P010 UBWC written via a linear P010 intermediate.
	// method: 0 vkCmdCopyImage, 1 vkCmdCopyBufferToImage. Result is read back through a YCbCr sampler.
	bool write_experiment(int kind, int method, std::vector<uint16_t> &y, std::vector<uint16_t> &cbcr, std::string *err);

	// Experiment switch: transition the client surface from GENERAL (keep contents/metadata) instead of UNDEFINED.
	void set_preserve_surface_layout(bool preserve);

	std::string device_name() const;
	void *pyrowave_device_handle() const;

	struct Impl;
private:
	Impl *impl;
};
} // namespace slp
