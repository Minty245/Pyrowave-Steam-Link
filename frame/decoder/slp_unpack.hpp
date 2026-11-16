// Steam Link Pyrowave Frame layer v7.26: HEVC full colour (Steam Link Full Colour) - rebuilds the full-colour picture
// from a packed 4:2:0 HEVC picture the hardware decoded into vrlink's surface (see unpack.comp for the layouts).
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace slp
{
struct RgbaExport;

struct PackedLayout
{
	int mode = 0;          // 0 plain 4:2:0, 1 packed 4:2:2, 2 packed 4:4:4
	int width = 0;         // W
	int height = 0;        // H: the picture
	int packed_height = 0; // Hp: the surface
	int strip = 0;         // E (4:2:2)
};

struct SurfaceDesc
{
	int dmabuf_fd = -1;          // borrowed; dup()ed
	uint32_t width = 0, height = 0;
	uint32_t vk_format = 0;      // vrlink's: G8_B8R8_2PLANE_420 (Q08C) or G10X6_B10X6R10X6_2PLANE_420_3PACK16 (Q10C)
	uint64_t modifier = 0;
	uint64_t plane_offset[2] = {0, 0};
	uint64_t plane_pitch[2] = {0, 0};
	uint64_t size = 0;
};

class Unpacker
{
public:
	Unpacker();
	~Unpacker();
	Unpacker(const Unpacker &) = delete;
	Unpacker &operator=(const Unpacker &) = delete;

	bool init(std::string *err);
	std::string device_name() const;
	// v7.33: shared RGBA images in the GPU's compressed tiling (UBWC) where the driver allows (default on); applies to
	// surfaces imported afterwards.
	void set_rgba_compressed(bool on);
	// Imports a decoder surface (read only) and creates its shared 10-bit RGBA image of the same size. Index or -1.
	int import_surface(const SurfaceDesc &d, std::string *err);
	void release(int index);
	// Rebuilds the picture into the surface's RGBA image and waits for the GPU. ms: GPU-side wall time (submit to done).
	bool unpack(int index, const PackedLayout &l, double *ms, std::string *err);
	bool rgba_export(int index, RgbaExport *out, std::string *err);

	// Self-test (hosts without dma-buf modifiers, e.g. lavapipe): a local 2-plane surface filled from 16-bit planes
	// (luma W x Hp, interleaved CbCr W/2 x Hp/2, values in the top bits), and the RGBA image read back.
	int create_test_surface(uint32_t w, uint32_t h, uint32_t vk_format, const std::vector<uint16_t> &y,
	                        const std::vector<uint16_t> &cbcr, std::string *err);
	bool readback_rgba(int index, std::vector<uint32_t> &px, std::string *err);

	struct Impl;
private:
	Impl *impl;
};
} // namespace slp
