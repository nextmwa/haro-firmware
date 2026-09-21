#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// OV2640 camera capture + JPEG upload only -- face DETECTION itself runs
// server-side (haro-server's face_tracking.py), not on this device. Tried
// on-device detection twice: the default 2-stage MSRMNP model missed a
// clearly visible, well-lit, centered face for minutes at a time (its
// region-proposal first stage is a recall bottleneck), and the more
// accurate single-stage ESPDet model crashed the board with ESP_ERR_NO_MEM
// alongside the rest of its workload (audio pipeline, wake_word's AFE,
// WiFi). The server has neither problem.
esp_err_t camera_face_track_init(void);

// Returns true and fills *dx/*dy with the last face position the server
// reported (see camera_face_track_set_remote_position() below), normalized
// to [-1, 1] (0 = frame center), if one arrived within the last
// CAMERA_FACE_TRACK_STALE_MS. Returns false (treat as "no face") once that
// window has elapsed, e.g. the person stepped out of frame or the server
// connection dropped. Cheap: just an atomic read, safe to poll every loop
// tick from main.c.
bool camera_face_track_get_offset(float *dx, float *dy);

// Called by main.c when a PROTOCOL_EVENT_FACE_POSITION arrives from the
// server (see protocol.h) -- feeds the same atomic state
// camera_face_track_get_offset() reads. `found=false` does NOT clear the
// last known position immediately: camera_face_track_get_offset()'s own
// staleness window handles that, the same way a locally-detected face
// going briefly out of frame used to be handled, so a single missed/no-face
// reply doesn't cause a visible snap back to center.
void camera_face_track_set_remote_position(bool found, float dx, float dy);

#ifdef __cplusplus
}
#endif
