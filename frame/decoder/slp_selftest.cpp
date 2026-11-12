// Steam Link Pyrowave - Frame GPU self-test.
// SPDX-License-Identifier: MIT
//
// Runs as a normal user process; does not touch vrlink, the compositor or any system file.
//   1. creates a private Vulkan device + Pyrowave on it
//   2. encodes a synthetic 1344x5376 4:2:0 test pattern with Pyrowave (budget as at 350 Mbps / 144 Hz)
//   3. allocates a surface exactly like vrlink's decode surface (P010, UBWC modifier, dma-buf export)
//   4. imports that dma-buf the way the layer will, decodes into it (decode -> pack -> plane copies)
//   5. reads it back and compares against Pyrowave's own CPU-readback decode and the source pattern
//   6. times N decodes
//   7. v20: full colour (mode 6): the shared 10-bit RGBA image for 4:2:0 and 4:4:4 frames, NV12 fallback, timings
//      v21: v7.21 power changes (merged pack bit-identical, fast pyramid, import round trip, compressed image, per-pass
//      GPU times)
// Output: text report; also writes the encoded SLPW frame to ./slp_testframe.slpw for later layer tests.

#include "slp_pyro_dec.hpp"
#include "volk.h"
#include "pyrowave.h"

#include <gnu/libc-version.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

using namespace slp;

static uint16_t pattern_y(int x, int y, int w, int h)
{
	// Each of the 4 tiles (1344x1344) gets a different mix: gradient, fine checker, rings, text-like bars.
	int tile = y / (h / 4);
	int ty = y % (h / 4);
	double fx = double(x) / w, fy = double(ty) / (h / 4);
	double v = 0;
	switch (tile)
	{
	case 0: v = 0.5 * fx + 0.5 * fy; break;
	case 1: v = (((x >> 3) ^ (ty >> 3)) & 1) ? 0.85 : 0.15; break;
	case 2: { double dx = fx - 0.5, dy = fy - 0.5; v = 0.5 + 0.5 * std::sin(80.0 * std::sqrt(dx * dx + dy * dy)); break; }
	default: v = ((x / 2) % 7 < 3 && (ty / 12) % 2) ? 0.9 : 0.2 + 0.3 * fx; break;
	}
	return uint16_t(std::lround(std::min(1.0, std::max(0.0, v)) * 65535.0));
}

static uint16_t pattern_c(int x, int y, int cw, int ch, int which)
{
	double fx = double(x) / cw, fy = double(y) / ch;
	double v = which == 0 ? 0.5 + 0.35 * std::sin(6.28 * fx * 3) * fy : 0.5 + 0.35 * std::cos(6.28 * fy * 5) * (1 - fx);
	return uint16_t(std::lround(v * 65535.0));
}

static double psnr10(const uint16_t *a, int sa_shift, const uint16_t *b, int sb_shift, size_t n, size_t stride_a = 1,
                     size_t stride_b = 1, int *max_diff = nullptr)
{
	double se = 0;
	int md = 0;
	for (size_t i = 0; i < n; i++)
	{
		int va = a[i * stride_a] >> sa_shift, vb = b[i * stride_b] >> sb_shift;
		int d = va - vb;
		md = std::max(md, std::abs(d));
		se += double(d) * d;
	}
	if (max_diff)
		*max_diff = md;
	double mse = se / double(n);
	return mse == 0 ? 99.0 : 10.0 * std::log10(1023.0 * 1023.0 / mse);
}

