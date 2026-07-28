// Python bindings (nanobind) for the CV184X dual-OS big-core media path:
//   - VpssCamera: attach to a small-core VPSS channel and fetch frames with
//     zero-copy access (CVI_SYS_Mmap of the VB buffer, exposed as memoryview
//     plus raw addr/size for ctypes users).
//   - Osd: RGN overlay region attached to a VPSS channel; the double-buffered
//     canvas is exposed as a writable memoryview plus raw addr/size.
//
// Frame lifecycle contract (agreed): one outstanding frame per camera. The
// mapped view becomes invalid as soon as the frame is released (explicitly,
// via `with`, or implicitly by the next read()); accessing a stale view is
// undefined behaviour and is intentionally not guarded.

#include <nanobind/nanobind.h>
#include <nanobind/stl/shared_ptr.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#include "cvi_comm_video.h"
#include "cvi_sys.h"
#include "cvi_vo.h"
#include "tdl_app/camera.hpp"
#include "tdl_app/layout.hpp"
#include "tdl_app/media_link.hpp"
#include "tdl_app/media_types.hpp"
#include "tdl_app/osd_region.hpp"
#include "tdl_app/sys_context.hpp"
#include "tdl_app/vo_output.hpp"

namespace nb = nanobind;

namespace {

[[noreturn]] void raise(const std::string &message) {
  throw std::runtime_error(message);
}

nb::object memoryviewFrom(void *data, std::size_t size, bool writable) {
  PyObject *view = PyMemoryView_FromMemory(
      static_cast<char *>(data), static_cast<Py_ssize_t>(size),
      writable ? PyBUF_WRITE : PyBUF_READ);
  if (!view) {
    throw nb::python_error();
  }
  return nb::steal(view);
}

class PyVpssCamera;

// A single captured frame: metadata copied out of VIDEO_FRAME_INFO_S plus the
// CVI_SYS_Mmap'ed view over all planes (planes are contiguous in the VB block,
// as relied upon by camera_demo_support::frameToBgrMat).
class PyFrame {
 public:
  PyFrame(PyVpssCamera *camera, const VIDEO_FRAME_INFO_S &info,
          unsigned char *mapped, std::size_t map_size)
      : camera_(camera), mapped_(mapped), map_size_(map_size) {
    const auto &vf = info.stVFrame;
    width_ = static_cast<int>(vf.u32Width);
    height_ = static_cast<int>(vf.u32Height);
    format_ = static_cast<int>(vf.enPixelFormat);
    sequence_ = vf.u32TimeRef;
    timestamp_us_ = vf.u64PTS;
    phys_addr_ = vf.u64PhyAddr[0];
    std::size_t offset = 0;
    for (int i = 0; i < 3; ++i) {
      strides_[i] = static_cast<std::size_t>(vf.u32Stride[i]);
      plane_sizes_[i] = static_cast<std::size_t>(vf.u32Length[i]);
      plane_offsets_[i] = offset;
      offset += plane_sizes_[i];
      if (plane_sizes_[i] > 0) {
        plane_count_ = i + 1;
      }
    }
  }

  ~PyFrame() { unmap(); }

  PyFrame(const PyFrame &) = delete;
  PyFrame &operator=(const PyFrame &) = delete;

  bool valid() const { return mapped_ != nullptr; }

  void requireValid() const {
    if (!mapped_) {
      raise("frame has been released; its buffer is no longer mapped");
    }
  }

  // Drops the mapping and returns the VPSS buffer to the pool (through the
  // owning camera, if it still holds this frame as current).
  void release();

  // Called by the camera when the underlying VPSS buffer goes away (next
  // read() or camera close). Only drops the mapping.
  void invalidate() { unmap(); }

  void detachCamera() { camera_ = nullptr; }

  std::uintptr_t addr() const {
    requireValid();
    return reinterpret_cast<std::uintptr_t>(mapped_);
  }

  std::size_t size() const { return map_size_; }

  nb::object data() {
    requireValid();
    return memoryviewFrom(mapped_, map_size_, false);
  }

  nb::object plane(int index) {
    requireValid();
    if (index < 0 || index >= plane_count_) {
      raise("plane index out of range, frame has " +
            std::to_string(plane_count_) + " plane(s)");
    }
    return memoryviewFrom(mapped_ + plane_offsets_[index],
                          plane_sizes_[index], false);
  }

