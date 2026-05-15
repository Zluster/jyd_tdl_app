# MMF

This document describes how the project uses `Mmf` as a graph bootstrap layer
over the lower-level media wrappers.

## Position In The Stack

`Mmf` is not a replacement for every media wrapper.
It is a composition helper on top of:

- `MediaSystem`
- `VpssGroup`
- `VencChannel`
- `VdecChannel`
- `VoOutput`
- `OsdRegion`
- `MediaLink`

Implementation lives under:

- `src/media/mmf.cpp`

## What `Mmf` Should Own

`Mmf` is responsible for:

- opening `SYS/VB` through `MediaSystem`
- opening graph-owned nodes such as `VPSS/VENC/VDEC/VO/OSD`
- applying bind relationships with `MediaLink`
- closing them in reverse order

`Mmf` is not responsible for:

- sensor / MIPI / ISP bring-up
- algorithm inference
- RTSP packet transport
- image file based model validation

Sensor-backed bring-up remains in `SensorMedia`.

## Current Graph Nodes

`Mmf::GraphConfig` currently supports:

- `vpss`
- `venc`
- `vdec`
- `vo`
- `osd`
- `osd_attaches`
- `binds`

## Current Supported Bind Model

Graph binds are represented as generic source/destination channels:

```cpp
tdl_app::Mmf::BindConfig b = tdl_app::Mmf::bind(
    tdl_app::MediaChannel::vi(0, 0),
    tdl_app::MediaChannel::vpss(0, 0));
```

This is intentionally generic so the same config form can express:

- `VI -> VPSS`
- `VPSS -> VENC`
- `VDEC -> VPSS`
- `VPSS -> VO`

OSD attachment is represented separately from `binds`, because it is not a
`SYS_Bind` path. It now has an explicit graph entry:

```cpp
graph.osd.push_back(tdl_app::OsdRegion::canvas(0, 320, 80));
graph.osd_attaches.push_back(
    tdl_app::Mmf::osdAttach(0, tdl_app::MediaChannel::venc(0), 16, 16, 0));
```

## Convenience Builders

### Single `VPSS`

```cpp
tdl_app::Mmf mmf(tdl_app::Mmf::singleVpss(
    0, 0,
    1920, 1080,
    640, 640,
    18,
    true, 0, 0));
```

### `VI -> multi-VPSS`

```cpp
std::vector<tdl_app::Mmf::VpssConfig> outputs;
outputs.push_back(tdl_app::Mmf::vpssOutput(0, 0, 1920, 1080, 640, 640, 18));
outputs.push_back(tdl_app::Mmf::vpssOutput(1, 0, 1920, 1080, 960, 540, 18));

tdl_app::Mmf mmf(tdl_app::Mmf::viToMultiVpss(outputs, 0, 0));
```

### Generic graph

```cpp
tdl_app::Mmf::GraphConfig graph;
graph.vpss.push_back(tdl_app::Mmf::vpssOutput(0, 0, 1920, 1080, 640, 640, 18));
graph.venc.push_back(tdl_app::VencChannel::h264(0, 640, 640, 2048, 25, 25, 25));
graph.binds.push_back(tdl_app::Mmf::bind(
    tdl_app::MediaChannel::vpss(0, 0),
    tdl_app::MediaChannel::venc(0)));

tdl_app::Mmf mmf(tdl_app::Mmf::mediaGraph(graph));
mmf.open(&error);
```

## Recommended Usage Split

- use `SensorMedia` when you need sensor/MIPI/ISP/VI bring-up
- use `Mmf` when the board graph starts from already-available modules or when
  you want to assemble `VPSS/VENC/VDEC/VO/OSD/bind` from clean C++ config
- use raw wrappers directly when you need step-by-step experiments or partial control

## Current Limitations

`Mmf` does not yet own every media workflow. In particular:

- complex multi-layer `VO` tuning is still left to `VoOutput`
- custom `VB` pool topologies beyond one common pool still need direct wrappers
- sensor-backed full graph startup still belongs to `SensorMedia`

## Extension Direction

Good next incremental `Mmf` additions are:

- graph-owned OSD bitmap update helpers or canvas fill helpers
- multiple common `VB` pools through `VideoBufferManager`
- optional `VO` channel rectangles and z-order composition
- explicit `VDEC -> VPSS -> VO` helper builders
- explicit `VI -> VPSS -> VENC` helper builders
