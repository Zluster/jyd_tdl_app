This directory contains the active MMF implementation.

- `core/` contains shared private CV184x helpers: runtime init, error storage,
  format conversion, display-service IPC, frame-copy helpers, and codec channel
  resource arbitration.
- `camera/` implements camera C APIs and the CVI VPSS frame/display binding
  helper used by camera and display code.
- `display/` implements display C APIs and CVI RGN/OSD helpers.
- `jpeg/` implements JPEG encode/decode C APIs and CVI VENC/VDEC helpers.
- `http/` implements the JPEG-over-HTTP server.
- `audio/` implements PCM capture/playback, 3A state, AENC/ADEC, and CVI audio
  helpers.
- `touch/` implements the Linux `/dev/touchscreen` helper.

The public API remains C in `include/mmf`. Some backend files are still C++ as a
transition step because they own CVI resources with RAII and use `std::vector` /
`std::thread`; these should be converted module by module after this layout is
stable.