  int width_ = 0;
  int height_ = 0;
  int format_ = 0;
  std::uint64_t sequence_ = 0;
  std::uint64_t timestamp_us_ = 0;
  std::uint64_t phys_addr_ = 0;
  int plane_count_ = 0;
  std::size_t strides_[3] = {0, 0, 0};
  std::size_t plane_sizes_[3] = {0, 0, 0};
  std::size_t plane_offsets_[3] = {0, 0, 0};

 private:
  void unmap() {
    if (mapped_) {
      CVI_SYS_Munmap(mapped_, static_cast<CVI_U32>(map_size_));
      mapped_ = nullptr;
    }
  }

  PyVpssCamera *camera_ = nullptr;
  unsigned char *mapped_ = nullptr;
  std::size_t map_size_ = 0;
};

// Wraps tdl_app::Camera on a VPSS group/channel owned by the small core.
class PyVpssCamera {
 public:
  PyVpssCamera(int group, int channel, int timeout_ms)
      : group_(group), channel_(channel),
        camera_(tdl_app::Camera::vpss(group, channel, 0, 0,
                                      tdl_app::PixelFormat::NV12, timeout_ms)) {}

  ~PyVpssCamera() { close(); }

  PyVpssCamera(const PyVpssCamera &) = delete;
  PyVpssCamera &operator=(const PyVpssCamera &) = delete;

  void open() {
    if (opened_) {
      return;
    }
    std::string error;
    if (!camera_.open(&error)) {
      raise("camera open failed (grp" + std::to_string(group_) + "/ch" +
            std::to_string(channel_) + "): " + error);
    }
    opened_ = true;
  }

  std::shared_ptr<PyFrame> read() {
    if (!opened_) {
      open();
    }
    // The next CVI_VPSS_GetChnFrame recycles the previous buffer, so any
    // still-alive Python frame object must lose its mapping first.
    dropCurrent(false);

    tdl_app::Frame frame;
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = camera_.read(&frame, &error);
    }
    if (!ok) {
      raise("camera read failed (grp" + std::to_string(group_) + "/ch" +
            std::to_string(channel_) + "): " + error);
    }

    auto *video = static_cast<VIDEO_FRAME_INFO_S *>(frame.native);
    const auto &vf = video->stVFrame;
    std::size_t map_size = 0;
    for (int i = 0; i < 3; ++i) {
      map_size += vf.u32Length[i];
    }
    if (map_size == 0) {
      camera_.releaseFrame();
      raise("frame buffer length is zero");
    }

    auto *mapped = static_cast<unsigned char *>(
        CVI_SYS_Mmap(vf.u64PhyAddr[0], static_cast<CVI_U32>(map_size)));
    if (!mapped) {
      camera_.releaseFrame();
      raise("CVI_SYS_Mmap failed");
    }
    CVI_SYS_IonInvalidateCache(vf.u64PhyAddr[0], mapped,
                               static_cast<CVI_U32>(map_size));

    auto result = std::make_shared<PyFrame>(this, *video, mapped, map_size);
    current_ = result;
    return result;
  }

  // Called from PyFrame::release: only the current frame owns a VPSS buffer.
  void releaseIfCurrent(PyFrame *frame) {
    auto current = current_.lock();
    if (current && current.get() == frame) {
      camera_.releaseFrame();
      current_.reset();
    }
  }

  void close() {
    dropCurrent(true);
    if (opened_) {
      camera_.close();
      opened_ = false;
    }
  }

  int group() const { return group_; }
  int channel() const { return channel_; }

 private:
  void dropCurrent(bool detach) {
    if (auto current = current_.lock()) {
      current->invalidate();
      if (detach) {
        current->detachCamera();
      }
    }
    current_.reset();
  }

  int group_ = 0;
  int channel_ = 0;
  bool opened_ = false;
  tdl_app::Camera camera_;
  std::weak_ptr<PyFrame> current_;
};

void PyFrame::release() {
  if (camera_) {
    camera_->releaseIfCurrent(this);
  }
  unmap();
}

// Snapshot of the RGN back-buffer returned by CVI_RGN_GetCanvasInfo. Valid
// until the matching Osd.update(); fetch a fresh one for every draw cycle.
struct PyOsdCanvas {
  std::uintptr_t addr = 0;
  std::size_t size = 0;
  int width = 0;
  int height = 0;
  int stride = 0;
  int format = 0;