int main(int argc, char **argv)
{
	int iters = 200;
	bool no_surface = false;
	uint64_t modifier = 0x0500000000000001ull; // QCOM compressed (UBWC), as vrlink
	size_t budget = 303819;                    // 350 Mbps at 144 Hz
	int surf_bits = 8;                         // 8 = NV12 (what the layer uses now), 10 = P010
	int only_mode = -1;                        // --mode N: run only that output mode
	int tile_w = 1344;                         // --width N: atlas width (4 stacked square views, height 4N)
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--no-surface")) no_surface = true;
		else if (!strcmp(argv[i], "--linear")) modifier = 0;
		else if (!strcmp(argv[i], "--budget") && i + 1 < argc) budget = size_t(atol(argv[++i]));
		else if (!strcmp(argv[i], "--bits") && i + 1 < argc) surf_bits = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mode") && i + 1 < argc) only_mode = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--width") && i + 1 < argc) tile_w = atoi(argv[++i]);
	}
	const int W = tile_w, H = 4 * tile_w, CW = W / 2, CH = H / 2;
	printf("slp_selftest v21b: glibc %s, %dx%d 4:2:0, %d-bit client surface (%s), budget %zu B, modifier 0x%016llx%s\n",
	       gnu_get_libc_version(), W, H, surf_bits, surf_bits == 8 ? "NV12 / Q08C" : "P010 / Q10C", budget, (unsigned long long)modifier,
	       no_surface ? " (no client surface)" : "");

	Decoder dec;
	std::string err;
	if (!dec.init(W, H, surf_bits, &err))
	{
		printf("FAIL init: %s\n", err.c_str());
		return 1;
	}
	printf("device: %s\n", dec.device_name().c_str());

	// --- encode the test pattern on the same Pyrowave device ---
	std::vector<uint16_t> sy(size_t(W) * H), scb(size_t(CW) * CH), scr(size_t(CW) * CH);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			sy[size_t(y) * W + x] = pattern_y(x, y, W, H);
	for (int y = 0; y < CH; y++)
		for (int x = 0; x < CW; x++)
		{
			scb[size_t(y) * CW + x] = pattern_c(x, y, CW, CH, 0);
			scr[size_t(y) * CW + x] = pattern_c(x, y, CW, CH, 1);
		}
	pyrowave_device pdev = static_cast<pyrowave_device>(dec.pyrowave_device_handle());
	pyrowave_encoder enc = nullptr;
	pyrowave_encoder_create_info eci = {};
	eci.device = pdev;
	eci.width = W;
	eci.height = H;
	eci.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
	if (pyrowave_encoder_create(&eci, &enc) != PYROWAVE_SUCCESS)
	{
		printf("FAIL pyrowave_encoder_create\n");
		return 1;
	}
	pyrowave_cpu_buffer in = {};
	in.data[0] = sy.data();
	in.data[1] = scb.data();
	in.data[2] = scr.data();
	in.row_stride_in_bytes[0] = size_t(W) * 2;
	in.row_stride_in_bytes[1] = in.row_stride_in_bytes[2] = size_t(CW) * 2;
	in.plane_size_in_bytes[0] = sy.size() * 2;
	in.plane_size_in_bytes[1] = in.plane_size_in_bytes[2] = scb.size() * 2;
	in.width = W;
	in.height = H;
	in.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P16;
	pyrowave_rate_control rc = {};
	rc.maximum_bitstream_size = budget;
	if (pyrowave_encoder_encode_cpu(enc, &in, &rc) != PYROWAVE_SUCCESS)
	{
		printf("FAIL pyrowave_encoder_encode_cpu\n");
		return 1;
	}
	size_t np = 0, outp = 0;
	pyrowave_encoder_compute_num_packets(enc, budget, &np);
	std::vector<pyrowave_packet> packets(np);
	std::vector<uint8_t> bits(budget + 65536);
	if (!np || pyrowave_encoder_packetize(enc, packets.data(), budget, &outp, bits.data(), bits.size()) != PYROWAVE_SUCCESS)
	{
		printf("FAIL packetize\n");
		return 1;
	}
	FrameHeader fh = {};
	fh.magic = kFrameMagic;
	fh.version = kFrameVersion;
	fh.chroma = 0;
	fh.header_size = sizeof(FrameHeader);
	fh.width = W;
	fh.height = H;
	fh.seq = 1;
	fh.num_packets = uint32_t(outp);
	std::vector<uint8_t> frame(sizeof(FrameHeader) + outp * 4);
	for (size_t i = 0; i < outp; i++)
	{
		uint32_t s = uint32_t(packets[i].size);
		memcpy(frame.data() + sizeof(FrameHeader) + i * 4, &s, 4);
		frame.insert(frame.end(), bits.data() + packets[i].offset, bits.data() + packets[i].offset + packets[i].size);
		fh.payload_bytes += s;
	}
	memcpy(frame.data(), &fh, sizeof(fh));
	pyrowave_encoder_destroy(enc);
	printf("encoded: %zu packet(s), %u payload bytes, SLPW frame %zu bytes\n", outp, fh.payload_bytes, frame.size());
	if (FILE *f = fopen("slp_testframe.slpw", "wb"))
	{
		fwrite(frame.data(), 1, frame.size(), f);
		fclose(f);
	}

	// --- CPU reference decode ---
	std::vector<uint16_t> ry, rcb, rcr;
	if (!dec.decode_cpu_reference(frame.data(), frame.size(), ry, rcb, rcr, &err))
	{
		printf("FAIL cpu reference decode: %s\n", err.c_str());
		return 1;
	}
	printf("reference decode vs source (10-bit PSNR): Y %.2f Cb %.2f Cr %.2f dB\n",
	       psnr10(ry.data(), 6, sy.data(), 6, ry.size()), psnr10(rcb.data(), 6, scb.data(), 6, rcb.size()),
	       psnr10(rcr.data(), 6, scr.data(), 6, rcr.size()));

	if (no_surface)
	{
		printf("client surface skipped (--no-surface); done.\n");
		return 0;
	}

	// Repeat frame (PC had nothing new): header only, num_packets = 0.
	std::vector<uint8_t> rep_frame(sizeof(FrameHeader));
	{
		FrameHeader rh = fh;
		rh.num_packets = 0;
		rh.payload_bytes = 0;
		rh.seq = 2;
		memcpy(rep_frame.data(), &rh, sizeof(rh));
	}

	struct Res
	{
		int mode = 0;
		bool ran = false, ok = false, rep_ok = false, direct = false;
		int md[3] = {0, 0, 0}, dmd = -1;
		bool luma_direct = false;
		double cpu = 0, cpu95 = 0, gd = 0, fin = 0, cp = 0, gd_nodither = -1, seq_ms = 0, pipe_ms = 0;
	};
	const char *mode_names[6] = {"0 dither pass + copies",    "1 pyro-dither + copies",    "2 (broken) dither->surf",
	                             "3 (broken) direct8->surf", "4 pyro-dither, C direct",   "5 pyro-dither, Y+C direct"};
	const int sh = surf_bits == 8 ? 8 : 6;
	auto avg = [](const std::vector<double> &v) {
		double s = 0;
		for (double x : v) s += x;
		return v.empty() ? 0.0 : s / v.size();
	};
	auto stat = [](std::vector<double> v, const char *name) {
		std::sort(v.begin(), v.end());
		double s = 0;
		for (double x : v) s += x;
		printf("    %-26s avg %6.3f  p50 %6.3f  p95 %6.3f  max %6.3f ms\n", name, s / v.size(), v[v.size() / 2],
		       v[size_t(v.size() * 0.95)], v.back());
	};

	auto run_mode = [&](Decoder &d, int mode) -> Res {
		Res res;
		res.mode = mode;
		printf("=== mode %s ===\n", mode_names[mode]);
		TargetDesc desc, desc2;
		if (d.create_client_like_surface(modifier, &desc, &err) < 0 || d.create_client_like_surface(modifier, &desc2, &err) < 0)
		{
			printf("  FAIL client-like surface: %s\n", err.c_str());
			return res;
		}
		desc.readable = desc2.readable = true;
		int tgt = d.import_target(desc, &err);
		int tgt2 = tgt >= 0 ? d.import_target(desc2, &err) : -1;
		if (tgt < 0 || tgt2 < 0)
		{
			printf("  FAIL import: %s\n", err.c_str());
			return res;
		}
		printf("  surface: %s\n", d.alias_status().c_str());
		DecodeTiming tm;
		d.set_dither(false);
		std::vector<uint16_t> gy, gc;
		if (!d.decode_into(frame.data(), frame.size(), tgt, &tm, &err) || !d.readback_sampled(tgt, gy, gc, &err))
		{
			printf("  FAIL decode/readback: %s\n", err.c_str());
			return res;
		}
		res.ran = true;
		res.direct = tm.direct;
		res.luma_direct = tm.luma_direct;
		double py = psnr10(gy.data(), sh, ry.data(), sh, gy.size(), 1, 1, &res.md[0]);
		psnr10(gc.data(), sh, rcb.data(), sh, rcb.size(), 2, 1, &res.md[1]);
		psnr10(gc.data() + 1, sh, rcr.data(), sh, rcr.size(), 2, 1, &res.md[2]);
		res.ok = res.md[0] <= 1 && res.md[1] <= 1 && res.md[2] <= 1;
		printf("  via YCbCr sampler (as vrcompositor): max diff Y %d Cb %d Cr %d (8-bit units), Y 'PSNR' %.1f dB -> %s%s\n",
		       res.md[0], res.md[1], res.md[2], py, res.ok ? "MATCH" : "MISMATCH",
		       tm.luma_direct ? " [Y+C written directly]" : tm.direct ? " [C written directly]" : "");
		if (!res.ok)
		{
			printf("    luma per quarter:");
			size_t tile = size_t(W) * (H / 4);
			for (int t4 = 0; t4 < 4; t4++)
				printf(" %.1f", psnr10(gy.data() + t4 * tile, sh, ry.data() + t4 * tile, sh, tile));
			printf(" dB; samples Y[0]=%04x/%04x CbCr[0]=%04x,%04x/%04x,%04x\n", gy[0], ry[0], gc[0], gc[1], rcb[0], rcr[0]);
		}
		// Repeat frame into the other surface must show the same picture.
		std::vector<uint16_t> qy, qc;
		if (d.decode_into(rep_frame.data(), rep_frame.size(), tgt2, &tm, &err) && d.readback_sampled(tgt2, qy, qc, &err))
		{
			int a = 0, b = 0, c = 0;
			psnr10(qy.data(), sh, ry.data(), sh, qy.size(), 1, 1, &a);
			psnr10(qc.data(), sh, rcb.data(), sh, rcb.size(), 2, 1, &b);
			psnr10(qc.data() + 1, sh, rcr.data(), sh, rcr.size(), 2, 1, &c);
			res.rep_ok = a <= 1 && b <= 1 && c <= 1;
			printf("  repeat frame into a second surface: max diff Y %d Cb %d Cr %d -> %s\n", a, b, c, res.rep_ok ? "MATCH" : "MISMATCH");
		}
		else
			printf("  repeat frame: FAIL %s\n", err.c_str());
		if (mode == kModeDither || mode == kModeDitherAlias || d.pyrowave_dither_available())
		{
			d.set_dither(true);
			std::vector<uint16_t> dy, dc;
			if (d.decode_into(frame.data(), frame.size(), tgt, &tm, &err) && d.readback_sampled(tgt, dy, dc, &err))
			{
				int b = 0, c = 0;
				psnr10(dy.data(), sh, ry.data(), sh, dy.size(), 1, 1, &res.dmd);
				psnr10(dc.data(), sh, rcb.data(), sh, rcb.size(), 2, 1, &b);
				psnr10(dc.data() + 1, sh, rcr.data(), sh, rcr.size(), 2, 1, &c);
				res.dmd = std::max(res.dmd, std::max(b, c));
				printf("  dithered output: max diff %d (expected 1)%s\n", res.dmd,
				       (mode == kModeDither || mode == kModeDitherAlias) ? "" : " [dither inside Pyrowave's final pass]");
			}
		}
		std::vector<double> cpu, gd, fin, cp;
		int incomplete = 0;
		for (int i = 0; i < iters; i++)
		{
			if (!d.decode_into(frame.data(), frame.size(), (i & 1) ? tgt2 : tgt, &tm, &err))
			{
				printf("  FAIL decode_into #%d: %s\n", i, err.c_str());
				return res;
			}
			cpu.push_back(tm.cpu_ms);
			gd.push_back(tm.gpu_decode_ms);
			fin.push_back(tm.gpu_finish_ms);
			cp.push_back(tm.gpu_copy_ms);
			incomplete += !tm.complete;
		}
		printf("  timing over %d decodes (%d incomplete):\n", iters, incomplete);
		stat(cpu, "CPU push -> fence");
		stat(gd, "GPU Pyrowave decode");
		stat(fin, "GPU finish (dither/pack)");
		stat(cp, "GPU plane copies");
		if (mode != kModeDither && mode != kModeDitherAlias && d.pyrowave_dither_available())
		{
			// Same mode without the dither, to see what the dithered final pass costs.
			d.set_dither(false);
			std::vector<double> g2;
			for (int i = 0; i < iters / 2; i++)
				if (d.decode_into(frame.data(), frame.size(), (i & 1) ? tgt2 : tgt, &tm, &err))
					g2.push_back(tm.gpu_decode_ms);
			d.set_dither(true);
			if (!g2.empty())
			{
				stat(g2, "GPU decode, dither off");
				res.gd_nodither = avg(g2);
			}
		}
		// Throughput: back-to-back decodes, one at a time (v7.2) vs two in flight (v7.3, waiter thread).
		{
			const int N = std::max(60, iters);
			auto ts = std::chrono::steady_clock::now();
			for (int i = 0; i < N; i++)
				d.decode_into(frame.data(), frame.size(), (i & 1) ? tgt2 : tgt, &tm, &err);
			double seq_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ts).count() / N;
			std::vector<int> tickets(N, -1);
			std::vector<double> lat;
			std::mutex qm;
			std::condition_variable qcv;
			int submitted = 0;
			bool werr = false;
			ts = std::chrono::steady_clock::now();
			std::thread waiter([&] {
				for (int i = 0; i < N; i++)
				{
					int tkt;
					{
						std::unique_lock<std::mutex> lk(qm);
						qcv.wait(lk, [&] { return submitted > i; });
						tkt = tickets[i];
					}
					DecodeTiming wt;
					std::string e2;
					if (tkt < 0 || !d.wait(tkt, &wt, &e2))
						werr = true;
					else
						lat.push_back(wt.cpu_ms);
				}
			});
			for (int i = 0; i < N; i++)
			{
				int tkt = d.submit(frame.data(), frame.size(), (i & 1) ? tgt2 : tgt, &err);
				{
					std::lock_guard<std::mutex> lk(qm);
					tickets[i] = tkt;
					submitted = i + 1;
				}
				qcv.notify_all();
			}
			waiter.join();
			double pipe_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ts).count() / N;
			res.seq_ms = seq_ms;
			res.pipe_ms = pipe_ms;
			printf("  throughput: one at a time %.3f ms/frame (%.0f fps), two in flight %.3f ms/frame (%.0f fps)%s\n", seq_ms,
			       1000.0 / seq_ms, pipe_ms, 1000.0 / pipe_ms, werr ? " [ERRORS]" : "");
			if (!lat.empty())
				stat(lat, "submit -> done, pipelined");
		}
		// v7.5 luma post-filter: picture vs the source per tile (deblock should not hurt the PSNR, sharpening changes
		// only the even tiles), and what it costs on the GPU.
		if (d.postfx_available())
		{
			auto tile_psnr = [&](const std::vector<uint16_t> &g, int t4) {
				size_t tile = size_t(W) * (H / 4), off = t4 * tile;
				double se = 0;
				for (size_t i = 0; i < tile; i++)
				{
					double dv = (g[off + i] / 65535.0 - sy[off + i] / 65535.0) * 255.0;
					se += dv * dv;
				}
				double mse = se / double(tile);
				return mse <= 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
			};
			struct Cfg { int mode, coarse, bpp; float dw, di, sp, step; const char *name; int light = 1; float blur = 0, bc = 1, ramp = 0, ov = 0, tmp = 0; } cfgs[] = {
				{0, 2, 1, 0, 0, 0, 2, "off"}, {0, 2, 1, 0, 0, 0.5f, 2, "sharpen 0.5 only"},
				{0, 1, 1, 0.35f, 1, 0, 2, "Mia: seam w.35 i1, no sharp"}, {0, 1, 1, 0.35f, 1, 0.3f, 2, "Mia: + sharpen 0.3"},
				{0, 1, 1, 0, 0, 0, 2, "wide blur 1 px", 1, 1.0f, 1.0f}, {0, 1, 1, 0, 0, 0, 2, "wide blur 2 px", 1, 2.0f, 1.0f},
				{0, 1, 1, 0, 0, 0, 2, "wide blur 2 px, contrast 0", 1, 2.0f, 0.0f},
				{0, 1, 0, 0.25f, 0, 0, 2, "deblock w.25", 1, 0, 1, 0},
				{0, 1, 0, 0.25f, 0, 0, 2, "deblock w.25, ramp 1", 1, 0, 1, 1},
				{0, 1, 1, 0, 0, 0, 2, "blur 1.5 px, ramp 0.5", 1, 1.5f, 1.0f, 0.5f},
				{0, 1, 1, 0, 0, 0, 2, "blur 1.5 px, ramp 1", 1, 1.5f, 1.0f, 1.0f},
				{0, 1, 1, 0, 0, 0, 2, "blur 1.5, ramp 1, overshoot .5", 1, 1.5f, 1.0f, 1.0f, 0.5f},
				{0, 1, 0, 0.25f, 1, 0.5f, 2, "Balanced + blur 1.5 ramp 1", 1, 1.5f, 1.0f, 1.0f},
				{0, 1, 1, 0, 0, 0, 2, "headset stability 0.5 only", 1, 0, 1.0f, 0, 0, 0.5f},
				{0, 1, 0, 0.25f, 1, 0.5f, 2, "Balanced + blur 1 + stab .5", 1, 1.0f, 1.0f, 0.5f, 0, 0.5f},
				{0, 2, 1, 4, 0, 0, 2, "seam 16+32+64, wide 4"}};
			printf("  post-filter (luma vs source, 8-bit PSNR per tile: inset0 wide1 inset2 wide3; frame %.2f bits/pixel):\n",
			       double(fh.payload_bytes) * 8.0 / double(W * H));
			for (auto &c : cfgs)
			{
				d.set_postfx_mode(c.mode);
				d.set_postfx_light(c.light != 0);
				d.set_postfx_blur(c.blur, c.bc);
				d.set_postfx_temporal(c.tmp, 1.0f);   // v7.17
				{   // v7.14: shared edge ramp, Steam Frame default projection, gaze straight ahead
					const float r[16] = {c.ramp, 15.0f, 55.0f, c.ov, 0.58f, 0.40f, 0.32f, 0.58f, -0.33f, 0.40f,
					                     0.42f, 0.40f, 0.32f, 0.42f, -0.33f, 0.40f};
					d.set_ramp(r);
				}
				d.set_postfx_seam(c.coarse, c.bpp != 0);
				d.set_postfx2(c.dw, c.di, c.sp, 0.35f, 0.8f, c.step);
				std::vector<uint16_t> fy, fc;
				std::vector<double> fin2;
				bool okd = d.decode_into(frame.data(), frame.size(), tgt, &tm, &err) && d.readback_sampled(tgt, fy, fc, &err);
				for (int i = 0; okd && i < iters / 2; i++)
					if (d.decode_into(frame.data(), frame.size(), (i & 1) ? tgt2 : tgt, &tm, &err))
						fin2.push_back(tm.gpu_finish_ms);
				if (!okd)
				{
					printf("    %-31s FAIL %s\n", c.name, err.c_str());
					continue;
				}
				printf("    %-31s %6.2f %6.2f %6.2f %6.2f dB   GPU finish avg %.3f ms\n", c.name, tile_psnr(fy, 0),
				       tile_psnr(fy, 1), tile_psnr(fy, 2), tile_psnr(fy, 3), avg(fin2));
			}
			d.set_postfx_mode(0);
			d.set_postfx_light(true);
			d.set_postfx_blur(0.0f, 1.0f);
			d.set_postfx_temporal(0.0f, 1.0f);
			{
				const float r0[16] = {0.0f, 15.0f, 55.0f, 0.0f, 0.5f, 0.5f, 0.32f, 0.58f, -0.33f, 0.40f,
				                      0.5f, 0.5f, 0.32f, 0.42f, -0.33f, 0.40f};
				d.set_ramp(r0);
			}
			d.set_postfx_seam(2, true);
			d.set_postfx2(0.0f, 0.0f, 0.0f, 0.35f, 0.8f, 2.0f);
		}
		else
			printf("  post-filter: not available in this mode\n");
		res.cpu = avg(cpu);
		std::sort(cpu.begin(), cpu.end());
		res.cpu95 = cpu[size_t(cpu.size() * 0.95)];
		res.gd = avg(gd);
		res.fin = avg(fin);
		res.cp = avg(cp);
		return res;
	};

	std::vector<Res> results;
	// Modes 2 and 3 are known broken on Turnip (self-test v5); run them only on request. 5 last: experimental.
	for (int mode = 0; mode < (surf_bits == 8 ? 6 : 1); mode++)   // mode 6 (full colour) has its own section below
	{
		if (only_mode >= 0 ? mode != only_mode : (mode == 2 || mode == 3 || mode == 5))
			continue;
		if (mode == 0)
			results.push_back(run_mode(dec, 0));
		else
		{
			Decoder dm;
			if (!dm.init(W, H, surf_bits, &err, mode))
			{
				printf("=== mode %s ===\n  FAIL init: %s\n", mode_names[mode], err.c_str());
				continue;
			}
			results.push_back(run_mode(dm, mode));
		}
	}
	// ---- v20: full colour (mode 6) ----
	bool fc_ok = true;
	if (surf_bits == 8 && (only_mode < 0 || only_mode == kModeFullColour))
	{
		printf("\n=== full colour (mode 6): shared 10-bit RGBA image, R = Cr, G = Y, B = Cb ===\n");
		Decoder df;
		auto fcfail = [&](const char *what) {
			printf("  FAIL %s: %s\n", what, err.c_str());
			fc_ok = false;
		};
		do
		{
			if (!df.init(W, H, surf_bits, &err, kModeFullColour)) { fcfail("init"); break; }
			// 4:4:4 test frame: full-size chroma with a per-pixel Cr checker (any subsampling would average it away)
			std::vector<uint8_t> f444;
			{
				std::vector<uint16_t> qy(size_t(W) * H), qb(size_t(W) * H), qr(size_t(W) * H);
				for (int y = 0; y < H; y++)
					for (int x = 0; x < W; x++)
					{
						size_t i = size_t(y) * W + x;
						qy[i] = sy[i];
						qb[i] = pattern_c(x, y, W, H, 0);
						int v = int(pattern_c(x, y, W, H, 1)) + (((x ^ y) & 1) ? 4000 : -4000);
						qr[i] = uint16_t(std::min(65535, std::max(0, v)));
					}
				pyrowave_encoder e4 = nullptr;
				pyrowave_encoder_create_info c4 = {};
				c4.device = static_cast<pyrowave_device>(df.pyrowave_device_handle());
				c4.width = W;
				c4.height = H;
				c4.chroma = PYROWAVE_CHROMA_SUBSAMPLING_444;
				if (pyrowave_encoder_create(&c4, &e4) != PYROWAVE_SUCCESS) { err = "pyrowave_encoder_create(4:4:4)"; fcfail("encoder"); break; }
				pyrowave_cpu_buffer in4 = {};
				in4.data[0] = qy.data();
				in4.data[1] = qb.data();
				in4.data[2] = qr.data();
				in4.row_stride_in_bytes[0] = in4.row_stride_in_bytes[1] = in4.row_stride_in_bytes[2] = size_t(W) * 2;
				in4.plane_size_in_bytes[0] = in4.plane_size_in_bytes[1] = in4.plane_size_in_bytes[2] = qy.size() * 2;
				in4.width = W;
				in4.height = H;
				in4.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV444P16;
				pyrowave_rate_control rc4 = {};
				rc4.maximum_bitstream_size = budget;
				size_t np4 = 0, out4 = 0;
				std::vector<uint8_t> bits4(budget + 65536);
				bool eok = pyrowave_encoder_encode_cpu(e4, &in4, &rc4) == PYROWAVE_SUCCESS;
				if (eok)
					pyrowave_encoder_compute_num_packets(e4, budget, &np4);
				std::vector<pyrowave_packet> pk4(np4);
				eok = eok && np4 &&
				      pyrowave_encoder_packetize(e4, pk4.data(), budget, &out4, bits4.data(), bits4.size()) == PYROWAVE_SUCCESS;
				pyrowave_encoder_destroy(e4);
				if (!eok) { err = "encode 4:4:4"; fcfail("encoder"); break; }
				FrameHeader h4 = fh;
				h4.chroma = 1;
				h4.seq = 3;
				h4.num_packets = uint32_t(out4);
				h4.payload_bytes = 0;
				f444.resize(sizeof(FrameHeader) + out4 * 4);
				for (size_t i = 0; i < out4; i++)
				{
					uint32_t sz = uint32_t(pk4[i].size);
					memcpy(f444.data() + sizeof(FrameHeader) + i * 4, &sz, 4);
					f444.insert(f444.end(), bits4.data() + pk4[i].offset, bits4.data() + pk4[i].offset + pk4[i].size);
					h4.payload_bytes += sz;
				}
				memcpy(f444.data(), &h4, sizeof(h4));
				printf("  encoded 4:4:4: %zu packet(s), %u payload bytes (4:2:0 frame: %u)\n", out4, h4.payload_bytes, fh.payload_bytes);
			}
			std::vector<uint16_t> y4, b4, r4;
			if (!df.decode_cpu_reference(f444.data(), f444.size(), y4, b4, r4, &err)) { fcfail("4:4:4 cpu reference"); break; }
			{
				// chroma fidelity: 4:4:4 reference vs its source (the 4:2:0 frame cannot carry the per-pixel checker at all)
				std::vector<uint16_t> qr(size_t(W) * H);
				for (int y = 0; y < H; y++)
					for (int x = 0; x < W; x++)
					{
						int v = int(pattern_c(x, y, W, H, 1)) + (((x ^ y) & 1) ? 4000 : -4000);
						qr[size_t(y) * W + x] = uint16_t(std::min(65535, std::max(0, v)));
					}
				printf("  4:4:4 reference vs source (10-bit PSNR): Y %.2f dB, Cr (with per-pixel checker) %.2f dB\n",
				       psnr10(y4.data(), 6, sy.data(), 6, y4.size()), psnr10(r4.data(), 6, qr.data(), 6, r4.size()));
			}
			TargetDesc da, db;
			if (df.create_client_like_surface(modifier, &da, &err) < 0 || df.create_client_like_surface(modifier, &db, &err) < 0)
			{ fcfail("client-like surface"); break; }
			da.readable = db.readable = true;
			int ta = df.import_target(da, &err), tb = ta >= 0 ? df.import_target(db, &err) : -1;
			if (ta < 0 || tb < 0) { fcfail("import"); break; }
			printf("  surface: %s; full colour %s\n", df.alias_status().c_str(), df.full_colour() ? "on" : "OFF");
			RgbaExport ex;
			if (df.rgba_export(ta, &ex, &err))
			{
				printf("  shared image: dma-buf fd ok, %ux%u, modifier 0x%llx, offset %llu, row pitch %llu, size %llu\n", ex.width,
				       ex.height, (unsigned long long)ex.modifier, (unsigned long long)ex.offset, (unsigned long long)ex.row_pitch,
				       (unsigned long long)ex.size);
				close(ex.fd);
			}
			else
				printf("  shared image: not exportable here (%s)%s\n", err.c_str(), " - fine on a host GPU, NOT on the Frame");
			df.set_dither(false);
			DecodeTiming tm;
			// unpack + compare (10-bit codes). ref chroma is cw x ch (half size for 4:2:0 -> bilinear at the midpoint)
			auto cmp_rgba = [&](const std::vector<uint32_t> &px, const std::vector<uint16_t> &ry_, const std::vector<uint16_t> &rb_,
			                    const std::vector<uint16_t> &rr_, int cw, int ch, int md[3]) {
				md[0] = md[1] = md[2] = 0;
				auto ref_c = [&](const std::vector<uint16_t> &p, int x, int y) -> double {
					if (cw == W)
						return p[size_t(y) * W + x] / 65535.0 * 1023.0;
					double fx = (x + 0.5) * cw / double(W) - 0.5, fy = (y + 0.5) * ch / double(H) - 0.5;
					int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
					double ax = fx - x0, ay = fy - y0;
					auto at = [&](int xx, int yy) {
						xx = std::min(cw - 1, std::max(0, xx));
						yy = std::min(ch - 1, std::max(0, yy));
						return p[size_t(yy) * cw + xx] / 65535.0 * 1023.0;
					};
					return (at(x0, y0) * (1 - ax) + at(x0 + 1, y0) * ax) * (1 - ay) + (at(x0, y0 + 1) * (1 - ax) + at(x0 + 1, y0 + 1) * ax) * ay;
				};
				for (int y = 0; y < H; y++)
					for (int x = 0; x < W; x++)
					{
						uint32_t v = px[size_t(y) * W + x];
						int R = int(v & 1023), G = int((v >> 10) & 1023), B = int((v >> 20) & 1023);
						int ey = std::abs(G - int(std::lround(ry_[size_t(y) * W + x] / 65535.0 * 1023.0)));
						int eb = std::abs(B - int(std::lround(ref_c(rb_, x, y))));
						int er = std::abs(R - int(std::lround(ref_c(rr_, x, y))));
						md[0] = std::max(md[0], ey);
						md[1] = std::max(md[1], eb);
						md[2] = std::max(md[2], er);
					}
			};
			auto row = [&](const char *name, int md[3], int tol) {
				bool ok = md[0] <= tol && md[1] <= tol && md[2] <= tol;
				printf("  %-44s max diff Y %d Cb %d Cr %d (10-bit codes) -> %s\n", name, md[0], md[1], md[2], ok ? "MATCH" : "MISMATCH");
				fc_ok = fc_ok && ok;
			};
			int md[3];
			std::vector<uint32_t> px;
			// a) NV12 output (before vrcompositor switches over): as mode 0
			std::vector<uint16_t> gy, gc;
			if (!df.decode_into(frame.data(), frame.size(), ta, &tm, &err) || !df.readback_sampled(ta, gy, gc, &err)) { fcfail("NV12 decode"); break; }
			psnr10(gy.data(), sh, ry.data(), sh, gy.size(), 1, 1, &md[0]);
			psnr10(gc.data(), sh, rcb.data(), sh, rcb.size(), 2, 1, &md[1]);
			psnr10(gc.data() + 1, sh, rcr.data(), sh, rcr.size(), 2, 1, &md[2]);
			{
				bool ok = md[0] <= 1 && md[1] <= 1 && md[2] <= 1;
				printf("  %-44s max diff Y %d Cb %d Cr %d (8-bit units) -> %s\n", "4:2:0 -> NV12 surface (fallback)", md[0], md[1], md[2],
				       ok ? "MATCH" : "MISMATCH");
				fc_ok = fc_ok && ok;
			}
			// b) 4:2:0 -> RGBA
			df.set_target_output(ta, kOutRGBA);
			if (!df.decode_into(frame.data(), frame.size(), ta, &tm, &err) || !df.readback_rgba(ta, px, &err)) { fcfail("4:2:0 RGBA"); break; }
			cmp_rgba(px, ry, rcb, rcr, CW, CH, md);
			row("4:2:0 -> RGBA (chroma bilinear, midpoint)", md, 1);
			// c) 4:4:4 -> RGBA
			if (!df.decode_into(f444.data(), f444.size(), ta, &tm, &err) || !df.readback_rgba(ta, px, &err)) { fcfail("4:4:4 RGBA"); break; }
			cmp_rgba(px, y4, b4, r4, W, H, md);
			row(tm.chroma444 ? "4:4:4 -> RGBA (full-size chroma)" : "4:4:4 -> RGBA [frame NOT seen as 4:4:4]", md, 1);
			// d) repeat after the 4:4:4 frame into the other target, RGBA
			df.set_target_output(tb, kOutRGBA);
			if (!df.decode_into(rep_frame.data(), rep_frame.size(), tb, &tm, &err) || !df.readback_rgba(tb, px, &err)) { fcfail("repeat"); break; }
			cmp_rgba(px, y4, b4, r4, W, H, md);
			row("repeat after 4:4:4 -> second target RGBA", md, 1);
			// e) both outputs, 4:4:4: RGBA exact, NV12 luma exact (chroma box-averaged, reported only)
			df.set_target_output(tb, kOutBoth);
			if (!df.decode_into(f444.data(), f444.size(), tb, &tm, &err) || !df.readback_rgba(tb, px, &err) ||
			    !df.readback_sampled(tb, gy, gc, &err)) { fcfail("both"); break; }
			cmp_rgba(px, y4, b4, r4, W, H, md);
			row("4:4:4 -> both: RGBA", md, 1);
			{
				int a = 0;
				psnr10(gy.data(), sh, y4.data(), sh, gy.size(), 1, 1, &a);
				printf("  %-44s max diff Y %d (8-bit units) -> %s\n", "4:4:4 -> both: NV12 luma", a, a <= 1 ? "MATCH" : "MISMATCH");
				fc_ok = fc_ok && a <= 1;
			}
			// f) dither on: within a code of the reference
			df.set_dither(true);
			if (df.decode_into(frame.data(), frame.size(), ta, &tm, &err) && df.readback_rgba(ta, px, &err))
			{
				cmp_rgba(px, ry, rcb, rcr, CW, CH, md);
				row("4:2:0 -> RGBA, dither on (expect <= 1)", md, 1);
			}
			// g) Balanced post-filter (seam, sharpen, blur, stability, ramp) on the RGBA path: runs, luma per tile vs source
			{
				const float r[16] = {0.2f, 15.0f, 55.0f, 0.2f, 0.58f, 0.40f, 0.32f, 0.58f, -0.33f, 0.40f,
				                     0.42f, 0.40f, 0.32f, 0.42f, -0.33f, 0.40f};
				df.set_ramp(r);
				df.set_postfx_mode(0);
				df.set_postfx_seam(0, true);
				df.set_postfx2(0.0f, 0.0f, 0.5f, 0.35f, 0.8f, 2.0f);
				df.set_postfx_blur(0.3f, 3.0f);
				df.set_postfx_temporal(0.6f, 0.6f);
				bool okd = true;
				for (int i = 0; okd && i < 3; i++)
					okd = df.decode_into(frame.data(), frame.size(), ta, &tm, &err);
				okd = okd && df.readback_rgba(ta, px, &err);
				if (!okd) { fcfail("post-filter on RGBA"); break; }
				printf("  Balanced post-filter on RGBA (luma vs source, 10-bit PSNR per tile):");
				size_t tile = size_t(W) * (H / 4);
				for (int t4 = 0; t4 < 4; t4++)
				{
					double se = 0;
					for (size_t i = 0; i < tile; i++)
					{
						double dv = double((px[t4 * tile + i] >> 10) & 1023) - sy[t4 * tile + i] / 65535.0 * 1023.0;
						se += dv * dv;
					}
					printf(" %.2f", se <= 0 ? 99.0 : 10.0 * std::log10(1023.0 * 1023.0 * double(tile) / se));
				}
				printf(" dB (postfx %s, blur %s)\n", df.postfx_available() ? "on" : "unavailable", df.postfx_blur_available() ? "on" : "unavailable");
			}
			// g2) v21: the v7.21 power changes on the Balanced RGBA path. Separate passes (v7.20) vs the blur and
			// sharpening folded into the pack: bit-identical. Fast pyramid: within a code or two of the exact one.
			std::vector<uint32_t> px_sep, px_mrg, px_fast;
			auto run3 = [&](bool merge, bool fast, std::vector<uint32_t> &out) {
				df.set_merge(merge);
				df.set_fast_pyramid(fast);
				bool okd = true;
				for (int i = 0; okd && i < 3; i++)
					okd = df.decode_into(frame.data(), frame.size(), ta, &tm, &err);
				return okd && df.readback_rgba(ta, out, &err);
			};
			auto cmp_px = [&](const std::vector<uint32_t> &a, const std::vector<uint32_t> &b, int md2[3], size_t *ndiff) {
				md2[0] = md2[1] = md2[2] = 0;
				*ndiff = 0;
				for (size_t i = 0; i < a.size() && i < b.size(); i++)
				{
					if (a[i] == b[i])
						continue;
					(*ndiff)++;
					md2[2] = std::max(md2[2], std::abs(int(a[i] & 1023) - int(b[i] & 1023)));
					md2[0] = std::max(md2[0], std::abs(int((a[i] >> 10) & 1023) - int((b[i] >> 10) & 1023)));
					md2[1] = std::max(md2[1], std::abs(int((a[i] >> 20) & 1023) - int((b[i] >> 20) & 1023)));
				}
			};
			auto row2 = [&](const char *name, const std::vector<uint32_t> &a, const std::vector<uint32_t> &b, int tol) {
				int m2[3];
				size_t nd;
				cmp_px(a, b, m2, &nd);
				bool ok = m2[0] <= tol && m2[1] == 0 && m2[2] == 0;
				printf("  %-44s %zu px differ, max Y %d Cb %d Cr %d -> %s\n", name, nd, m2[0], m2[1], m2[2], ok ? "MATCH" : "MISMATCH");
				fc_ok = fc_ok && ok;
			};
			if (!run3(false, false, px_sep) || !run3(true, false, px_mrg)) { fcfail("v7.21 merge check"); break; }
			row2("v7.21 merged pack vs separate passes (<= 1)", px_mrg, px_sep, 1);   // Turnip compiles the two differently: rare 1-code rounding flips
			{
				std::vector<uint32_t> px_rep;
				if (!df.decode_into(rep_frame.data(), rep_frame.size(), ta, &tm, &err) || !df.readback_rgba(ta, px_rep, &err)) { fcfail("merged repeat"); break; }
				row2("v7.21 merged repeat vs last picture (<= 1)", px_rep, px_mrg, 1);
			}
			if (!run3(true, true, px_fast)) { fcfail("v7.21 fast pyramid"); break; }
			row2("v7.21 fast pyramid vs exact (expect <= 3)", px_fast, px_mrg, 3);
			// g3) v21: read the shared image the way vrcompositor does (export, import with the reported layout)
			{
				std::vector<uint32_t> px_imp;
				if (df.rgba_export(ta, &ex, &err))
				{
					close(ex.fd);
					if (!df.readback_rgba(ta, px_imp, &err, true)) { fcfail("import check (LINEAR)"); break; }
					row2("shared image read through an import", px_imp, px_fast, 0);
				}
				else
					printf("  shared image import check: skipped (not exportable here)\n");
			}
			// g4) v21: per-pass GPU times, Balanced, 4:2:0 -> RGBA: v7.20 path vs v7.21
			auto passes = [&](const char *name, Decoder &d, int a, int b) {
				double sum[6] = {}, tot = 0;
				int n = 0;
				for (int i = 0; i < iters; i++)
				{
					if (!d.decode_into(frame.data(), frame.size(), (i & 1) ? b : a, &tm, &err)) { fcfail("pass timing"); return; }
					for (int k = 0; k < 6; k++)
						sum[k] += tm.pass_ms[k];
					tot += tm.gpu_decode_ms + tm.gpu_pack_copy_ms;
					n++;
				}
				printf("  %-30s decode %.3f | pyramid %.3f blur %.3f sharpen %.3f pack %.3f (seam %.3f other %.3f) | total %.3f ms\n",
				       name, tot / n - (sum[0] + sum[1] + sum[2] + sum[3] + sum[4] + sum[5]) / n, sum[2] / n, sum[3] / n,
				       sum[4] / n, sum[5] / n, sum[1] / n, sum[0] / n, tot / n);
			};
			df.set_target_output(tb, kOutRGBA);
			printf("  GPU ms per frame, Balanced, 4:2:0 -> RGBA, %d decodes:\n", iters);
			df.set_merge(false);
			df.set_fast_pyramid(false);
			passes("v7.20 (separate passes)", df, ta, tb);
			df.set_merge(true);
			passes("merged, exact pyramid", df, ta, tb);
			df.set_fast_pyramid(true);
			passes("v7.21 (merged, fast pyramid)", df, ta, tb);
			// g5) v21: compressed (UBWC) shared image, if the driver offers it: same picture, read directly and through
			// an import, and its pack time
			{
				Decoder::set_default_rgba_compressed(true);
				Decoder dz;
				bool zok = dz.init(W, H, surf_bits, &err, kModeFullColour);
				Decoder::set_default_rgba_compressed(false);
				TargetDesc za, zb;
				int zta = -1, ztb = -1;
				if (zok && dz.create_client_like_surface(modifier, &za, &err) >= 0 && dz.create_client_like_surface(modifier, &zb, &err) >= 0)
				{
					za.readable = zb.readable = true;
					zta = dz.import_target(za, &err);
					ztb = zta >= 0 ? dz.import_target(zb, &err) : -1;
				}
				if (zta < 0 || ztb < 0)
					printf("  compressed shared image: decoder / surfaces failed (%s)\n", err.c_str());
				else if (dz.rgba_modifier(zta) == 0)
					printf("  compressed shared image: not offered by this driver (stays LINEAR)\n");
				else
				{
					printf("  compressed shared image: modifier 0x%llx\n", (unsigned long long)dz.rgba_modifier(zta));
					const float r[16] = {0.2f, 15.0f, 55.0f, 0.2f, 0.58f, 0.40f, 0.32f, 0.58f, -0.33f, 0.40f,
					                     0.42f, 0.40f, 0.32f, 0.42f, -0.33f, 0.40f};
					dz.set_ramp(r);
					dz.set_dither(true);   // as df at this point
					dz.set_postfx_mode(0);
					dz.set_postfx_seam(0, true);
					dz.set_postfx2(0.0f, 0.0f, 0.5f, 0.35f, 0.8f, 2.0f);
					dz.set_postfx_blur(0.3f, 3.0f);
					dz.set_postfx_temporal(0.6f, 0.6f);
					dz.set_target_output(zta, kOutRGBA);
					dz.set_target_output(ztb, kOutRGBA);
					std::vector<uint32_t> pz, pzi;
					bool okd = true;
					for (int i = 0; okd && i < 3; i++)
						okd = dz.decode_into(frame.data(), frame.size(), zta, &tm, &err);
					if (!okd || !dz.readback_rgba(zta, pz, &err)) { fcfail("compressed decode / readback"); break; }
					// px_fast carries a trace of the earlier exact-pyramid frames in its stability history: within a code
					row2("compressed image vs LINEAR (<= 1, history)", pz, px_fast, 1);
					if (!dz.readback_rgba(zta, pzi, &err, true)) { fcfail("compressed import check"); break; }
					row2("compressed image: import vs direct read", pzi, pz, 0);
					passes("v7.21 + compressed image", dz, zta, ztb);
				}
			}
			// h) timings, two targets alternating, RGBA output
			df.set_target_output(tb, kOutRGBA);
			for (int pass = 0; pass < 2; pass++)
			{
				const std::vector<uint8_t> &fr = pass ? f444 : frame;
				std::vector<double> cpu, gd, fin, cp;
				for (int i = 0; i < iters; i++)
				{
					if (!df.decode_into(fr.data(), fr.size(), (i & 1) ? tb : ta, &tm, &err)) { fcfail("timing decode"); break; }
					cpu.push_back(tm.cpu_ms);
					gd.push_back(tm.gpu_decode_ms);
					fin.push_back(tm.gpu_finish_ms);
					cp.push_back(tm.gpu_copy_ms);
				}
				if (cpu.empty())
					break;
				printf("  timing %s -> RGBA, Balanced post-filter, %d decodes:\n", pass ? "4:4:4" : "4:2:0", iters);
				stat(cpu, "CPU push -> fence");
				stat(gd, "GPU Pyrowave decode");
				stat(fin, "GPU filters");
				stat(cp, "GPU pack (RGBA)");
			}
			df.set_postfx_blur(0.0f, 1.0f);
			df.set_postfx_temporal(0.0f, 1.0f);
			df.set_postfx2(0.0f, 0.0f, 0.0f, 0.35f, 0.8f, 2.0f);
		} while (false);
		printf("  full colour: %s\n", fc_ok ? "PASS" : "FAIL");
	}

	printf("\nSUMMARY (8-bit NV12 client surface, %d iterations each, dither on; Pyrowave-side dither %s)\n", iters,
	       dec.pyrowave_dither_available() ? "available" : "NOT available (old libpyrowave)");
	printf("  %-26s %-9s %-8s %-7s %8s %8s %8s %8s %8s %8s %9s %8s %8s\n", "mode", "picture", "repeat", "direct", "cpu avg",
	       "cpu p95", "gpu dec", "finish", "copies", "gpu sum", "dec nodit", "ms/f seq", "ms/f 2x");
	bool base_ok = false;
	for (auto &r : results)
	{
		if (r.mode == 0)
			base_ok = r.ok;
		if (!r.ran)
		{
			printf("  %-26s FAILED (see above)\n", mode_names[r.mode]);
			continue;
		}
		printf("  %-26s %-9s %-8s %-7s %8.3f %8.3f %8.3f %8.3f %8.3f %8.3f %9.3f %8.3f %8.3f\n", mode_names[r.mode],
		       r.ok && r.dmd <= 1 ? "MATCH" : "MISMATCH", r.rep_ok ? "MATCH" : "MISMATCH",
		       r.luma_direct ? "Y+C" : r.direct ? "C" : "no", r.cpu, r.cpu95, r.gd, r.fin, r.cp, r.gd + r.fin + r.cp,
		       r.gd_nodither, r.seq_ms, r.pipe_ms);
	}
	printf("RESULT: %s%s\n", base_ok || only_mode > 0 ? "PASS" : "MISMATCH", fc_ok ? "" : " (full colour FAILED)");
	printf("done.\n");
	return 0;
}
