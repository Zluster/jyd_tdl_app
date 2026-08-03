# Current State Snapshot

- source repo: `/home/jyd/zwz/sophpi/jyd_tdl_app`
- branch: `dual_benchmark`
- commit: `7aad3908b0d2703d496335c2f9f967c5ac294914`

Current baseline:

- include prefix: `include/mmf`
- symbol prefix: `mmf_*`
- modules:
  - `system`
  - `camera`
  - `audio`
  - `audio_3a`
  - `audio_codec`
  - `display`
  - `jpg`
  - `touch`
  - `jpg_http`

Standalone state:

- `CMakeLists.txt` exists inside this directory
- public headers are under `include/mmf`
- native CV184x backend sources are under module directories plus `src/core`
- examples are under `examples`
- public headers and backend sources do not include `tdl_app/*` wrappers
- the minimal library links CV184x SDK libraries through `` supplied by the parent build or by an extracted SDK build

Implementation state:

- camera frame query/get/put, VPSS scale mode, ISP setters/query IPC, display bind/OSD, JPEG encode/decode, HTTP JPEG stream, PCM audio, audio 3A control, and basic AENC/ADEC are implemented through the CV184x native backend
- touch remains Linux `/dev/touchscreen` based and can be tested independently
- remaining work is mostly resource arbitration, deeper OSD snapshot guarantees, and long-run stress hardening

Extraction notes:

- keep `include/mmf`, `src/core`, `src/camera`, `src/display`, `src/jpeg`, `src/http`, `src/audio`, `src/system`, `src/touch`, `examples`, and `docs` together
- provide CV184x include/library variables equivalent to `` and `` in the extracted build
- carry `third_party/vendor/ini/ini.c` or replace it with an equivalent `ini_parse` provider required by vendor sensor libraries