  nb::object data() const {
    return memoryviewFrom(reinterpret_cast<void *>(addr), size, true);
  }
};

class PyOsd {
 public:
  PyOsd(int handle, int width, int height, int pixel_format, int canvas_count,
        std::uint32_t bg_color)
      : region_(tdl_app::OsdRegion::canvas(handle, width, height, pixel_format,
                                           canvas_count, bg_color)) {}

  ~PyOsd() { unmapPersistent(); }  // OsdRegion dtor detaches and destroys.

  PyOsd(const PyOsd &) = delete;
  PyOsd &operator=(const PyOsd &) = delete;

  void create() {
    std::string error;
    if (!tdl_app::ensureMmfRuntimeInitialized(&error)) {
      raise("MMF runtime init failed: " + error);
    }
    if (!region_.create(&error)) {
      raise("osd create failed: " + error);
    }
  }

  void attach(int group, int channel, int x, int y, int layer) {
    std::string error;
    if (!region_.attach(tdl_app::MediaChannel::vpss(group, channel), x, y,
                        layer, &error)) {
      raise("osd attach failed: " + error);
    }
  }

  PyOsdCanvas canvas() {
    tdl_app::OsdCanvas raw;
    std::string error;
    if (!region_.getCanvas(&raw, &error)) {
      raise("osd get canvas failed: " + error);
    }
    canvas_armed_ = true;
    if (!raw.data) {
      raise("osd canvas has no virtual address");
    }
    PyOsdCanvas out;
    out.addr = reinterpret_cast<std::uintptr_t>(raw.data);
    out.width = raw.width;
    out.height = raw.height;
    out.stride = raw.stride;
    out.format = raw.pixel_format;
    out.size = static_cast<std::size_t>(raw.stride) *
               static_cast<std::size_t>(raw.height);
    return out;
  }

  // Resident-canvas mode: map the canvas ION buffer(s) once with mappings we
  // own. Unlike canvas(), the returned views stay valid across update()
  // calls, so they can back a persistent framebuffer (e.g. LVGL renders into
  // them directly). Requires canvas_count=1 (single physical buffer).
  PyOsdCanvas persistentCanvas() {
    if (persistent_count_ > 0) {
      return persistent_view_[0];
    }
    mapNextCanvas();  // leaves the canvas armed for the first update()
    return persistent_view_[0];
  }

  // Resident double buffering (requires canvas_count=2). Maps both ping-pong
  // buffers and returns (back, front): `back` is the buffer the next render
  // must go to first. Pass them to LVGL as (buf1, buf2) with render mode
  // DIRECT; LVGL's buffer alternation then stays phase-locked with the RGN
  // swap performed by update(). Pushes one cleared frame during setup.
  nb::tuple persistentPair() {
    if (persistent_count_ >= 2) {
      return nb::make_tuple(persistent_view_[1], persistent_view_[0]);
    }
    if (persistent_count_ == 0) {
      mapNextCanvas();  // buffer A (current back), armed
    }
    std::string error;
    if (!region_.updateCanvas(&error)) {  // swap: A visible (cleared), back=B
      raise("osd update failed: " + error);
    }
    canvas_armed_ = false;
    mapNextCanvas();  // buffer B, armed for the first real update()
    if (persistent_phys_[1] == persistent_phys_[0]) {
      raise("osd returned the same canvas twice; persistent_pair requires "
            "canvas_count=2");
    }
    // (back, front) = (B, A)
    return nb::make_tuple(persistent_view_[1], persistent_view_[0]);
  }

  void update() {
    std::string error;
    if (persistent_count_ > 0 && !canvas_armed_) {
      // GetCanvasInfo/UpdateCanvas must strictly alternate. In persistent
      // mode the user never calls canvas() again, so re-arm here, and make
      // sure the hardware still uses one of the buffers we mapped.
      tdl_app::OsdCanvas raw;
      if (!region_.getCanvas(&raw, &error)) {
        raise("osd get canvas failed: " + error);
      }
      bool known = false;
      for (int i = 0; i < persistent_count_; ++i) {
        known = known || (raw.phys == persistent_phys_[i]);
      }
      if (!known) {
        raise("osd canvas physical address changed; persistent canvases "
              "require a fixed canvas ring (canvas_count must match)");
      }
    }
    if (!region_.updateCanvas(&error)) {
      raise("osd update failed: " + error);
    }
    canvas_armed_ = false;
  }

