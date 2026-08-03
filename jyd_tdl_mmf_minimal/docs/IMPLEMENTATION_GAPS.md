# Implementation Gaps

This file tracks the remaining gaps after the native CV184x backend split. The baseline is no longer a placeholder-only wrapper.

## Camera

- Harden `mmf_camera_put_frame` so every source releases through the exact owning VPSS/VDEC path instead of relying on coarse session cleanup.
- Continue validating ISP getter/setter coverage against the small-core display-service IPC.
- Protect runtime VPSS scale changes with a cross-process resource policy if multiple apps can change scale simultaneously.

## Display

- Verify whether screen snapshot includes every RGN/OSD layer in the same way the panel shows it.
- Keep `show_frame` paths limited to formats the VO path really supports; add explicit conversion only if required by product use.
- Add long-run bind/unbind/display-clear stress tests with HTTP streaming active.

## JPG

- Keep the VENC/VDEC channel allocator centralized so snapshot, HTTP stream, and decode/display cannot collide.
- Validate repeated JPEG decode/release under stress; previous board logs showed stale VDEC release failures when resources were churned too quickly.

## Audio

- Promote 3A from global-ish helper state to a fuller capture/playback session policy if ASR and playback coexist for long periods.
- Add long-run full-duplex tests for 1ch/2ch capture, loopback, and AENC/ADEC with changing volume and 3A parameters.

## Touch

- Parse `/dev/touchscreen` events.
- Normalize coordinates using screen size, swap, and invert options.

## JPG HTTP

- Replace the basic threaded server with an epoll loop if many concurrent clients are required.
- Publish display-matching JPEG frames in `DISPLAY_PULL` mode.
- Reuse or reserve JPEG encoder resources to avoid conflicts with snapshot.
