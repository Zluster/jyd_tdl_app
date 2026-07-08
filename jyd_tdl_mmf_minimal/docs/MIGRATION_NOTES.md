# Migration Notes

This migration has two main changes:

1. Naming changes from `jyd_mmf_*` to `mmf_*`
2. Module coverage expands from `system/camera/audio` to `system/camera/audio/display/touch/jpg_http`

## Naming Shift

Old names:

- `jyd_mmf_common.h`
- `jyd_mmf_system.h`
- `jyd_mmf_camera.h`
- `jyd_mmf_audio.h`
- `jyd_mmf_*`

New names:

- `mmf_common.h`
- `mmf_system.h`
- `mmf_camera.h`
- `mmf_audio.h`
- `mmf_display.h`
- `mmf_touch.h`
- `mmf_jpg_http.h`
- `mmf_*`

The goal is simple:

- remove project-private prefix noise
- keep one short and uniform module prefix
- make later extraction or reuse easier

## Old Capability To New Module

camera:

- old: `CameraSource / FrameSource / Camera`
- new: `mmf_camera_*`

audio:

- old: `Audio / AudioInput / AudioOutput / AudioEncoder / AudioDecoder`
- new: `mmf_audio_input_* / output_* / encoder_* / decoder_*`

system:

- old: implicit IPC setup and scattered connect behavior
- new: `mmf_system_*`

display:

- old: spread across camera demo paths and VO-related helpers
- new: `mmf_display_*`

touch:

- old: no unified public surface
- new: `mmf_touch_*`

jpg_http:

- old: no unified public surface
- new: `mmf_jpg_http_*`

## Suggested Implementation Order

Step 1:

- `mmf_system`
- `mmf_camera`

Step 2:

- `mmf_audio`

Step 3:

- `mmf_display`
- `mmf_touch`
- `mmf_jpg_http`

That order keeps the base stable because display, touch, and image transport all sit on top of the media foundation.