  void setVisible(bool visible) {
    std::string error;
    if (!region_.setVisible(visible, &error)) {
      raise("osd set_visible failed: " + error);
    }
  }

  void moveTo(int x, int y) {
    std::string error;
    if (!region_.moveTo(x, y, &error)) {
      raise("osd move_to failed: " + error);
    }
  }

  void detach() { region_.detach(); }

  void destroy() {
    unmapPersistent();
    region_.detach();
    region_.destroy();
  }

  bool isCreated() const { return region_.isCreated(); }
  bool isAttached() const { return region_.isAttached(); }
  int handle() const { return region_.handle(); }

 private:
  // GetCanvasInfo the current back buffer, map it persistently into the next
  // free slot and clear it to transparent. Leaves the canvas armed.
  void mapNextCanvas() {
    if (persistent_count_ >= 2) {
      raise("all persistent canvas slots are already mapped");
    }

    tdl_app::OsdCanvas raw;
    std::string error;
    if (!region_.getCanvas(&raw, &error)) {
      raise("osd get canvas failed: " + error);
    }
    canvas_armed_ = true;
    if (raw.phys == 0) {
      raise("osd canvas has no physical address");
    }

    const std::size_t size = static_cast<std::size_t>(raw.stride) *
                             static_cast<std::size_t>(raw.height);
    void *va = CVI_SYS_Mmap(raw.phys, static_cast<CVI_U32>(size));
    if (va == nullptr) {
      raise("CVI_SYS_Mmap of osd canvas failed");
    }
    std::memset(va, 0, size);  // transparent, avoids one frame of garbage

    const int slot = persistent_count_;
    persistent_va_[slot] = va;
    persistent_size_[slot] = size;
    persistent_phys_[slot] = raw.phys;

    persistent_view_[slot].addr = reinterpret_cast<std::uintptr_t>(va);
    persistent_view_[slot].size = size;
    persistent_view_[slot].width = raw.width;
    persistent_view_[slot].height = raw.height;
    persistent_view_[slot].stride = raw.stride;
    persistent_view_[slot].format = raw.pixel_format;
    persistent_count_ = slot + 1;
  }

  void unmapPersistent() {
    for (int i = 0; i < persistent_count_; ++i) {
      CVI_SYS_Munmap(persistent_va_[i],
                     static_cast<CVI_U32>(persistent_size_[i]));
      persistent_va_[i] = nullptr;
      persistent_size_[i] = 0;
      persistent_phys_[i] = 0;
    }
    persistent_count_ = 0;
  }

  tdl_app::OsdRegion region_;
  void *persistent_va_[2] = {nullptr, nullptr};
  std::size_t persistent_size_[2] = {0, 0};
  std::uint64_t persistent_phys_[2] = {0, 0};
  int persistent_count_ = 0;
  bool canvas_armed_ = false;  // GetCanvasInfo issued, UpdateCanvas pending
  PyOsdCanvas persistent_view_[2];
};

// MIPI screen output. Opening it lights up the panel; the video reaching the
// screen still needs the two MediaLink binds (live -> grp1/ch0 -> VO).
class PyVoOutput {
 public:
  PyVoOutput(int device, int layer, int channel, int width, int height,
             int pixel_format, int interface_type, int interface_sync,
             int rotation) {
    tdl_app::VoOutput::Config config;
    config.device = device;
    config.layer = layer;
    config.channel = channel;
    config.width = width;
    config.height = height;
    config.pixel_format = pixel_format;
    config.interface_type = interface_type;
    config.interface_sync = interface_sync;
    config.rotation = rotation;
    vo_ = std::make_unique<tdl_app::VoOutput>(config);
  }

  void open() {
    std::string error;
    if (!tdl_app::ensureMmfRuntimeInitialized(&error)) {
      raise("MMF runtime init failed: " + error);
    }
    if (!vo_->open(&error)) {
      raise("vo open failed: " + error);
    }
  }

  void close() { vo_->close(); }
  bool isOpen() const { return vo_->isOpen(); }

 private:
  std::unique_ptr<tdl_app::VoOutput> vo_;
};

MMF_CHN_S makeMmfChannel(MOD_ID_E module, int device, int channel) {
  MMF_CHN_S out;
  std::memset(&out, 0, sizeof(out));
  out.enModId = module;
  out.s32DevId = device;
  out.s32ChnId = channel;
  return out;
}

