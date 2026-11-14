# HEVC decoder probe (read-only)

Checks what the Frame's hardware video decoder can do, for the "HEVC 4:4:4 / 4:2:2" question:

- its formats, frame-size limits and controls (HEVC profiles and levels);
- whether it decodes these test clips correctly (12 frames each, a test pattern; the brightness is compared
  with the PC's own decode):
  - 4:2:0 8/10-bit 768x3072 (your stream size) and 1344x5376 (Steam Link's stock size): baseline;
  - 4:2:0 768x4096, 768x6144, 1536x3072, 1536x6144: the sizes a 4:2:2 or 4:4:4 picture packed into a
    normal 4:2:0 stream would need;
  - native 4:2:2 and 4:4:4, 8 and 10-bit (HEVC Range Extensions).

It only opens the decoder like a video player would, with its own session, and changes nothing. It refuses to
run while a Steam Link VR session is streaming (vrlink running) unless given `--anyway`.

Run (on the Frame, as the normal user): `python3 probe.py` from this folder. The report is printed and saved to
`/tmp/slp_hevc_probe/report.txt`. It contains no account, session, key or network details.

The clips were made with `ffmpeg -f lavfi -i testsrc2=s=WxH:r=144 -frames:v 12 -pix_fmt PF -c:v libx265 -preset
fast -crf 24 -x265-params bframes=0:keyint=6:min-keyint=6:repeat-headers=1:aud=1`; `refs.json` holds the 16x16
block means of the brightness of the first and last frame of each.
