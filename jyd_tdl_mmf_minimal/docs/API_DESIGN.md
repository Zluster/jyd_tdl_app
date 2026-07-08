# API Design

This baseline is centered on short `mmf_*` naming and a small number of clear modules.

Goals:

- standalone C API that can be extracted from `jyd_tdl_app`
- stable module boundaries before real backend work
- explicit ownership for frames, VENC/VDEC channels, display bindings, and audio sessions
- no dependency on old single-system sensor/profile/proc abstractions

## Modules

1. `system`
   Handles init, ready state, version, last error, and cross-module video settings.
2. `camera`
   Handles output listing, frame acquisition, release, snapshot, and ISP controls.
3. `audio`
   Handles PCM capture and playback sessions.
4. `audio_3a`
   Handles AEC, noise suppression, AGC, and AEC reference policy.
5. `audio_codec`
   Handles AENC and ADEC sessions.
6. `display`
   Handles display window setup, camera binding, pushed frames, image display, snapshot, and OSD clear.
7. `jpg`
   Handles JPEG encode and decode sessions.
8. `touch`
   Handles touchscreen input events from `/dev/touchscreen`.
9. `jpg_http`
   Handles JPEG-over-HTTP image serving.

## Camera Base

The camera model stays source-oriented:

- `main`
- `ai`
- `live`
- `subrgb`
- `screen`

The upper layer should care about:

- which source to read
- output size
- output format
- scale policy

The lower layer should hide:

- VI/VPSS details
- bind/unbind details
- sensor-loss rebuild logic; small core owns automatic rebuild and upper layers should not trigger it

Frame ownership is explicit:

- `mmf_camera_get_frame`
- `mmf_camera_put_frame`

This is required because direct VPSS frames need release back to the VB pool.

Camera control APIs are declared for AE, AWB, exposure, analog gain, ISP digital gain, white balance, flip, and mirror. The current skeleton does not implement them yet; the real backend needs small-core ISP wrappers or an equivalent big-core MPI path.

## Display Base

Display is independent and should not be hidden inside camera.

Current display APIs:

- `mmf_display_open`
- `mmf_display_set_window`
- `mmf_display_bind_camera`
- `mmf_display_unbind`
- `mmf_display_show_frame`
- `mmf_display_show_image_file`
- `mmf_display_snapshot`
- `mmf_display_clear`
- `mmf_display_clear_overlay`
- `mmf_display_get_status`

This keeps room for:

- direct VO binding to a camera source
- upper-layer frame push
- local file display
- OSD overlay display
- screen snapshot as JPEG

`mmf_display_snapshot` is intended to capture what the screen shows. The backend must verify whether the selected capture point includes RGN/OSD composition; if not, snapshot needs an OSD-aware path.

The current CV184x native backend snapshots selected camera/VPSS sources. OSD is handled through RGN, but whether `include_osd=1` exactly matches the final VO composition still needs board-side confirmation for every display path.

## JPG Base

JPEG is separate from display and HTTP:

- one-shot encode/decode
- persistent encoder for HTTP streaming
- persistent decoder for image display

The backend must reserve VENC/VDEC channels by role so snapshot, HTTP stream, and display decode do not collide.

## Audio Base

The audio model stays session-oriented:

- `mmf_audio_input_*`
- `mmf_audio_output_*`
- `mmf_audio_encoder_*`
- `mmf_audio_decoder_*`

3A remains attached to the input session, not only to a global switch.

AEC needs a playback reference. The API keeps this visible through:

- `provide_aec_reference` on playback
- `aec_reference_device` and `aec_reference_channel` on capture
- 3A status reporting whether a playback reference is available

## Touch Base

Touch only handles input. It does not own GUI logic.

Current proposed APIs:

- `mmf_touch_open`
- `mmf_touch_read_event`
- `mmf_touch_get_status`
- `mmf_touch_close`

Config keeps:

- input device path
- screen size
- coordinate swap/invert flags

## JPG HTTP Base

The image transport module is explicitly JPEG over HTTP.

Three modes are kept:

1. `CAMERA_PULL`
   The module pulls frames from a camera source and encodes/publishes them.
2. `DISPLAY_PULL`
   The module publishes the same source as the display snapshot path.
3. `PUSH`
   The upper layer pushes JPEG or raw frames into the module.

## Out of Scope For This Stage

This stage defines the base and a first real backend. Remaining complex parts:

- epoll-based HTTP server loop; the current backend uses a background listener and one client thread per connection
- real touchscreen event parser
- real dual-core IPC wiring
- real ISP control path

The main purpose is to lock down the public API surface first.