const char *modName(MOD_ID_E id) {
  switch (id) {
    case CVI_ID_VI: return "vi";
    case CVI_ID_VPSS: return "vpss";
    case CVI_ID_VENC: return "venc";
    case CVI_ID_VDEC: return "vdec";
    case CVI_ID_VO: return "vo";
    case CVI_ID_RGN: return "rgn";
    default: return "unknown";
  }
}

// Returns (module_name, device, channel) of the bind source feeding `dest`,
// or None when nothing is bound to it.
nb::object getBindSource(const MMF_CHN_S &dest) {
  std::string error;
  if (!tdl_app::ensureMmfRuntimeInitialized(&error)) {
    raise("MMF runtime init failed: " + error);
  }
  MMF_CHN_S source;
  std::memset(&source, 0, sizeof(source));
  if (CVI_SYS_GetBindbyDest(&dest, &source) != CVI_SUCCESS) {
    return nb::none();
  }
  return nb::make_tuple(modName(source.enModId),
                        static_cast<int>(source.s32DevId),
                        static_cast<int>(source.s32ChnId));
}

// CVI_SYS_Bind between two media channels (VPSS->VPSS, VPSS->VO, ...).
class PyMediaLink {
 public:
  PyMediaLink(const tdl_app::MediaChannel &source,
              const tdl_app::MediaChannel &destination) {
    tdl_app::MediaLink::Config config;
    config.source = source;
    config.destination = destination;
    link_ = std::make_unique<tdl_app::MediaLink>(config);
  }

  void bind() {
    std::string error;
    if (!tdl_app::ensureMmfRuntimeInitialized(&error)) {
      raise("MMF runtime init failed: " + error);
    }
    if (!link_->bind(&error)) {
      raise("media link bind failed: " + error);
    }
  }

  void unbind() { link_->unbind(); }
  bool isBound() const { return link_->isBound(); }

 private:
  std::unique_ptr<tdl_app::MediaLink> link_;
};

}  // namespace

