# H.264 decoder probe (read-only)

The H.264 twin of `../hevc_probe`, for "full colour on an H.264 stream" (qaLink's H.264 mode). It reuses that
probe's V4L2 code (imported unchanged) and checks whether the Frame's decoder takes these H.264 clips, comparing the
decoded brightness with the PC's own decode:

- 4:2:0 8-bit 768x3072 and 960x3840: baseline;
- 768x4096, 768x6144, 960x7680: a colour picture packed below the normal one (as SLFC does for HEVC);
- 1152x3072, 1536x3072, 1920x3840: packed beside it (the PC's NVENC H.264 encoder stops at 4096 in either direction);
- 4:2:0 10-bit (High 10), native 4:2:2 and 4:4:4: for information.

It refuses to run while vrlink is running unless given `--anyway`. Run on the Frame: `python3 probe.py` from this
folder (needs `../hevc_probe/probe.py` next to it). The report is printed and saved to `/tmp/slp_h264_probe/report.txt`.
It contains no account, session, key or network details.

Clips: `ffmpeg -f lavfi -i testsrc2=s=WxH:r=144 -frames:v 12 -pix_fmt PF -c:v libx264 -preset fast -crf 24
-x264-params bframes=0:keyint=6:min-keyint=6:scenecut=0:repeat-headers=1:aud=1`; `refs.json` holds the 16x16 block
means of the raw luma of the first and last frame (decoded with `-fps_mode passthrough`), plus profile and level.
