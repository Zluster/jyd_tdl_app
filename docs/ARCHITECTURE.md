# Architecture

The project is split into two parallel stacks:

1. media stack
2. algorithm stack

They meet only at `Frame` / `FrameSource` / `FrameSink`, so camera, RTSP,
offline image input, and future transport modules do not leak into the
algorithm wrappers.

## Media Stack

Public media-facing classes:

- `SysContext`
- `VideoBufferManager`
- `VideoBufferPool`
- `MediaSystem`
- `ViChannel`
- `VpssGroup`
- `MediaLink`
- `FrameReader`
- `OsdRegion`
- `RegionOverlay`
- `VdecChannel`
- `VencChannel`
- `Mmf`
- `Camera`
- `SensorMedia`
- `RtspStreamer`

Responsibilities:

- `SysContext`: `SYS` lifecycle, chip/version query, `ION` alloc/flush/invalidate
- `VideoBufferManager`: common `VB` pool configuration lifecycle
- `VideoBufferPool`: dynamic `VB` pool / block allocate-map-release wrapper
- `MediaSystem`: `SYS` and optional `VB` lifecycle
- `ViChannel`: explicit `VI` channel enable/disable wrapper
- `VpssGroup`: explicit `VPSS` group/channel wrapper
- `MediaLink`: generic `SYS_Bind` / `SYS_UnBind`
- `FrameReader`: direct frame acquisition from `VI` or `VPSS`
- `OsdRegion`: primary `RGN/OSD` wrapper with canvas access
- `RegionOverlay`: `RGN/OSD` overlay attach and bitmap update
- `VdecChannel`: `VDEC` channel / stream / frame wrapper
- `VencChannel`: `VENC` channel / encoded packet wrapper
- `Mmf`: convenient bootstrap for `SYS + VB + VPSS + bind`
- `Camera`: thin high-level frame source wrapper over `FrameReader`
- `SensorMedia`: board-side sensor / MIPI / ISP / VI / VPSS bring-up helper
- `RtspStreamer`: encoded output transport

Private board-specific implementation lives under:

- `src/private/media/`

Current private scope:

- sensor ini parsing
- MIPI reset / clock / rx attr
- ISP startup and shutdown
- VI pipe / device setup
- VPSS setup for sensor-backed pipelines

The goal is that upper layers should usually stop at the public classes above.
Only true SoC bring-up details should remain under `private/media`.

## Algorithm Stack

The algorithm side is organized around four layers:

1. `FrameSource` for input abstraction
2. `NnBase` and algorithm subclasses for inference
3. `FrameSink` for output / display / streaming
4. `Pipeline` / `VisionPipeline` for composition and orchestration

Public header split:

- `pipeline.hpp`: primary application-facing pipeline API
- `simple_pipeline.hpp`: compatibility forwarding header

## Intended Subclasses

- `NnClassifier`
- `NnFeature`
- `NnYolov5`
- `NnYolov8`
- future task-specific wrappers such as face, pose, OCR, feature, etc.
- `ModelDescriptor` / `ModelSpec` parser for app-owned model metadata

Each subclass can own:

- model selection
- initialization hooks
- postprocess logic
- model-specific output mapping

## Why This Shape

This keeps application code stable while the vendor SDK details stay hidden.
It also makes it easier to add camera input, RTSP, OSD, or board-specific
media paths later without changing the public result types.

The media stack is intentionally layered so future features can be added
incrementally:

- custom MIPI / sensor setup
- multiple `VPSS` groups
- multiple outputs from one source
- manual `VI -> VPSS -> VENC` binding
- direct frame acquisition for debug
- `RGN/OSD` overlays before RTSP or save
- attach-existing mode when the board starts media elsewhere

The model descriptor layer is the only metadata contract the SDK consumes. It
should carry application metadata such as:

- `basic.type`
- `basic.model`
- `extra.runtime`
- `extra.task`
- `extra.model_type`
- `extra.input_type`
- `extra.preprocess`
- `extra.mean`
- `extra.scale`
- `extra.anchors`
- `extra.labels`
- `extra.normalize`

## GitHub Usage

If you publish the repo on GitHub, other developers can clone it and work on
the C++ wrapper without cloning Sophpi itself. To build for CV184X, they still
need:

- the ARM cross toolchain
- the CV184X vendor runtime bundle
- the model/config bundle

## Public API Names

Use these terms in the public headers:

- `MediaSystem`: basic `SYS/VB` lifecycle
- `SysContext`: low-level `SYS/ION` helper
- `VideoBufferManager`: common `VB` lifecycle
- `VideoBufferPool`: dynamic `VB` pool helper
- `ViChannel`: `VI` channel wrapper
- `VpssGroup`: `VPSS` group wrapper
- `MediaLink`: generic module bind wrapper
- `FrameReader`: low-level frame pull API
- `OsdRegion`: primary `RGN/OSD` wrapper
- `RegionOverlay`: `RGN/OSD` wrapper
- `VdecChannel`: `VDEC` wrapper
- `VencChannel`: `VENC` wrapper
- `Mmf`: convenience media bootstrap
- `Camera`: application-level capture wrapper
- `SensorMedia`: sensor-backed startup helper
- `FrameSource`: image file, camera, or future network source
- `NnBase`: common model wrapper base
- `NnClassifier`: classification family
- `NnFeature`: feature embedding family
- `NnYolov5`: YOLO-style detection family
- `NnYolov8`: YOLOv8-style detection family
- `FrameSink`: display, encode, file save, or RTSP publish
- `Pipeline`: primary application-facing source/model/sink wrapper
- `VisionPipeline`: wiring helper that joins source, model, and sink
- `ModelDescriptor` / `ModelSpec`: app-owned model metadata

Avoid tying upper-layer code directly to vendor module names or to one fixed
execution mode. The same pipeline should support:

- image + algorithm
- camera + algorithm
- camera + algorithm + display / RTSP
- image + algorithm + save

The same media base should also support:

- sensor full-stack startup
- attach to existing media graph
- manual bind/unbind experiments
- multi-group and multi-output expansion
- OSD overlay insertion before encode or stream