NB_MODULE(tdl_py, m) {
  m.doc() = "CV184X dual-OS big-core bindings: VPSS frame capture (zero-copy) "
            "and RGN OSD overlay";

  m.def("init", []() {
    std::string error;
    if (!tdl_app::ensureMmfRuntimeInitialized(&error)) {
      raise("MMF runtime init failed: " + error);
    }
  }, "Initialize the dual-OS MMF runtime (CVI_SYS_Init via small core). "
     "Called automatically by VpssCamera.open() and Osd.create().");

  // --- pixel format constants -------------------------------------------
  m.attr("FORMAT_RGB888") = tdl_app::PixelFormat::RGB888;
  m.attr("FORMAT_BGR888") = tdl_app::PixelFormat::BGR888;
  m.attr("FORMAT_RGB888_PLANAR") = tdl_app::PixelFormat::RGB888_PLANAR;
  m.attr("FORMAT_BGR888_PLANAR") = tdl_app::PixelFormat::BGR888_PLANAR;
  m.attr("FORMAT_ARGB1555") = tdl_app::PixelFormat::ARGB1555;
  m.attr("FORMAT_ARGB4444") = tdl_app::PixelFormat::ARGB4444;
  m.attr("FORMAT_ARGB8888") = tdl_app::PixelFormat::ARGB8888;
  m.attr("FORMAT_YUV400") = tdl_app::PixelFormat::YUV400;
  m.attr("FORMAT_NV12") = tdl_app::PixelFormat::NV12;
  m.attr("FORMAT_NV21") = tdl_app::PixelFormat::NV21;

  // --- dual-OS layout constants ------------------------------------------
  m.attr("CAPTURE_GROUP") = tdl_app::DualOsLayout::kCaptureVpssGroup;   // 0
  m.attr("DISPLAY_GROUP") = tdl_app::DualOsLayout::kDisplayVpssGroup;   // 1
  m.attr("MAIN_CHANNEL") = tdl_app::DualOsLayout::kMainChannel;         // 0
  m.attr("AI_CHANNEL") = tdl_app::DualOsLayout::kAiChannel;             // 1
  m.attr("LIVE_CHANNEL") = tdl_app::DualOsLayout::kLiveChannel;         // 2
  m.attr("SUB_RGB_CHANNEL") = tdl_app::DualOsLayout::kSubRgbChannel;    // 3
  m.attr("DISPLAY_CHANNEL") = tdl_app::DualOsLayout::kDisplayChannel;   // 0
  m.attr("LIVE_WIDTH") = tdl_app::DualOsLayout::kLiveWidth;
  m.attr("LIVE_HEIGHT") = tdl_app::DualOsLayout::kLiveHeight;

  // --- frame ---------------------------------------------------------------
  nb::class_<PyFrame>(m, "Frame",
      "One captured VPSS frame. The pixel buffer is memory-mapped (no copy). "
      "Valid until release()/with-exit or the camera's next read().")
      .def_ro("width", &PyFrame::width_)
      .def_ro("height", &PyFrame::height_)
      .def_ro("format", &PyFrame::format_)
      .def_ro("sequence", &PyFrame::sequence_)
      .def_ro("timestamp_us", &PyFrame::timestamp_us_)
      .def_ro("phys_addr", &PyFrame::phys_addr_)
      .def_ro("plane_count", &PyFrame::plane_count_)
      .def_prop_ro("valid", &PyFrame::valid)
      .def_prop_ro("addr", &PyFrame::addr,
                   "Mapped virtual address of plane 0 (all planes contiguous)")
      .def_prop_ro("size", &PyFrame::size, "Total mapped size in bytes")
      .def_prop_ro("data", &PyFrame::data,
                   "Zero-copy read-only memoryview over all planes")
      .def("plane", &PyFrame::plane, nb::arg("index"),
           "Zero-copy read-only memoryview of one plane")
      .def_prop_ro("strides",
                   [](const PyFrame &f) {
                     return nb::make_tuple(f.strides_[0], f.strides_[1],
                                           f.strides_[2]);
                   })
      .def_prop_ro("plane_sizes",
                   [](const PyFrame &f) {
                     return nb::make_tuple(f.plane_sizes_[0], f.plane_sizes_[1],
                                           f.plane_sizes_[2]);
                   })
      .def_prop_ro("plane_offsets",
                   [](const PyFrame &f) {
                     return nb::make_tuple(f.plane_offsets_[0],
                                           f.plane_offsets_[1],
                                           f.plane_offsets_[2]);
                   })
      .def("release", &PyFrame::release,
           "Unmap the buffer and return it to the VPSS pool")
      .def("__enter__", [](nb::object self) { return self; })
      .def("__exit__",
           [](PyFrame &self, nb::handle, nb::handle, nb::handle) {
             self.release();
             return false;
           },
           nb::arg("exc_type").none(), nb::arg("exc_value").none(),
           nb::arg("traceback").none());

  // --- camera ----------------------------------------------------------------
  nb::class_<PyVpssCamera>(m, "VpssCamera",
      "Attach to a small-core VPSS channel and fetch frames (zero-copy). "
      "One frame outstanding at a time; read() invalidates the previous one.")
      .def(nb::init<int, int, int>(), nb::arg("group"), nb::arg("channel"),
           nb::arg("timeout_ms") = 1000)
      .def_static("main",
                  [](int timeout_ms) {
                    return new PyVpssCamera(
                        tdl_app::DualOsLayout::kCaptureVpssGroup,
                        tdl_app::DualOsLayout::kMainChannel, timeout_ms);
                  },
                  nb::arg("timeout_ms") = 1000,
                  "grp0/ch0 main 1920x1080 NV12")
      .def_static("ai",
                  [](int timeout_ms) {
                    return new PyVpssCamera(
                        tdl_app::DualOsLayout::kCaptureVpssGroup,
                        tdl_app::DualOsLayout::kAiChannel, timeout_ms);
                  },
                  nb::arg("timeout_ms") = 1000,
                  "grp0/ch1 ai 640x640 RGB888_PLANAR")
      .def_static("live",
                  [](int timeout_ms) {
                    return new PyVpssCamera(
                        tdl_app::DualOsLayout::kCaptureVpssGroup,
                        tdl_app::DualOsLayout::kLiveChannel, timeout_ms);
                  },
                  nb::arg("timeout_ms") = 1000,
                  "grp0/ch2 live 1280x720 NV12")
      .def_static("sub_rgb",
                  [](int timeout_ms) {
                    return new PyVpssCamera(
                        tdl_app::DualOsLayout::kCaptureVpssGroup,
                        tdl_app::DualOsLayout::kSubRgbChannel, timeout_ms);
                  },
                  nb::arg("timeout_ms") = 1000,
                  "grp0/ch3 subrgb 640x640 NV21")
      .def_static("screen",
                  [](int timeout_ms) {
                    return new PyVpssCamera(
                        tdl_app::DualOsLayout::kDisplayVpssGroup,
                        tdl_app::DualOsLayout::kDisplayChannel, timeout_ms);
                  },
                  nb::arg("timeout_ms") = 1000,
                  "grp1/ch0 display 1280x720 NV12")
      .def_prop_ro("group", &PyVpssCamera::group)
      .def_prop_ro("channel", &PyVpssCamera::channel)
      .def("open", &PyVpssCamera::open)
      // keep_alive<0, 1>: the returned Frame keeps this camera alive.
      .def("read", &PyVpssCamera::read, nb::keep_alive<0, 1>(),
           "Blocking read (up to timeout_ms); returns a zero-copy Frame")
      .def("close", &PyVpssCamera::close)
      .def("__enter__", [](nb::object self) { return self; })
      .def("__exit__",
           [](PyVpssCamera &self, nb::handle, nb::handle, nb::handle) {
             self.close();
             return false;
           },
           nb::arg("exc_type").none(), nb::arg("exc_value").none(),
           nb::arg("traceback").none());

  // --- OSD -------------------------------------------------------------------
  nb::class_<PyOsdCanvas>(m, "OsdCanvas",
      "RGN back-buffer view; valid until the matching Osd.update(). "
      "Fetch a fresh canvas for every draw cycle (double buffered).")
      .def_ro("addr", &PyOsdCanvas::addr)
      .def_ro("size", &PyOsdCanvas::size)
      .def_ro("width", &PyOsdCanvas::width)
      .def_ro("height", &PyOsdCanvas::height)
      .def_ro("stride", &PyOsdCanvas::stride)
      .def_ro("format", &PyOsdCanvas::format)
      .def_prop_ro("data", &PyOsdCanvas::data,
                   "Zero-copy writable memoryview over the canvas");

  nb::class_<PyOsd>(m, "Osd",
      "RGN overlay region drawn over a VPSS channel (defaults target the "
      "display path grp1/ch0). Typical cycle: canvas() -> write pixels -> "
      "update().")
      .def(nb::init<int, int, int, int, int, std::uint32_t>(),
           nb::arg("handle"),
           nb::arg("width") = tdl_app::DualOsLayout::kLiveWidth,
           nb::arg("height") = tdl_app::DualOsLayout::kLiveHeight,
           nb::arg("pixel_format") = tdl_app::PixelFormat::ARGB8888,
           nb::arg("canvas_count") = 2, nb::arg("bg_color") = 0)
      .def("create", &PyOsd::create)
      .def("attach", &PyOsd::attach,
           nb::arg("group") = tdl_app::DualOsLayout::kDisplayVpssGroup,
           nb::arg("channel") = tdl_app::DualOsLayout::kDisplayChannel,
           nb::arg("x") = 0, nb::arg("y") = 0, nb::arg("layer") = 10)
      .def("canvas", &PyOsd::canvas)
      .def("persistent_canvas", &PyOsd::persistentCanvas,
           "Resident canvas: a writable view that stays valid across "
           "update() calls (mapping owned by this object, not the RGN SDK). "
           "Use it as a persistent framebuffer, e.g. for LVGL direct "
           "rendering. Requires canvas_count=1.")
      .def("persistent_pair", &PyOsd::persistentPair,
           "Resident double buffering (requires canvas_count=2): returns "
           "(back, front) writable views over the two ping-pong canvases. "
           "Give them to LVGL as (buf1, buf2) in DIRECT mode; each update() "
           "swaps, staying phase-locked with LVGL's buffer alternation. "
           "Pushes one cleared frame during setup.")
      .def("update", &PyOsd::update)
      .def("set_visible", &PyOsd::setVisible, nb::arg("visible"))
      .def("move_to", &PyOsd::moveTo, nb::arg("x"), nb::arg("y"))
      .def("detach", &PyOsd::detach)
      .def("destroy", &PyOsd::destroy)
      .def_prop_ro("created", &PyOsd::isCreated)
      .def_prop_ro("attached", &PyOsd::isAttached)
      .def_prop_ro("handle", &PyOsd::handle);

  // --- display path ---------------------------------------------------------
  m.attr("INTERFACE_MIPI") = tdl_app::VoInterfaceType::Mipi;
  m.attr("INTERFACE_LVDS") = tdl_app::VoInterfaceType::Lvds;
  m.attr("SYNC_720x1280_60") = tdl_app::VoInterfaceSync::P720_1280_60;
  m.attr("SYNC_1080x1920_60") = tdl_app::VoInterfaceSync::P1080_1920_60;
  m.attr("SYNC_480x800_60") = tdl_app::VoInterfaceSync::P480_800_60;
  m.attr("SYNC_440x1920_60") = tdl_app::VoInterfaceSync::P440_1920_60;
  m.attr("SYNC_480x640_60") = tdl_app::VoInterfaceSync::P480_640_60;

  nb::class_<PyVoOutput>(m, "VoOutput",
      "MIPI panel output. Defaults match the dual-OS portrait screen "
      "(720x1280 NV12, hardware rotation 90).")
      .def(nb::init<int, int, int, int, int, int, int, int, int>(),
           nb::arg("device") = tdl_app::DualOsLayout::kVoDevice,
           nb::arg("layer") = 0,
           nb::arg("channel") = tdl_app::DualOsLayout::kVoChannel,
           nb::arg("width") = tdl_app::DualOsLayout::kScreenWidth,
           nb::arg("height") = tdl_app::DualOsLayout::kScreenHeight,
           nb::arg("pixel_format") = tdl_app::PixelFormat::NV12,
           nb::arg("interface_type") = tdl_app::VoInterfaceType::Mipi,
           nb::arg("interface_sync") = tdl_app::VoInterfaceSync::P720_1280_60,
           nb::arg("rotation") = tdl_app::DualOsLayout::kVoRotation)
      .def("open", &PyVoOutput::open)
      .def("close", &PyVoOutput::close)
      .def_prop_ro("opened", &PyVoOutput::isOpen);

  nb::class_<PyMediaLink>(m, "MediaLink",
      "CVI_SYS_Bind between two media channels. Display path needs two: "
      "vpss_to_vpss(0, 2, 1, 0) then vpss_to_vo(1, 0, 0, 0).")
      .def_static("vpss_to_vpss",
                  [](int src_group, int src_channel, int dst_group,
                     int dst_channel) {
                    return new PyMediaLink(
                        tdl_app::MediaChannel::vpss(src_group, src_channel),
                        tdl_app::MediaChannel::vpss(dst_group, dst_channel));
                  },
                  nb::arg("src_group"), nb::arg("src_channel"),
                  nb::arg("dst_group"), nb::arg("dst_channel"))
      .def_static("vpss_to_vo",
                  [](int src_group, int src_channel, int layer, int channel) {
                    return new PyMediaLink(
                        tdl_app::MediaChannel::vpss(src_group, src_channel),
                        tdl_app::MediaChannel::vo(layer, channel));
                  },
                  nb::arg("src_group"), nb::arg("src_channel"),
                  nb::arg("layer") = 0, nb::arg("channel") = 0)
      .def("bind", &PyMediaLink::bind)
      .def("unbind", &PyMediaLink::unbind)
      .def_prop_ro("bound", &PyMediaLink::isBound);

  // --- state queries (for idempotent setup scripts) --------------------------
  m.def("vo_is_enabled",
        [](int device) {
          std::string error;
          if (!tdl_app::ensureMmfRuntimeInitialized(&error)) {
            raise("MMF runtime init failed: " + error);
          }
          return CVI_VO_IsEnabled(device) == CVI_TRUE;
        },
        nb::arg("device") = 0,
        "True when the VO device is already enabled (panel lit)");

  m.def("get_bind_source_vpss",
        [](int group, int channel) {
          return getBindSource(makeMmfChannel(CVI_ID_VPSS, group, channel));
        },
        nb::arg("group"), nb::arg("channel") = 0,
        "Bind source feeding a VPSS group as (module, dev, chn), or None. "
        "e.g. get_bind_source_vpss(1) -> ('vpss', 0, 2) when live is bound");

  m.def("get_bind_source_vo",
        [](int layer, int channel) {
          return getBindSource(makeMmfChannel(CVI_ID_VO, layer, channel));
        },
        nb::arg("layer") = 0, nb::arg("channel") = 0,
        "Bind source feeding a VO channel as (module, dev, chn), or None");
}
