// Steam Link Pyrowave - live status for the settings app (PC).
// SPDX-License-Identifier: MIT
//
// The backend writes %LOCALAPPDATA%\SteamLinkPyrowave\status.json about once a second while a stream runs:
//   {"t": <unix seconds>, "streaming": 1, "pyrowave": 1, "fps": 144.0, "mbps": 311.8, "encode_ms": 1.12, "wait_ms": 0.2,
//    "width": 768, "gaze_calibrated": 1, "gaze_resid_px": 3.1, "vrlink_target_mbps": 350}
// The app treats a file older than 3 s as "not streaming". Values the PC cannot know (Frame decode time,
// network time) are not in it.
#pragma once

// One encoded Pyrowave frame (worker thread): submit -> bytes, and the part of it spent waiting for vrlink's own
// D3D11 work (atlas rendering, queued behind the game on the GPU). Pyrowave's own time = total - wait.
void slp_status_frame(double encode_ms, double wait_ms, size_t bytes);
// Any other value ("width", "gaze_calibrated", ...). Cheap; written with the next frame's flush.
void slp_status_set(const char *key, double value);
