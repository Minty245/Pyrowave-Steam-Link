// Steam Link Pyrowave v7.26: HEVC full colour round trip on the host (lavapipe).
// Packs an RGB picture exactly like Steam Link Full Colour's PC shaders (C++ copy of slfc.cpp's HLSL), quantises to
// 10 bits, runs the Frame's unpack.comp through slp::Unpacker and compares:
//   - with the CPU reference of the same unpack (layout agreement PC <-> Frame, should be within quantisation),
//   - RGB vs the source (what each mode buys), next to a plain 4:2:0 conversion (Steam Link's own filter).
// usage: unpack_test in.rgb W H [vkformat]   (in.rgb: 8-bit RGB, gamma-encoded; vkformat default G16_B16R16_2PLANE_420)
#include "slp_unpack.hpp"
#include "volk.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace slp;
static int W, H;
static std::vector<float> R, G, B;   // source, 0..1
static const float K[3] = {0.2126f, 0.7152f, 0.0722f};
static float sat(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float s2l(float x) { return x <= 0.04045f ? x / 12.92f : powf((x + 0.055f) / 1.055f, 2.4f); }
static void rgb(int x, int y, float c[3])
{
	x = std::max(0, std::min(W - 1, x)); y = std::max(0, std::min(H - 1, y));
	c[0] = sat(R[y * W + x]); c[1] = sat(G[y * W + x]); c[2] = sat(B[y * W + x]);
}
static void cbcr(const float c[3], float out[2])
{
	float y = c[0] * K[0] + c[1] * K[1] + c[2] * K[2];
	out[0] = sat((c[2] - y) / 1.8556f + 0.5f);
	out[1] = sat((c[0] - y) / 1.5748f + 0.5f);
}
static std::vector<float> C2;   // 4:2:2 chroma W/2 x H x 2
static void c2_build()
{
	static const float w[8] = {-9, -16, 32, 122, 122, 32, -16, -9};
	C2.assign(size_t(W / 2) * H * 2, 0);
	for (int y = 0; y < H; y++)
		for (int i = 0; i < W / 2; i++) {
			float a[3] = {0, 0, 0}, c[3];
			for (int k = 0; k < 8; k++) {
				rgb(std::max(0, std::min(W - 1, 2 * i - 3 + k)), y, c);
				for (int j = 0; j < 3; j++) a[j] += c[j] * w[k];
			}
			for (int j = 0; j < 3; j++) a[j] /= 258.0f;
			cbcr(a, &C2[(size_t(y) * (W / 2) + i) * 2]);
		}
}
static void C(int i, int y, float o[2])
{
	i = std::max(0, std::min(W / 2 - 1, i));
	o[0] = C2[(size_t(y) * (W / 2) + i) * 2];
	o[1] = C2[(size_t(y) * (W / 2) + i) * 2 + 1];
}
static void chroma_at(int x, int y, float o[2])
{
	float t = (x - 0.5f) * 0.5f, fi = floorf(t), f = t - fi;
	float a[2], b[2];
	C(int(fi), y, a); C(int(fi) + 1, y, b);
	o[0] = a[0] + (b[0] - a[0]) * f; o[1] = a[1] + (b[1] - a[1]) * f;
}
static float luma_adjust(const float c[3], const float cc[2])
{
	float target = s2l(c[0]) * K[0] + s2l(c[1]) * K[1] + s2l(c[2]) * K[2], lo = 0, hi = 1, cb = cc[0] - 0.5f, cr = cc[1] - 0.5f;
	for (int it = 0; it < 12; it++) {
		float m = 0.5f * (lo + hi), r = m + 1.5748f * cr, b = m + 1.8556f * cb, g = (m - 0.2126f * r - 0.0722f * b) / 0.7152f;
		float l = s2l(sat(r)) * K[0] + s2l(sat(g)) * K[1] + s2l(sat(b)) * K[2];
		if (l < target) lo = m; else hi = m;
	}
	return 0.5f * (lo + hi);
}
// --- the PC's ps_y / ps_uv (slfc.cpp) ---
static float ps_y(int x, int yp, int mode, int Hp, int E)
{
	(void)Hp;
	float c[3];
	if (yp < H) {
		rgb(x, yp, c);
		if (mode == 1) { float cc[2]; chroma_at(x, yp, cc); return luma_adjust(c, cc); }
		return c[0] * K[0] + c[1] * K[1] + c[2] * K[2];
	}
	int r = yp - H, hw = W / 2;
	float o[2];
	if (mode == 1) {
		if (r >= E) return 0;
		if (x < hw) { C(x, 2 * r + 1, o); return o[0]; }
		C(x - hw, 2 * r + 1, o); return o[1];
	}
	if (mode >= 2) { if (r >= H) return 0; rgb(x, r, c); cbcr(c, o); return o[0]; }
	return 0;
}
static void ps_uv(int i, int j, int mode, int E, float out[2])
{
	int hh = H / 2;
	out[0] = out[1] = 0.5f;
	if (mode == 1) {
		if (j < hh) { C(i, 2 * j, out); return; }
		int r = E + (j - hh);
		if (r < hh) C(i, 2 * r + 1, out);
		return;
	}
	if (mode == 3) {   // tiled 4:4:4 (Full Colour 0.2+): copy of slfc.cpp's ps_uv
		if (j >= H) return;
		bool top = j < hh;
		int jj = top ? j : j - hh;
		int x = (i & ~31) * 2 + (i & 31), y = (jj & ~31) * 2 + (jj & 31) + (top ? 0 : 32);
		float c[3], o[2];
		rgb(x, y, c); cbcr(c, o); out[0] = o[1];
		rgb(x + 32, y, c); cbcr(c, o); out[1] = o[1];
		return;
	}
	if (mode == 2) {
		if (j >= H) return;
		float c[3], o[2];
		rgb(i, j, c); cbcr(c, o); out[0] = o[1];
		rgb(i + W / 2, j, c); cbcr(c, o); out[1] = o[1];
		return;
	}
	// mode 0: Steam Link's 4:2:0 (8-tap filter on both axes)
	static const float w[8] = {-9, -16, 32, 122, 122, 32, -16, -9};
	if (j >= hh) return;
	float a[3] = {0, 0, 0}, c[3];
	for (int ky = 0; ky < 8; ky++)
		for (int kx = 0; kx < 8; kx++) {
			rgb(2 * i - 3 + kx, std::max(0, std::min(H - 1, 2 * j - 3 + ky)), c);
			for (int q = 0; q < 3; q++) a[q] += c[q] * w[kx] * w[ky];
		}
	for (int q = 0; q < 3; q++) a[q] /= 258.0f * 258.0f;
	cbcr(a, out);
}
static uint16_t q10(float v) { int n = int(lrintf(sat(v) * 1023.0f)); return uint16_t(n << 6); }
static float d10(uint32_t v) { return float(v & 1023) / 1023.0f; }
static double psnr(double mse) { return mse > 0 ? 10 * log10(1.0 / mse) : 99.0; }

int main(int argc, char **argv)
{
	if (argc < 4) { printf("usage: unpack_test in.rgb W H [vkformat [pad_w pad_h]]\n"); return 1; }
	W = atoi(argv[2]); H = atoi(argv[3]);
	uint32_t fmt = argc > 4 ? uint32_t(atoi(argv[4])) : uint32_t(VK_FORMAT_G16_B16R16_2PLANE_420_UNORM);
	// v7.31: the decoder pads its surfaces (e.g. 1280 -> 1344 wide, 6832 -> 6848 tall); the padding is filled with
	// garbage here, so a rebuild that reads it, or a shader that normalises by the picture size, fails
	const int padw = argc > 6 ? atoi(argv[5]) : 0, padh = argc > 6 ? atoi(argv[6]) : 0;
	FILE *f = fopen(argv[1], "rb");
	std::vector<uint8_t> in(size_t(W) * H * 3);
	if (!f || fread(in.data(), 1, in.size(), f) != in.size()) { printf("cannot read %s\n", argv[1]); return 1; }
	fclose(f);
	R.resize(size_t(W) * H); G.resize(R.size()); B.resize(R.size());
	for (size_t i = 0; i < R.size(); i++) { R[i] = in[3 * i] / 255.0f; G[i] = in[3 * i + 1] / 255.0f; B[i] = in[3 * i + 2] / 255.0f; }
	c2_build();
	Unpacker u;
	std::string err;
	if (!u.init(&err)) { printf("init: %s\n", err.c_str()); return 1; }
	printf("GPU %s, %dx%d, surface format %u\n", u.device_name().c_str(), W, H, fmt);
	int fails = 0;
	for (int mode : {0, 1, 2, 3}) {
		int Hp = mode >= 2 ? 2 * H : mode == 1 ? (((H + H / 3) + 15) & ~15) : (((H + H / 3) + 15) & ~15);
		int E = Hp - H;
		const int Ws = W + padw, Hs = Hp + padh;
		std::vector<uint16_t> yp(size_t(Ws) * Hs, uint16_t(1023 << 6)), uv(size_t(Ws / 2) * (Hs / 2) * 2, uint16_t(77 << 6));
		// (mode 0 luma: plain Y')
		for (int y = 0; y < Hp; y++)
			for (int x = 0; x < W; x++) {
				float v;
				if (mode == 0) { float c[3]; rgb(x, y, c); v = y < H ? c[0] * K[0] + c[1] * K[1] + c[2] * K[2] : 0; }
				else v = ps_y(x, y, mode, Hp, E);
				yp[size_t(y) * Ws + x] = q10(v);
			}
		for (int j = 0; j < Hp / 2; j++)
			for (int i = 0; i < W / 2; i++) {
				float o[2];
				ps_uv(i, j, mode, E, o);
				uv[(size_t(j) * (Ws / 2) + i) * 2] = q10(o[0]);
				uv[(size_t(j) * (Ws / 2) + i) * 2 + 1] = q10(o[1]);
			}
		int t = u.create_test_surface(uint32_t(Ws), uint32_t(Hs), fmt, yp, uv, &err);
		if (t < 0) { printf("surface: %s\n", err.c_str()); return 1; }
		PackedLayout l;
		l.mode = mode; l.width = W; l.height = H; l.packed_height = Hp; l.strip = E;
		double ms = 0;
		std::vector<uint32_t> px;
		if (!u.unpack(t, l, &ms, &err) || !u.readback_rgba(t, px, &err)) { printf("unpack: %s\n", err.c_str()); return 1; }
		// CPU reference of the unpack from the quantised planes
		auto Yq = [&](int x, int y) { return (yp[size_t(y) * Ws + x] >> 6) / 1023.0f; };
		auto Cq = [&](int i, int j, int c) { return (uv[(size_t(j) * (Ws / 2) + i) * 2 + c] >> 6) / 1023.0f; };
		auto C422 = [&](int i, int y, float o[2]) {
			i = std::max(0, std::min(W / 2 - 1, i));
			if (!(y & 1)) { o[0] = Cq(i, y >> 1, 0); o[1] = Cq(i, y >> 1, 1); return; }
			int r = y >> 1;
			if (r < E) { o[0] = Yq(i, H + r); o[1] = Yq(W / 2 + i, H + r); return; }
			o[0] = Cq(i, H / 2 + (r - E), 0); o[1] = Cq(i, H / 2 + (r - E), 1);
		};
		double maxd = 0, mse_rgb = 0, mse_c = 0;
		int bad_below = 0;
		int bad_pad = 0;
		for (int y = 0; y < Hs; y++)
			for (int x = 0; x < Ws; x++) {
				uint32_t v = px[size_t(y) * Ws + x];
				float cr = d10(v), yy = d10(v >> 10), cb = d10(v >> 20);
				if (y >= H) { bad_below += (yy != 0.0f || fabsf(cb - 512 / 1023.0f) > 1e-3f); continue; }
				if (x >= W) { bad_pad += v != px[size_t(y) * Ws + W - 1]; continue; }   // repeats the last column
				float ey = Yq(x, y), ecb, ecr;
				if (mode == 3) {
					ecb = Yq(x, H + y);
					ecr = Cq((x & ~63) / 2 + (x & 31), (y & ~63) / 2 + (y & 31) + ((y & 32) ? H / 2 : 0), (x & 32) ? 1 : 0);
				}
				else if (mode == 2) { ecb = Yq(x, H + y); ecr = x < W / 2 ? Cq(x, y, 0) : Cq(x - W / 2, y, 1); }
				else if (mode == 1) {
					float t2 = (x - 0.5f) * 0.5f, fi = floorf(t2), fr = t2 - fi, a[2], b[2];
					C422(int(fi), y, a); C422(int(fi) + 1, y, b);
					ecb = a[0] + (b[0] - a[0]) * fr; ecr = a[1] + (b[1] - a[1]) * fr;
				} else {
					float tx = (x - 0.5f) * 0.5f, ty = (y - 0.5f) * 0.5f, fx = tx - floorf(tx), fy = ty - floorf(ty);
					int ax = std::max(0, std::min(W / 2 - 1, int(floorf(tx)))), bx = std::max(0, std::min(W / 2 - 1, int(floorf(tx)) + 1));
					int ay = std::max(0, std::min(H / 2 - 1, int(floorf(ty)))), by = std::max(0, std::min(H / 2 - 1, int(floorf(ty)) + 1));
					auto bil = [&](int c) {
						float t0 = Cq(ax, ay, c) + (Cq(bx, ay, c) - Cq(ax, ay, c)) * fx;
						float t1 = Cq(ax, by, c) + (Cq(bx, by, c) - Cq(ax, by, c)) * fx;
						return t0 + (t1 - t0) * fy;
					};
					ecb = bil(0); ecr = bil(1);
				}
				maxd = std::max(maxd, double(std::max(fabsf(yy - ey), std::max(fabsf(cb - ecb), fabsf(cr - ecr)))));
				// RGB vs source
				float r = yy + 1.5748f * (cr - 0.5f), b = yy + 1.8556f * (cb - 0.5f), g = (yy - 0.2126f * r - 0.0722f * b) / 0.7152f;
				float s[3];
				rgb(x, y, s);
				mse_rgb += (sat(r) - s[0]) * (sat(r) - s[0]) + (sat(g) - s[1]) * (sat(g) - s[1]) + (sat(b) - s[2]) * (sat(b) - s[2]);
				float so[2];
				cbcr(s, so);
				mse_c += (cb - so[0]) * (cb - so[0]) + (cr - so[1]) * (cr - so[1]);
			}
		double n = double(W) * H;
		bool ok = maxd <= 1.5 / 1023.0 && bad_below == 0 && bad_pad == 0;
		fails += !ok;
		printf("%s (%dx%d in %dx%d, surface %dx%d): GPU vs CPU unpack max diff %.2f LSB, area below %s, padding %s -> %s | "
		       "RGB PSNR %.2f dB, chroma PSNR %.2f dB  [%.2f ms]\n",
		       mode == 3 ? "4:4:4 tiled" : mode == 2 ? "4:4:4 split" : mode == 1 ? "4:2:2" : "4:2:0", W, H, W, Hp, Ws, Hs, maxd * 1023.0,
		       bad_below ? "NOT black" : "black", bad_pad ? "WRONG" : "edge",
		       ok ? "MATCH" : "MISMATCH", psnr(mse_rgb / (3 * n)), psnr(mse_c / (2 * n)), ms);
		u.release(t);
	}
	return fails ? 2 : 0;
}
