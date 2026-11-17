#!/usr/bin/env python3
# Steam Link Pyrowave: read-only probe of the Frame's hardware H.264 decoder (2026-11-16).
#
# The H.264 twin of ../hevc_probe/probe.py, for "full colour on an H.264 stream" (qaLink's H.264 mode). It reuses
# that probe's V4L2 code unchanged and only swaps the coded format (H264), the access-unit splitter and the clips:
#   - 4:2:0 8-bit at 768x3072 and 960x3840: baseline (normal stream sizes);
#   - 768x4096, 768x6144, 960x7680: a 4:2:2 / 4:4:4 picture packed below the normal one (as SLFC does for HEVC);
#   - 1152x3072, 1536x3072, 1920x3840: the same packed beside it instead (the PC's NVENC H.264 encoder stops at
#     4096 pixels in either direction, so tall packing can't be encoded there; side by side can);
#   - 4:2:0 10-bit (High 10), native 4:2:2 and 4:4:4 (High 4:2:2 / High 4:4:4 Predictive): for information.
#
# Nothing is changed on the Frame: it opens the decoder like any video player would, from user space, with its own
# private decoder session, and closes it again. No files are written outside /tmp/slp_h264_probe. No account,
# session, key or network information is read or printed.
#
# Run it while NOT streaming (quit the Steam Link VR session first); it refuses to run while vrlink is running
# unless given --anyway.
import builtins, importlib.util, json, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = "/tmp/slp_h264_probe"

spec = importlib.util.spec_from_file_location("hevc_probe", os.path.join(HERE, "..", "hevc_probe", "probe.py"))
P = importlib.util.module_from_spec(spec)
spec.loader.exec_module(P)

H264 = P.fcc("H264")


def split_access_units(data):
    """Annex-B H.264 -> list of access units. The clips have an access unit delimiter (NAL 9) before every one."""
    starts, i, n = [], 0, len(data)
    while True:
        j = data.find(b"\x00\x00\x01", i)
        if j < 0: break
        s = j - 1 if j > 0 and data[j - 1] == 0 else j
        starts.append((s, j + 3)); i = j + 3
    aus, cur = [], None
    for s, p in starts:
        t = data[p] & 0x1f if p < n else 0
        if t == 9 and cur is not None:
            aus.append(data[cur:s]); cur = s
        elif cur is None:
            cur = s
    if cur is not None: aus.append(data[cur:])
    return aus


def _open(path, *a, **k):
    # the HEVC probe's DecodeTest reads clips/<name>.hevc; ours are .h264 in this folder
    if path.endswith(".hevc"):
        path = os.path.join(HERE, "clips", os.path.basename(path)[:-5] + ".h264")
    return builtins.open(path, *a, **k)


# point the shared code at H.264 (the HEVC probe's file itself is untouched)
P.HEVC = H264                    # coded format used by find_decoder() and DecodeTest.run()
P.split_access_units = split_access_units
P.open = _open
P.OUT_DIR = OUT_DIR


def main():
    if P.vrlink_running() and "--anyway" not in sys.argv:
        print("vrlink (the Steam Link VR client) is running. Quit the streaming session first, then run this again\n"
              "(or add --anyway to run it next to vrlink; it would only open its own decoder session).")
        return 2
    os.makedirs(OUT_DIR, exist_ok=True)
    log = P.log
    log("Steam Link Pyrowave H.264 decoder probe, %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
    try: log("kernel %s" % os.uname().release)
    except Exception: pass
    dec = P.find_decoder()
    if not dec:
        log("\nNo V4L2 node with an H.264 decoder found."); return 1
    P.describe_decoder(dec)
    refs = json.load(builtins.open(os.path.join(HERE, "refs.json")))
    log("\n== Decode tests (12 frames each, I-frame every 6, 144 fps)")
    order = lambda c: (c.split("_")[0], int(c.split("_")[1]), int(c.split("x")[-1]), c)
    for clip in sorted(refs, key=order):
        t = P.DecodeTest(dec, clip, refs[clip])
        t0 = time.monotonic()
        try: t.run()
        except Exception as e: t.result["verdict"] = "FAILED: %s: %s" % (type(e).__name__, e)
        finally: t.close()
        r = refs[clip]
        log("  %s (%s %d-bit %ux%u, %s level %s): %s  [%.1f s]" % (
            clip, r["chroma"], r["bits"], r["w"], r["h"], r["profile"], r["level"],
            t.result.get("verdict", "?"), time.monotonic() - t0))
        for k in ("decoder picks", "capture format used", "frames decoded", "access units"):
            if k in t.result: log("      %s: %s" % (k, t.result[k]))
        for c in t.result.get("checks", []): log("      " + c)
    with builtins.open(os.path.join(OUT_DIR, "report.txt"), "w") as f:
        f.write("\n".join(P.lines) + "\n")
    log("\nReport written to %s/report.txt" % OUT_DIR)
    return 0


if __name__ == "__main__":
    sys.exit(main())
