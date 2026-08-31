// Python bindings (nanobind) for the CV184X dual-OS big-core media path:
//   - VpssCamera: attach to a small-core VPSS channel and fetch frames with
//     zero-copy access (CVI_SYS_Mmap of the VB buffer, exposed as memoryview
//     plus raw addr/size for ctypes users).
//   - Osd: RGN overlay region attached to a VPSS channel; the double-buffered
//     canvas is exposed as a writable memoryview plus raw addr/size.
//   - Detector/Classifier/KeypointDetector/InstanceSegmenter/PlateRecognizer:
//     NPU inference on VpssCamera frames or image files (TDL_PY_WITH_NPU).
//
// Frame lifecycle contract (agreed): one outstanding frame per camera. The
// mapped view becomes invalid as soon as the frame is released (explicitly,
// via `with`, or implicitly by the next read()); accessing a stale view is
// undefined behaviour and is intentionally not guarded.

#include <nanobind/nanobind.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#include "cvi_comm_video.h"
#include "cvi_region.h"
#include "cvi_sys.h"
#include "cvi_vo.h"
#include "tdl_app/camera.hpp"
#include "tdl_app/layout.hpp"
#include "tdl_app/media_link.hpp"
#include "tdl_app/media_types.hpp"
#include "tdl_app/osd_region.hpp"
#include "tdl_app/sys_context.hpp"
#include "tdl_app/touch_input.hpp"
#include "tdl_app/vo_output.hpp"
#include "tdl_app/rgb_led.hpp"

#ifdef TDL_PY_WITH_NPU
#include "algorithm/private/vpss_preprocessor.hpp"
#include "tdl_app/classifier.hpp"
#include "tdl_app/byte_tracker.hpp"
#include "tdl_app/detector.hpp"
#include "tdl_app/face_emotion_recognizer.hpp"
#include "tdl_app/face_recognizer.hpp"
#include "tdl_app/hand_gesture_recognizer.hpp"
#include "tdl_app/instance_segmenter.hpp"
#include "tdl_app/keypoint_detector.hpp"
#include "tdl_app/model_descriptor.hpp"
#include "tdl_app/plate_recognizer.hpp"
#include "tdl_app/pose_classifier.hpp"
#include "tdl_app/self_learning_classifier.hpp"
#include "tdl_app/single_object_tracker.hpp"
#include "tdl_app/vision_task_types.hpp"
#endif

namespace nb = nanobind;

void registerAudioBindings(nb::module_ &m);

namespace {

[[noreturn]] void raise(const std::string &message) {
  throw std::runtime_error(message);
}

#ifdef TDL_PY_WITH_NPU
tdl_app::Box boxFromPython(nb::handle value) {
  tdl_app::Box box;
  box.x1 = nb::cast<float>(value.attr("x1"));
  box.y1 = nb::cast<float>(value.attr("y1"));
  box.x2 = nb::cast<float>(value.attr("x2"));
  box.y2 = nb::cast<float>(value.attr("y2"));
  box.score = nb::cast<float>(value.attr("score"));
  box.class_id = nb::cast<int>(value.attr("class_id"));
  if (!box.valid()) {
    raise("box must satisfy x2 > x1 and y2 > y1");
  }
  return box;
}

jyd_tracker::Detection trackerDetectionFromPython(nb::handle value) {
  const tdl_app::Box box = boxFromPython(value);
  return {box.x1, box.y1, box.x2, box.y2, box.score, box.class_id};
}

tdl_app::Box trackerBox(const jyd_tracker::Track &track) {
  tdl_app::Box box;
  box.x1 = track.box.x1;
  box.y1 = track.box.y1;
  box.x2 = track.box.x2;
  box.y2 = track.box.y2;
  box.score = track.box.score;
  box.class_id = track.box.class_id;
  return box;
}
#endif

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
      : camera_(camera), info_(info), mapped_(mapped), map_size_(map_size) {
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

  // Copy of the frame's VIDEO_FRAME_INFO_S. Its physical addresses stay valid
  // while the frame is alive (the camera holds the VB until the next read() or
  // close()), so NPU inference can consume it zero-copy.
  const VIDEO_FRAME_INFO_S &nativeInfo() const { return info_; }

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
  VIDEO_FRAME_INFO_S info_ {};
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

#ifdef TDL_PY_WITH_NPU
// Builds the SDK frame view over a live PyFrame for the algorithm classes.
tdl_app::Frame sdkFrameFrom(PyFrame &frame) {
  frame.requireValid();
  tdl_app::Frame out;
  out.native = const_cast<VIDEO_FRAME_INFO_S *>(&frame.nativeInfo());
  out.width = frame.width_;
  out.height = frame.height_;
  out.format = frame.format_;
  out.sequence = frame.sequence_;
  out.timestamp_us = frame.timestamp_us_;
  return out;
}

// Shared load path for every algorithm wrapper: ModelSessionConfig::fromSpec,
// GIL released (model load can take seconds), errors become exceptions.
template <typename Model>
void loadModel(Model &model, const char *what, const std::string &model_spec,
               const std::string &firmware) {
  std::string error;
  bool ok = false;
  {
    nb::gil_scoped_release guard;
    ok = model.load(tdl_app::ModelSessionConfig::fromSpec(model_spec, firmware),
                    &error);
  }
  if (!ok) {
    raise(std::string(what) + " load failed: " + error);
  }
}

// Face dense landmark (face_dense_real.mud, runtime "face_dense_landmark") is
// a second-stage model: a first-stage face detector (SCRFD / YOLOv8-face, via
// tdl_py.Detector) supplies face boxes, and this runtime estimates dense
// landmarks per face ROI. No tdl_app class wraps this runtime yet, so this
// mirrors the camera path of apps/tdl_face_dense_keypoint_demo.cpp: VPSS
// hardware ROI crop (zero-copy from the camera VB), bmrt inference, output
// dequantize, then coordinates are mapped back to frame space.
class PyFaceDenseLandmark {
 public:
  PyFaceDenseLandmark() = default;
  ~PyFaceDenseLandmark() { close(); }

  PyFaceDenseLandmark(const PyFaceDenseLandmark &) = delete;
  PyFaceDenseLandmark &operator=(const PyFaceDenseLandmark &) = delete;

  bool initialized() const { return runtime_ != nullptr; }
  int inputWidth() const { return input_width_; }
  int inputHeight() const { return input_height_; }
  int landmarkCount() const { return landmark_count_; }

  bool load(const std::string &model_spec, const std::string &firmware,
            std::string *error) {
    close();
    if (!tdl_app::loadModelDescriptor(model_spec, &descriptor_, error)) {
      return false;
    }
    if (descriptor_.model_path.empty()) {
      fail(error, "dense landmark descriptor missing model path");
      return false;
    }

    if (bm_dev_request(&handle_, 0) != BM_SUCCESS) {
      fail(error, "bm_dev_request failed");
      close();
      return false;
    }
    if (!firmware.empty()) {
      setenv("BMRUNTIME_USING_FIRMWARE", firmware.c_str(), 1);
    }
    runtime_ = bmrt_create(handle_);
    if (!runtime_) {
      fail(error, "bmrt_create failed");
      close();
      return false;
    }
    if (!bmrt_load_bmodel(runtime_, descriptor_.model_path.c_str())) {
      fail(error, "bmrt_load_bmodel failed: " + descriptor_.model_path);
      close();
      return false;
    }

    const char **net_names = nullptr;
    bmrt_get_network_names(runtime_, &net_names);
    if (!net_names || bmrt_get_network_number(runtime_) <= 0) {
      fail(error, "dense landmark bmodel has no network");
      if (net_names) {
        std::free(net_names);
      }
      close();
      return false;
    }
    net_name_ = net_names[0];
    std::free(net_names);

    net_info_ = bmrt_get_network_info(runtime_, net_name_.c_str());
    if (!net_info_) {
      fail(error, "bmrt_get_network_info failed");
      close();
      return false;
    }
    if (net_info_->input_num != 1 || net_info_->output_num < 1 ||
        net_info_->stage_num < 1) {
      fail(error, "unexpected dense landmark network io layout");
      close();
      return false;
    }
    if (!parseInputShape(net_info_->stages[0].input_shapes[0], error) ||
        !parseOutputLayout(error)) {
      close();
      return false;
    }
    input_dtype_ = net_info_->input_dtypes[0];

    // The camera path feeds the model straight from the VPSS hardware
    // preprocessor, which only supports NHWC uint8 model inputs.
    if (nchw_layout_ || input_dtype_ != BM_UINT8) {
      fail(error, "dense landmark frame path requires an NHWC uint8 model");
      close();
      return false;
    }
    tdl_app::bmrt_runtime::VpssPreprocessor::Config vpss_config;
    vpss_config.width = input_width_;
    vpss_config.height = input_height_;
    vpss_config.rgb = normalizeToken(descriptor_.input_type) == "RGB";
    vpss_config.interleaved = true;
    vpss_config.input_dtype = input_dtype_;
    vpss_config.input_scale =
        net_info_->input_scales ? net_info_->input_scales[0] : 1.0f;
    vpss_config.input_zero_point =
        net_info_->input_zero_point ? net_info_->input_zero_point[0] : 0;
    const float mean = descriptor_.mean.empty() ? 0.0f : descriptor_.mean[0];
    const float scale =
        descriptor_.scale.empty() ? 1.0f / 255.0f : descriptor_.scale[0];
    vpss_config.mean = {{mean, mean, mean}};
    vpss_config.scale = {{scale, scale, scale}};
    preprocessor_.reset(new tdl_app::bmrt_runtime::VpssPreprocessor());
    if (!preprocessor_->open(handle_, vpss_config, error) ||
        !allocateDeviceOutputs(error)) {
      close();
      return false;
    }
    return true;
  }

  bool estimate(const VIDEO_FRAME_INFO_S &frame, const tdl_app::Box &box,
                float expand_ratio, std::vector<tdl_app::Point> *points,
                std::string *error) {
    if (!runtime_ || !preprocessor_ || !net_info_) {
      fail(error, "dense landmark runtime is not initialized");
      return false;
    }
    if (!points) {
      fail(error, "points output pointer is null");
      return false;
    }

    tdl_app::bmrt_runtime::VpssPreprocessor::Roi roi;
    if (!makeRoi(box, expand_ratio, static_cast<int>(frame.stVFrame.u32Width),
                 static_cast<int>(frame.stVFrame.u32Height), &roi)) {
      fail(error, "face box falls outside of the frame");
      return false;
    }

    if (!preprocessor_->preprocess(
            const_cast<VIDEO_FRAME_INFO_S *>(&frame), &roi, error)) {
      return false;
    }

    bm_tensor_t input_tensor {};
    bmrt_tensor_with_device(&input_tensor, preprocessor_->inputMemory(),
                            input_dtype_,
                            net_info_->stages[0].input_shapes[0]);
    std::vector<bm_tensor_t> output_tensors(
        static_cast<size_t>(net_info_->output_num), bm_tensor_t {});
    for (int i = 0; i < net_info_->output_num; ++i) {
      bmrt_tensor_with_device(&output_tensors[static_cast<size_t>(i)],
                              output_memories_[static_cast<size_t>(i)],
                              net_info_->output_dtypes[i],
                              net_info_->stages[0].output_shapes[i]);
    }
    if (!bmrt_launch_tensor_ex(runtime_, net_name_.c_str(), &input_tensor, 1,
                               output_tensors.data(), net_info_->output_num,
                               true, false) ||
        bm_thread_sync(handle_) != BM_SUCCESS) {
      fail(error, "dense landmark device launch failed");
      return false;
    }

    std::vector<std::vector<std::uint8_t>> output_bytes(
        static_cast<size_t>(net_info_->output_num));
    std::vector<bm_shape_t> output_shapes(
        static_cast<size_t>(net_info_->output_num));
    for (int i = 0; i < net_info_->output_num; ++i) {
      output_shapes[static_cast<size_t>(i)] =
          output_tensors[static_cast<size_t>(i)].shape;
      size_t count = 1;
      for (int d = 0; d < output_shapes[static_cast<size_t>(i)].num_dims;
           ++d) {
        count *=
            static_cast<size_t>(output_shapes[static_cast<size_t>(i)].dims[d]);
      }
      output_bytes[static_cast<size_t>(i)].resize(
          count * bmrt_data_type_size(net_info_->output_dtypes[i]));
      if (bm_memcpy_d2s(handle_, output_bytes[static_cast<size_t>(i)].data(),
                        output_tensors[static_cast<size_t>(i)].device_mem) !=
          BM_SUCCESS) {
        fail(error, "dense landmark output copy failed");
        return false;
      }
    }

    std::vector<tdl_app::Point> local_points;
    if (!decodePoints(output_bytes, output_shapes, &local_points, error)) {
      return false;
    }

    // Model coordinates are relative to the ROI crop; map them back to the
    // source frame coordinate space.
    points->clear();
    points->reserve(local_points.size());
    for (const tdl_app::Point &point : local_points) {
      tdl_app::Point mapped = point;
      mapped.x = roi.x + point.x * roi.width / input_width_;
      mapped.y = roi.y + point.y * roi.height / input_height_;
      points->push_back(mapped);
    }
    return true;
  }

  void close() {
    preprocessor_.reset();
    if (handle_) {
      for (bm_device_mem_t &memory : output_memories_) {
        if (memory.size > 0) {
          bm_free_device(handle_, memory);
          memory = bm_device_mem_t {};
        }
      }
    }
    output_memories_.clear();
    if (runtime_) {
      bmrt_destroy(runtime_);
      runtime_ = nullptr;
    }
    if (handle_) {
      bm_dev_free(handle_);
      handle_ = nullptr;
    }
    net_info_ = nullptr;
    net_name_.clear();
    input_width_ = 0;
    input_height_ = 0;
    nchw_layout_ = false;
    coord_output_index_ = -1;
    landmark_count_ = 0;
    coordinate_extent_ = 0.0f;
    input_dtype_ = BM_UINT8;
    descriptor_ = tdl_app::ModelDescriptor {};
  }

 private:
  static void fail(std::string *error, const std::string &message) {
    if (error) {
      *error = message;
    }
  }

  static std::string normalizeToken(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                     return static_cast<char>(std::toupper(c));
                   });
    std::replace(value.begin(), value.end(), '-', '_');
    return value;
  }

  // Face ROI is the square around the detection box center, enlarged by
  // expand_ratio, clamped to the frame.
  static bool makeRoi(const tdl_app::Box &box, float expand_ratio,
                      int image_width, int image_height,
                      tdl_app::bmrt_runtime::VpssPreprocessor::Roi *roi) {
    if (!roi || image_width <= 0 || image_height <= 0) {
      return false;
    }
    const float w = std::max(1.0f, box.x2 - box.x1);
    const float h = std::max(1.0f, box.y2 - box.y1);
    const float cx = (box.x1 + box.x2) * 0.5f;
    const float cy = (box.y1 + box.y2) * 0.5f;
    const float side = std::max(w, h) * std::max(1.0f, 1.0f + expand_ratio);
    const int x1 = std::max(
        0, std::min(image_width - 1,
                    static_cast<int>(std::floor(cx - side * 0.5f))));
    const int y1 = std::max(
        0, std::min(image_height - 1,
                    static_cast<int>(std::floor(cy - side * 0.5f))));
    const int x2 = std::max(
        x1 + 1, std::min(image_width,
                         static_cast<int>(std::ceil(cx + side * 0.5f))));
    const int y2 = std::max(
        y1 + 1, std::min(image_height,
                         static_cast<int>(std::ceil(cy + side * 0.5f))));
    roi->x = x1;
    roi->y = y1;
    roi->width = x2 - x1;
    roi->height = y2 - y1;
    return true;
  }

  bool parseInputShape(const bm_shape_t &shape, std::string *error) {
    if (shape.num_dims != 4) {
      fail(error, "dense landmark model only supports 4D input");
      return false;
    }
    if (shape.dims[3] == 3) {
      nchw_layout_ = false;
      input_height_ = shape.dims[1];
      input_width_ = shape.dims[2];
      return true;
    }
    if (shape.dims[1] == 3) {
      nchw_layout_ = true;
      input_height_ = shape.dims[2];
      input_width_ = shape.dims[3];
      return true;
    }
    fail(error, "unable to infer dense landmark input layout");
    return false;
  }

  bool parseOutputLayout(std::string *error) {
    coord_output_index_ = -1;
    landmark_count_ = 0;
    coordinate_extent_ =
        static_cast<float>(std::max(input_width_, input_height_));
    size_t best_coord_elements = 0;
    for (int i = 0; i < net_info_->output_num; ++i) {
      size_t element_count = 1;
      const bm_shape_t &shape = net_info_->stages[0].output_shapes[i];
      for (int d = 0; d < shape.num_dims; ++d) {
        element_count *= static_cast<size_t>(shape.dims[d]);
      }
      if (element_count > 3 && element_count % 3 == 0 &&
          element_count > best_coord_elements) {
        coord_output_index_ = i;
        best_coord_elements = element_count;
        landmark_count_ = static_cast<int>(element_count / 3);
      }
    }
    if (coord_output_index_ < 0) {
      fail(error,
           "dense landmark model does not expose a usable coordinate output");
      return false;
    }
    coordinate_extent_ = inferQuantizedCoordinateExtent(coord_output_index_);
    return true;
  }

  float inferQuantizedCoordinateExtent(int output_index) const {
    const bm_data_type_t dtype = net_info_->output_dtypes[output_index];
    if (dtype != BM_INT8 && dtype != BM_UINT8) {
      return static_cast<float>(std::max(input_width_, input_height_));
    }

    const float scale =
        net_info_->output_scales ? net_info_->output_scales[output_index]
                                 : 1.0f;
    const int zero_point = net_info_->output_zero_point
                               ? net_info_->output_zero_point[output_index]
                               : 0;
    const int qmin = (dtype == BM_INT8) ? -128 : 0;
    const int qmax = (dtype == BM_INT8) ? 127 : 255;
    const float min_value = (static_cast<float>(qmin) - zero_point) * scale;
    const float max_value = (static_cast<float>(qmax) - zero_point) * scale;
    const float magnitude =
        std::max(std::fabs(min_value), std::fabs(max_value));
    if (magnitude <= 1.5f) {
      return 1.0f;
    }
    return std::max(16.0f, std::round(magnitude / 16.0f) * 16.0f);
  }

  bool allocateDeviceOutputs(std::string *error) {
    output_memories_.resize(static_cast<size_t>(net_info_->output_num));
    for (int i = 0; i < net_info_->output_num; ++i) {
      size_t bytes = net_info_->max_output_bytes[i];
      if (bytes == 0) {
        const bm_shape_t &shape = net_info_->stages[0].output_shapes[i];
        size_t count = 1;
        for (int d = 0; d < shape.num_dims; ++d) {
          count *= static_cast<size_t>(shape.dims[d]);
        }
        bytes = count * bmrt_data_type_size(net_info_->output_dtypes[i]);
      }
      if (bytes == 0 ||
          bm_malloc_device_byte(handle_,
                                &output_memories_[static_cast<size_t>(i)],
                                bytes) != BM_SUCCESS) {
        fail(error, "dense landmark output allocation failed");
        return false;
      }
    }
    return true;
  }

  bool decodePoints(
      const std::vector<std::vector<std::uint8_t>> &output_bytes,
      const std::vector<bm_shape_t> &output_shapes,
      std::vector<tdl_app::Point> *points, std::string *error) const {
    if (!points || coord_output_index_ < 0 ||
        static_cast<size_t>(coord_output_index_) >= output_bytes.size()) {
      fail(error, "dense landmark coordinate output is unavailable");
      return false;
    }
    std::vector<float> coords;
    if (!decodeOutput(output_bytes[static_cast<size_t>(coord_output_index_)],
                      output_shapes[static_cast<size_t>(coord_output_index_)],
                      coord_output_index_, &coords, error)) {
      return false;
    }
    if (coords.size() < 3 || coords.size() % 3 != 0) {
      fail(error, "unexpected dense landmark coordinate count: " +
                      std::to_string(coords.size()));
      return false;
    }
    points->clear();
    points->reserve(coords.size() / 3);
    const float x_scale =
        static_cast<float>(input_width_) / std::max(1.0f, coordinate_extent_);
    const float y_scale =
        static_cast<float>(input_height_) / std::max(1.0f, coordinate_extent_);
    for (size_t i = 0; i + 2 < coords.size(); i += 3) {
      tdl_app::Point point;
      point.x = std::max(
          0.0f, std::min(static_cast<float>(input_width_),
                         coords[i] * x_scale));
      point.y = std::max(
          0.0f, std::min(static_cast<float>(input_height_),
                         coords[i + 1] * y_scale));
      points->push_back(point);
    }
    return true;
  }

  bool decodeOutput(const std::vector<std::uint8_t> &raw_bytes,
                    const bm_shape_t &shape, int output_index,
                    std::vector<float> *decoded, std::string *error) const {
    size_t element_count = 1;
    for (int d = 0; d < shape.num_dims; ++d) {
      element_count *= static_cast<size_t>(shape.dims[d]);
    }
    decoded->assign(element_count, 0.0f);

    const float scale =
        net_info_->output_scales ? net_info_->output_scales[output_index]
                                 : 1.0f;
    const int zero_point = net_info_->output_zero_point
                               ? net_info_->output_zero_point[output_index]
                               : 0;
    const bm_data_type_t dtype = net_info_->output_dtypes[output_index];

    if (dtype == BM_FLOAT32) {
      const float *ptr = reinterpret_cast<const float *>(raw_bytes.data());
      decoded->assign(ptr, ptr + element_count);
      return true;
    }
    if (dtype == BM_INT8) {
      const int8_t *ptr = reinterpret_cast<const int8_t *>(raw_bytes.data());
      for (size_t i = 0; i < element_count; ++i) {
        (*decoded)[i] = (static_cast<int>(ptr[i]) - zero_point) * scale;
      }
      return true;
    }
    if (dtype == BM_UINT8) {
      const uint8_t *ptr = reinterpret_cast<const uint8_t *>(raw_bytes.data());
      for (size_t i = 0; i < element_count; ++i) {
        (*decoded)[i] = (static_cast<int>(ptr[i]) - zero_point) * scale;
      }
      return true;
    }

    fail(error, "unsupported dense landmark output dtype");
    return false;
  }

  tdl_app::ModelDescriptor descriptor_;
  bm_handle_t handle_ = nullptr;
  void *runtime_ = nullptr;
  const bm_net_info_t *net_info_ = nullptr;
  std::string net_name_;
  int input_width_ = 0;
  int input_height_ = 0;
  bool nchw_layout_ = false;
  int coord_output_index_ = -1;
  int landmark_count_ = 0;
  float coordinate_extent_ = 0.0f;
  bm_data_type_t input_dtype_ = BM_UINT8;
  std::unique_ptr<tdl_app::bmrt_runtime::VpssPreprocessor> preprocessor_;
  std::vector<bm_device_mem_t> output_memories_;
};
#endif  // TDL_PY_WITH_NPU

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
             int rotation, bool preserve_hardware_on_close) {
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
    config.preserve_hardware_on_close = preserve_hardware_on_close;
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
  m.doc() = "CV184X dual-OS big-core bindings: VPSS frame capture (zero-copy), "
            "RGN OSD overlay, and NPU inference";

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
  m.attr("REAR_GROUP") = tdl_app::DualOsLayout::kRearVpssGroup;         // 3
  m.attr("AI_CHANNEL") = tdl_app::DualOsLayout::kAiChannel;             // 1
  m.attr("LIVE_CHANNEL") = tdl_app::DualOsLayout::kLiveChannel;         // 2
  m.attr("SUB_RGB_CHANNEL") = tdl_app::DualOsLayout::kSubRgbChannel;    // 3
  m.attr("DISPLAY_CHANNEL") = tdl_app::DualOsLayout::kDisplayChannel;   // 0
  m.attr("RGB_CHANNEL") = tdl_app::DualOsLayout::kRgbChannel;           // 0
  m.attr("AI_WIDTH") = tdl_app::DualOsLayout::kAiWidth;
  m.attr("AI_HEIGHT") = tdl_app::DualOsLayout::kAiHeight;
  m.attr("LIVE_WIDTH") = tdl_app::DualOsLayout::kLiveWidth;
  m.attr("LIVE_HEIGHT") = tdl_app::DualOsLayout::kLiveHeight;
  m.attr("SUB_RGB_WIDTH") = tdl_app::DualOsLayout::kSubRgbWidth;
  m.attr("SUB_RGB_HEIGHT") = tdl_app::DualOsLayout::kSubRgbHeight;
  m.attr("SCREEN_WIDTH") = tdl_app::DualOsLayout::kScreenWidth;
  m.attr("SCREEN_HEIGHT") = tdl_app::DualOsLayout::kScreenHeight;
  m.attr("RGB_WIDTH") = tdl_app::DualOsLayout::kRgbWidth;
  m.attr("RGB_HEIGHT") = tdl_app::DualOsLayout::kRgbHeight;
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
                  "grp0/ch2 live 720x480 NV12")
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
                  "grp1/ch0 display 720x480 NV12")
      .def_static("rgb",
                  [](int timeout_ms) {
                    return new PyVpssCamera(
                        tdl_app::DualOsLayout::kCaptureVpssGroup,
                        tdl_app::DualOsLayout::kRgbChannel, timeout_ms);
                  },
                  nb::arg("timeout_ms") = 1000,
                  "grp0/ch0 rgb 720x480 RGB888_PLANAR; actual channel "
                  "config is owned by the small core")
      .def_static("rear_ai",
                  [](int timeout_ms) {
                    return new PyVpssCamera(
                        tdl_app::DualOsLayout::kRearVpssGroup,
                        tdl_app::DualOsLayout::kAiChannel, timeout_ms);
                  },
                  nb::arg("timeout_ms") = 1000,
                  "grp3/ch1 rear ai 640x640 RGB888_PLANAR")
      .def_static("rear_live",
                  [](int timeout_ms) {
                    return new PyVpssCamera(
                        tdl_app::DualOsLayout::kRearVpssGroup,
                        tdl_app::DualOsLayout::kLiveChannel, timeout_ms);
                  },
                  nb::arg("timeout_ms") = 1000,
                  "grp3/ch2 rear live 720x480 NV12")
      .def_static("rear_sub_rgb",
                  [](int timeout_ms) {
                    return new PyVpssCamera(
                        tdl_app::DualOsLayout::kRearVpssGroup,
                        tdl_app::DualOsLayout::kSubRgbChannel, timeout_ms);
                  },
                  nb::arg("timeout_ms") = 1000,
                  "grp3/ch3 rear subrgb 640x640 NV21")
      .def_static("rear_rgb",
                  [](int timeout_ms) {
                    return new PyVpssCamera(
                        tdl_app::DualOsLayout::kRearVpssGroup,
                        tdl_app::DualOsLayout::kRgbChannel, timeout_ms);
                  },
                  nb::arg("timeout_ms") = 1000,
                  "grp3/ch0 rear rgb 720x480 RGB888_PLANAR (nominal; actual "
                  "frame attributes come from the small-core channel config)")
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
  m.attr("SYNC_720x480_60") = tdl_app::VoInterfaceSync::P720_480_60;
  m.attr("SYNC_720x1280_60") = tdl_app::VoInterfaceSync::P720_1280_60;
  m.attr("SYNC_1080x1920_60") = tdl_app::VoInterfaceSync::P1080_1920_60;
  m.attr("SYNC_480x800_60") = tdl_app::VoInterfaceSync::P480_800_60;
  m.attr("SYNC_440x1920_60") = tdl_app::VoInterfaceSync::P440_1920_60;
  m.attr("SYNC_480x640_60") = tdl_app::VoInterfaceSync::P480_640_60;

  nb::class_<PyVoOutput>(m, "VoOutput",
      "MIPI panel output. Defaults match the dual-OS landscape screen "
      "(720x480 NV12, no rotation).")
      .def(nb::init<int, int, int, int, int, int, int, int, int, bool>(),
           nb::arg("device") = tdl_app::DualOsLayout::kVoDevice,
           nb::arg("layer") = 0,
           nb::arg("channel") = tdl_app::DualOsLayout::kVoChannel,
           nb::arg("width") = tdl_app::DualOsLayout::kScreenWidth,
           nb::arg("height") = tdl_app::DualOsLayout::kScreenHeight,
           nb::arg("pixel_format") = tdl_app::PixelFormat::NV12,
           nb::arg("interface_type") = tdl_app::VoInterfaceType::Mipi,
           nb::arg("interface_sync") = tdl_app::VoInterfaceSync::P720_480_60,
           nb::arg("rotation") = tdl_app::DualOsLayout::kVoRotation,
           nb::arg("preserve_hardware_on_close") = false)
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
  // --- RGB LED ---------------------------------------------------------------
  nb::class_<tdl_app::RgbLed>(m, "RgbLed",
      "RGB LED controller through the dual-OS small core.")
      .def(nb::init<std::uint8_t>(), nb::arg("pixel_count") = 14)

      .def("set_pixel",
          [](tdl_app::RgbLed &self, std::uint8_t index,
              std::uint8_t r, std::uint8_t g, std::uint8_t b) {
            std::string ignored_error;
            return self.setPixel(index, {r, g, b}, &ignored_error);
          },
          nb::arg("index"), nb::arg("r"), nb::arg("g"), nb::arg("b"))

      .def("set_all",
           [](tdl_app::RgbLed &self, std::uint8_t r,
              std::uint8_t g, std::uint8_t b) {
             std::string ignored_error;
             return self.setAll(r, g, b, &ignored_error);
           },
           nb::arg("r"), nb::arg("g"), nb::arg("b"))

      .def("brightness",
           [](tdl_app::RgbLed &self, std::uint8_t value) {
             std::string error;
            return self.setBrightness(value, &error);
           },
           nb::arg("value"))

      .def("show", [](tdl_app::RgbLed &self) {
        std::string error;
        return self.show(&error);
      })

      .def("clear", [](tdl_app::RgbLed &self) {
        std::string error;
        return self.clear(&error);
      });

  registerAudioBindings(m);

  // --- touch input -----------------------------------------------------------
  nb::class_<tdl_app::TouchEvent>(m, "TouchEvent",
      "One touchscreen event in screen coordinates.")
      .def_prop_ro("phase", [](const tdl_app::TouchEvent &self) {
        switch (self.phase) {
          case tdl_app::TouchPhase::Down: return "down";
          case tdl_app::TouchPhase::Move: return "move";
          case tdl_app::TouchPhase::Up: return "up";
          case tdl_app::TouchPhase::Cancel: return "cancel";
        }
        return "cancel";
      })
      .def_ro("x", &tdl_app::TouchEvent::x)
      .def_ro("y", &tdl_app::TouchEvent::y)
      .def_ro("pressure", &tdl_app::TouchEvent::pressure)
      .def_ro("tracking_id", &tdl_app::TouchEvent::tracking_id)
      .def_ro("timestamp_us", &tdl_app::TouchEvent::timestamp_us);

  nb::class_<tdl_app::TouchInput>(m, "Touch",
      "Linux evdev touchscreen reader. Opens on construction and closes "
      "automatically on destruction; read() returns None on timeout.")
      .def("__init__", [](tdl_app::TouchInput *self,
                            const std::string &device, int rotation) {
        tdl_app::TouchInput::Config config;
        config.device = device;
        switch (rotation) {
          case 0:
            config.rotation = tdl_app::TouchRotation::Deg0;
            break;
          case 90:
            config.rotation = tdl_app::TouchRotation::Deg90;
            break;
          case 180:
            config.rotation = tdl_app::TouchRotation::Deg180;
            break;
          case 270:
            config.rotation = tdl_app::TouchRotation::Deg270;
            break;
          default:
            raise("touch rotation must be 0, 90, 180, or 270");
        }
        new (self) tdl_app::TouchInput(config);
        std::string error;
        if (!self->open(&error)) {
          raise("touch open failed: " + error);
        }
      }, nb::arg("device") = "/dev/input/event0",
         nb::arg("rotation") = 0)
      .def("read", [](tdl_app::TouchInput &self, int timeout_ms) -> nb::object {
        tdl_app::TouchEvent event;
        std::string error;
        bool received = false;
        {
          nb::gil_scoped_release guard;
          received = self.read(&event, timeout_ms, &error);
        }
        if (!received) {
          if (error.empty()) return nb::none();
          raise("touch read failed: " + error);
        }
        return nb::cast(event);
      }, nb::arg("timeout_ms") = 0,
         "Read one event; return None when timeout_ms elapses.");

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

  m.def("rgn_destroy",
        [](int handle, int group, int channel) {
          std::string error;
          if (!tdl_app::ensureMmfRuntimeInitialized(&error)) {
            raise("MMF runtime init failed: " + error);
          }
          if (group >= 0) {
            // Leftover regions may still be attached; detach is best effort.
            MMF_CHN_S chn = makeMmfChannel(CVI_ID_VPSS, group, channel);
            CVI_RGN_DetachFromChn(static_cast<RGN_HANDLE>(handle), &chn);
          }
          return CVI_RGN_Destroy(static_cast<RGN_HANDLE>(handle)) ==
                 CVI_SUCCESS;
        },
        nb::arg("handle"), nb::arg("group") = -1, nb::arg("channel") = 0,
        "Force-destroy an RGN handle regardless of which process created it "
        "(cleanup for leftovers after a killed process; OsdRegion.destroy() "
        "only destroys handles created by this object). Optionally detach "
        "from the given VPSS group/channel first (best effort). Returns True "
        "when an existing region was destroyed, False when the handle did "
        "not exist.");

#ifdef TDL_PY_WITH_NPU
  // --- NPU inference: result structures ------------------------------------
  nb::class_<tdl_app::Point>(m, "Point",
      "One keypoint/landmark in source-frame coordinates.")
      .def_ro("x", &tdl_app::Point::x)
      .def_ro("y", &tdl_app::Point::y)
      .def_ro("score", &tdl_app::Point::score)
      .def("__repr__", [](const tdl_app::Point &self) {
        return "<tdl_py.Point x=" + std::to_string(self.x) +
               " y=" + std::to_string(self.y) +
               " score=" + std::to_string(self.score) + ">";
      });

  nb::class_<tdl_app::Box>(m, "Box",
      "Detected box: corners (x1,y1,x2,y2), score, class_id. Face detectors "
      "(SCRFD) also fill landmarks with 5 facial keypoints.")
      .def_ro("x1", &tdl_app::Box::x1)
      .def_ro("y1", &tdl_app::Box::y1)
      .def_ro("x2", &tdl_app::Box::x2)
      .def_ro("y2", &tdl_app::Box::y2)
      .def_ro("score", &tdl_app::Box::score)
      .def_ro("class_id", &tdl_app::Box::class_id)
      .def_ro("landmarks", &tdl_app::Box::landmarks,
              "Per-face landmarks when the detector provides them "
              "(SCRFD: 5 points - eyes, nose, mouth corners), "
              "in frame coordinates like the box itself")
      .def_ro("landmarks", &tdl_app::Box::landmarks)
      .def_prop_ro("width", &tdl_app::Box::width)
      .def_prop_ro("height", &tdl_app::Box::height)
      .def("__repr__", [](const tdl_app::Box &self) {
        return "<tdl_py.Box (" + std::to_string(self.x1) + "," +
               std::to_string(self.y1) + ")-(" + std::to_string(self.x2) +
               "," + std::to_string(self.y2) +
               " score=" + std::to_string(self.score) +
               " class=" + std::to_string(self.class_id) + ">";
      });

  nb::class_<jyd_tracker::Track>(m, "Track",
      "A ByteTrack result in the coordinate system supplied to update().")
      .def_ro("id", &jyd_tracker::Track::id)
      .def_prop_ro("box", &trackerBox)
      .def_ro("age", &jyd_tracker::Track::age)
      .def_ro("missed", &jyd_tracker::Track::missed)
      .def_ro("previous_center_x", &jyd_tracker::Track::previous_center_x)
      .def_prop_ro("center_x", [](const jyd_tracker::Track &self) {
        return (self.box.x1 + self.box.x2) * 0.5f;
      })
      .def_prop_ro("center_y", [](const jyd_tracker::Track &self) {
        return (self.box.y1 + self.box.y2) * 0.5f;
      });

  nb::class_<jyd_tracker::ByteTracker>(m, "MultiObjectTracker",
      "ByteTrack-style multi-object tracker. update() accepts any Python "
      "box objects with x1/y1/x2/y2/score/class_id attributes.")
      .def("__init__", [](jyd_tracker::ByteTracker *self, float high_score,
                            float low_score, float iou_threshold,
                            int max_missed) {
        jyd_tracker::ByteTracker::Config config;
        config.high_score = high_score;
        config.low_score = low_score;
        config.iou_threshold = iou_threshold;
        config.max_missed = max_missed;
        if (config.low_score < 0.0f || config.high_score < config.low_score ||
            config.high_score > 1.0f || config.iou_threshold <= 0.0f ||
            config.iou_threshold > 1.0f || config.max_missed < 0) {
          raise("invalid ByteTrack thresholds or max_missed");
        }
        new (self) jyd_tracker::ByteTracker(config);
      }, nb::arg("high_score") = 0.45f, nb::arg("low_score") = 0.15f,
         nb::arg("iou_threshold") = 0.30f, nb::arg("max_missed") = 30)
      .def("update", [](jyd_tracker::ByteTracker &self, nb::iterable boxes) {
        std::vector<jyd_tracker::Detection> detections;
        for (nb::handle box : boxes) {
          detections.push_back(trackerDetectionFromPython(box));
        }
        return self.update(detections);
      }, nb::arg("boxes"), "Update tracker with detection boxes.")
      .def("reset", &jyd_tracker::ByteTracker::reset);

  nb::class_<tdl_app::SingleObjectTrackingResult>(m,
      "SingleObjectTrackingResult",
      "One FearTrack result in source-frame coordinates.")
      .def_ro("box", &tdl_app::SingleObjectTrackingResult::box)
      .def_ro("confidence", &tdl_app::SingleObjectTrackingResult::confidence)
      .def_ro("tracked", &tdl_app::SingleObjectTrackingResult::tracked)
      .def_ro("response_x", &tdl_app::SingleObjectTrackingResult::response_x)
      .def_ro("response_y", &tdl_app::SingleObjectTrackingResult::response_y)
      .def_ro("preprocess_ms", &tdl_app::SingleObjectTrackingResult::preprocess_ms)
      .def_ro("inference_ms", &tdl_app::SingleObjectTrackingResult::inference_ms)
      .def_ro("output_copy_ms", &tdl_app::SingleObjectTrackingResult::output_copy_ms)
      .def_ro("postprocess_ms", &tdl_app::SingleObjectTrackingResult::postprocess_ms)
      .def_ro("total_ms", &tdl_app::SingleObjectTrackingResult::total_ms);

  nb::class_<tdl_app::SingleObjectTracker>(m, "SingleObjectTracker",
      "FearTrack visual single-object tracker. Initialize from a user ROI, "
      "then track each live frame without a detector.")
      .def(nb::init<>())
      .def("load", [](tdl_app::SingleObjectTracker &self,
                      const std::string &model_spec,
                      const std::string &firmware) {
        std::string error;
        bool ok = false;
        {
          nb::gil_scoped_release guard;
          ok = self.load(model_spec, firmware, &error);
        }
        if (!ok) raise("single object tracker load failed: " + error);
      }, nb::arg("model_spec"), nb::arg("firmware") = "")
      .def("initialize", [](tdl_app::SingleObjectTracker &self,
                            PyFrame &frame, nb::handle target) {
        frame.requireValid();
        const tdl_app::Frame sdk_frame = sdkFrameFrom(frame);
        const tdl_app::Box box = boxFromPython(target);
        std::string error;
        bool ok = false;
        {
          nb::gil_scoped_release guard;
          ok = self.initializeFrame(sdk_frame, box, &error);
        }
        if (!ok) raise("single object tracker initialize failed: " + error);
      }, nb::arg("frame"), nb::arg("target"),
         "Set the template target from one live frame.")
      .def("track", [](tdl_app::SingleObjectTracker &self, PyFrame &frame) {
        frame.requireValid();
        const tdl_app::Frame sdk_frame = sdkFrameFrom(frame);
        tdl_app::SingleObjectTrackingResult result;
        std::string error;
        bool ok = false;
        {
          nb::gil_scoped_release guard;
          ok = self.runFrame(sdk_frame, &result, &error);
        }
        if (!ok) raise("single object tracker track failed: " + error);
        return result;
      }, nb::arg("frame"), "Track one live frame.")
      .def("reset", &tdl_app::SingleObjectTracker::reset)
      .def_prop_ro("initialized", &tdl_app::SingleObjectTracker::initialized)
      .def_prop_ro("ready", &tdl_app::SingleObjectTracker::ready)
      .def_prop_ro("current_box", &tdl_app::SingleObjectTracker::currentBox);

  nb::class_<tdl_app::ClassificationItem>(m, "ClassificationItem",
      "One top-k classification entry: class_id + score.")
      .def_ro("class_id", &tdl_app::ClassificationItem::class_id)
      .def_ro("score", &tdl_app::ClassificationItem::score)
      .def("__repr__", [](const tdl_app::ClassificationItem &self) {
        return "<tdl_py.ClassificationItem class=" +
               std::to_string(self.class_id) +
               " score=" + std::to_string(self.score) + ">";
      });

  nb::class_<tdl_app::Attribute>(m, "Attribute",
      "Named scalar attribute (e.g. \"ocr_text:<text>\" per OCR box).")
      .def_ro("name", &tdl_app::Attribute::name)
      .def_ro("value", &tdl_app::Attribute::value);

  nb::class_<tdl_app::AlgorithmResult>(m, "AlgorithmResult",
      "Generic inference result. Detection fills boxes; classification fills "
      "classes; OCR fills boxes + text (+ ocr_text attributes).")
      .def_ro("classes", &tdl_app::AlgorithmResult::classes)
      .def_ro("boxes", &tdl_app::AlgorithmResult::boxes)
      .def_ro("points", &tdl_app::AlgorithmResult::points)
      .def_ro("attributes", &tdl_app::AlgorithmResult::attributes)
      .def_ro("labels", &tdl_app::AlgorithmResult::labels)
      .def_ro("text", &tdl_app::AlgorithmResult::text)
      .def("label_of",
           [](const tdl_app::AlgorithmResult &self, int class_id) {
             if (class_id >= 0 &&
                 class_id < static_cast<int>(self.labels.size())) {
               return self.labels[static_cast<std::size_t>(class_id)];
             }
             return std::to_string(class_id);
           },
           nb::arg("class_id"),
           "Label string for class_id, or the id itself when out of range");

  nb::class_<tdl_app::KeypointResult>(m, "KeypointResult",
      "Pose/keypoint result; points are in source-frame coordinates.")
      .def_ro("width", &tdl_app::KeypointResult::width)
      .def_ro("height", &tdl_app::KeypointResult::height)
      .def_ro("box", &tdl_app::KeypointResult::box,
              "Detected person box when the underlying keypoint model provides one.")
      .def_ro("points", &tdl_app::KeypointResult::points);

  nb::class_<tdl_app::HandGestureResult>(m, "HandGestureResult",
      "One detected hand and its 21 keypoints. Coordinates are in the "
      "source-frame coordinate system.")
      .def_ro("box", &tdl_app::HandGestureResult::box)
      .def_ro("keypoints", &tdl_app::HandGestureResult::keypoints)
      .def_ro("score", &tdl_app::HandGestureResult::score)
      .def_prop_ro("label", [](const tdl_app::HandGestureResult &self) {
        return std::string(tdl_app::handGestureName(self.gesture));
      })
      .def_prop_ro("gesture", [](const tdl_app::HandGestureResult &self) {
        return std::string(tdl_app::handGestureName(self.gesture));
      });

  nb::class_<tdl_app::SelfLearningClassResult>(m, "SelfLearningClassResult",
      "One self-learning class ranked by cosine similarity.")
      .def_ro("label", &tdl_app::SelfLearningClassResult::label)
      .def_ro("score", &tdl_app::SelfLearningClassResult::score)
      .def_ro("sample_count", &tdl_app::SelfLearningClassResult::sample_count);

  nb::class_<tdl_app::SelfLearningClassificationProfile>(
      m, "SelfLearningClassificationProfile")
      .def_ro("feature_ms", &tdl_app::SelfLearningClassificationProfile::feature_ms)
      .def_ro("match_ms", &tdl_app::SelfLearningClassificationProfile::match_ms)
      .def_ro("total_ms", &tdl_app::SelfLearningClassificationProfile::total_ms);

  nb::class_<tdl_app::SelfLearningClassificationResult>(
      m, "SelfLearningClassificationResult",
      "Top-k self-learning classification results.")
      .def_ro("classes", &tdl_app::SelfLearningClassificationResult::classes)
      .def_ro("feature_dim", &tdl_app::SelfLearningClassificationResult::feature_dim)
      .def_prop_ro("empty", &tdl_app::SelfLearningClassificationResult::empty);

  nb::class_<tdl_app::InstanceSegment>(m, "InstanceSegment",
      "One segmented instance: detection box + polygon outline.")
      .def_ro("box", &tdl_app::InstanceSegment::box)
      .def_ro("outline", &tdl_app::InstanceSegment::outline);

  nb::class_<tdl_app::InstanceSegmentationResult>(m,
      "InstanceSegmentationResult")
      .def_ro("width", &tdl_app::InstanceSegmentationResult::width)
      .def_ro("height", &tdl_app::InstanceSegmentationResult::height)
      .def_ro("instances", &tdl_app::InstanceSegmentationResult::instances);

  // --- NPU inference: model wrappers ----------------------------------------
  nb::class_<tdl_app::Detector>(m, "Detector",
      "Object detector (YOLOv5/v8 etc., .mud model-spec). detect() runs on a "
      "VpssCamera frame; detect_image() on a file path.")
      .def(nb::init<>())
      .def("load",
           [](tdl_app::Detector &self, const std::string &model_spec,
              const std::string &firmware) {
             loadModel(self, "detector", model_spec, firmware);
           },
           nb::arg("model_spec"), nb::arg("firmware") = "",
           "Load a .mud model-spec (GIL released)")
      .def("detect",
           [](tdl_app::Detector &self, PyFrame &frame, float threshold,
              float iou_threshold) {
             const tdl_app::InferOptions options =
                 tdl_app::InferOptions::detection(threshold, iou_threshold);
             const tdl_app::Frame sdk_frame = sdkFrameFrom(frame);
             tdl_app::AlgorithmResult result;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.runFrame(sdk_frame, options, &result, &error);
             }
             if (!ok) {
               raise("detector inference failed: " + error);
             }
             return result;
           },
           nb::arg("frame"), nb::arg("threshold") = 0.5f,
           nb::arg("iou_threshold") = 0.45f,
           "Detect objects in a camera frame (zero-copy, GIL released)")
      .def("detect_image",
           [](tdl_app::Detector &self, const std::string &path,
              float threshold, float iou_threshold) {
             const tdl_app::InferOptions options =
                 tdl_app::InferOptions::detection(threshold, iou_threshold);
             tdl_app::AlgorithmResult result;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.run(path, options, &result, &error);
             }
             if (!ok) {
               raise("detector inference failed: " + error);
             }
             return result;
           },
           nb::arg("path"), nb::arg("threshold") = 0.5f,
           nb::arg("iou_threshold") = 0.45f,
           "Detect objects in an image file (GIL released)")
      .def("reset", &tdl_app::Detector::reset, "Unload the model")
      .def_prop_ro("initialized", &tdl_app::Detector::initialized)
      .def_prop_ro("model_type", &tdl_app::Detector::modelType);

  nb::class_<tdl_app::Classifier>(m, "Classifier",
      "Image classifier. classify() runs on a VpssCamera frame; "
      "classify_image() on a file path.")
      .def(nb::init<>())
      .def("load",
           [](tdl_app::Classifier &self, const std::string &model_spec,
              const std::string &firmware) {
             loadModel(self, "classifier", model_spec, firmware);
           },
           nb::arg("model_spec"), nb::arg("firmware") = "",
           "Load a .mud model-spec (GIL released)")
      .def("classify",
           [](tdl_app::Classifier &self, PyFrame &frame, float threshold,
              int top_k) {
             const tdl_app::InferOptions options =
                 tdl_app::InferOptions::classification(threshold, top_k);
             const tdl_app::Frame sdk_frame = sdkFrameFrom(frame);
             tdl_app::AlgorithmResult result;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.runFrame(sdk_frame, options, &result, &error);
             }
             if (!ok) {
               raise("classifier inference failed: " + error);
             }
             return result;
           },
           nb::arg("frame"), nb::arg("threshold") = 0.0f, nb::arg("top_k") = 5,
           "Classify a camera frame (zero-copy, GIL released)")
      .def("classify_image",
           [](tdl_app::Classifier &self, const std::string &path,
              float threshold, int top_k) {
             const tdl_app::InferOptions options =
                 tdl_app::InferOptions::classification(threshold, top_k);
             tdl_app::AlgorithmResult result;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.run(path, options, &result, &error);
             }
             if (!ok) {
               raise("classifier inference failed: " + error);
             }
             return result;
           },
           nb::arg("path"), nb::arg("threshold") = 0.0f, nb::arg("top_k") = 5,
           "Classify an image file (GIL released)")
      .def("reset", &tdl_app::Classifier::reset, "Unload the model")
      .def_prop_ro("initialized", &tdl_app::Classifier::initialized)
      .def_prop_ro("model_type", &tdl_app::Classifier::modelType);

  nb::class_<tdl_app::KeypointDetector>(m, "KeypointDetector",
      "Pose/keypoint estimator (YOLOv8-pose person17, hand, ...). estimate() "
      "runs on a VpssCamera frame; estimate_image() on a file path.")
      .def(nb::init<>())
      .def("load",
           [](tdl_app::KeypointDetector &self, const std::string &model_spec,
              const std::string &firmware) {
             loadModel(self, "keypoint detector", model_spec, firmware);
           },
           nb::arg("model_spec"), nb::arg("firmware") = "",
           "Load a .mud model-spec (GIL released)")
      .def("estimate",
           [](tdl_app::KeypointDetector &self, PyFrame &frame) {
             const tdl_app::Frame sdk_frame = sdkFrameFrom(frame);
             tdl_app::KeypointResult result;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.runFrame(sdk_frame, &result, &error);
             }
             if (!ok) {
               raise("keypoint inference failed: " + error);
             }
             return result;
           },
           nb::arg("frame"),
           "Estimate keypoints on a camera frame (zero-copy, GIL released)")
      .def("estimate_image",
           [](tdl_app::KeypointDetector &self, const std::string &path) {
             tdl_app::KeypointResult result;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.run(path, &result, &error);
             }
             if (!ok) {
               raise("keypoint inference failed: " + error);
             }
             return result;
           },
           nb::arg("path"), "Estimate keypoints on an image file (GIL released)")
      .def("reset", &tdl_app::KeypointDetector::reset, "Unload the model")
      .def_prop_ro("initialized", &tdl_app::KeypointDetector::initialized)
      .def_prop_ro("model_type", &tdl_app::KeypointDetector::modelType);

  nb::class_<tdl_app::PoseClassificationResult>(m, "PoseClassificationResult",
      "One body-pose classification result. Keypoints remain in source-frame "
      "coordinates.")
      .def_ro("keypoints", &tdl_app::PoseClassificationResult::keypoints)
      .def_ro("confidence", &tdl_app::PoseClassificationResult::confidence)
      .def_ro("history_size", &tdl_app::PoseClassificationResult::history_size)
      .def_prop_ro("label", [](const tdl_app::PoseClassificationResult &self) {
        return std::string(tdl_app::humanPoseClassName(self.pose));
      })
      .def_prop_ro("raw_label",
                   [](const tdl_app::PoseClassificationResult &self) {
        return std::string(tdl_app::humanPoseClassName(self.raw_pose));
      })
      .def_prop_ro("keypoint_ms",
                   [](const tdl_app::PoseClassificationResult &self) {
        return self.profile.keypoint_ms;
      })
      .def_prop_ro("total_ms", [](const tdl_app::PoseClassificationResult &self) {
        return self.profile.total_ms;
      });

  nb::class_<tdl_app::PoseClassifier>(m, "PoseClassifier",
      "Online body-pose classifier: COCO-17 keypoints plus temporal "
      "geometric rules. classify() accepts a VpssCamera frame.")
      .def(nb::init<>())
      .def("__init__",
           [](tdl_app::PoseClassifier *self, const std::string &model_spec,
              float keypoint_threshold, float ema_alpha, int smooth_frames,
              const std::string &firmware) {
             new (self) tdl_app::PoseClassifier();
             tdl_app::PoseClassifier::Config config;
             config.keypoint =
                 tdl_app::ModelSessionConfig::fromSpec(model_spec, firmware);
             config.keypoint_threshold = keypoint_threshold;
             config.coordinate_ema_alpha = ema_alpha;
             config.label_smooth_frames = smooth_frames;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self->load(config, &error);
             }
             if (!ok) raise("pose classifier load failed: " + error);
           },
           nb::arg("model"), nb::arg("keypoint_threshold") = 0.05f,
           nb::arg("ema_alpha") = 0.65f, nb::arg("smooth_frames") = 5,
           nb::arg("firmware") = "",
           "Create and load a COCO-17 pose classifier.")
      .def("load",
           [](tdl_app::PoseClassifier &self, const std::string &model_spec,
              float keypoint_threshold, float ema_alpha, int smooth_frames,
              const std::string &firmware) {
             tdl_app::PoseClassifier::Config config;
             config.keypoint =
                 tdl_app::ModelSessionConfig::fromSpec(model_spec, firmware);
             config.keypoint_threshold = keypoint_threshold;
             config.coordinate_ema_alpha = ema_alpha;
             config.label_smooth_frames = smooth_frames;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.load(config, &error);
             }
             if (!ok) raise("pose classifier load failed: " + error);
           },
           nb::arg("model"), nb::arg("keypoint_threshold") = 0.05f,
           nb::arg("ema_alpha") = 0.65f, nb::arg("smooth_frames") = 5,
           nb::arg("firmware") = "", "Load or replace the pose model.")
      .def("classify", [](tdl_app::PoseClassifier &self, PyFrame &frame) {
        const tdl_app::Frame sdk_frame = sdkFrameFrom(frame);
        tdl_app::PoseClassificationResult result;
        std::string error;
        bool ok = false;
        {
          nb::gil_scoped_release guard;
          ok = self.runFrame(sdk_frame, &result, &error);
        }
        if (!ok) raise("pose classification failed: " + error);
        return result;
      }, nb::arg("frame"),
      "Classify one live frame (zero-copy camera input, GIL released).")
      .def("classify_image",
           [](tdl_app::PoseClassifier &self, const std::string &path) {
             tdl_app::PoseClassificationResult result;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.run(path, &result, &error);
             }
             if (!ok) raise("pose classification failed: " + error);
             return result;
           },
           nb::arg("path"), "Classify one image file (GIL released).")
      .def("reset_smoothing", &tdl_app::PoseClassifier::resetSmoothing,
           "Clear the temporal keypoint and label history.")
      .def("reset", &tdl_app::PoseClassifier::reset, "Unload the model.")
      .def_prop_ro("initialized", &tdl_app::PoseClassifier::initialized);
  m.attr("BodyPoseClassifier") = m.attr("PoseClassifier");

  nb::class_<tdl_app::HandGestureRecognizer>(m, "HandGestureRecognizer",
      "Online hand gesture recognition: hand detection, 21 keypoints, and "
      "vendor keypoint-model gesture classification.")
      .def(nb::init<>())
      .def("load",
           [](tdl_app::HandGestureRecognizer &self,
              const std::string &detector_model,
              const std::string &keypoint_model, float hand_threshold,
              float iou_threshold, float roi_expand_ratio, int max_hands,
              const std::string &firmware,
              const std::string &gesture_classifier_model) {
             tdl_app::HandGestureRecognizer::Config config;
             config.detector_model_spec = detector_model;
             config.keypoint_model_spec = keypoint_model;
             config.hand_threshold = hand_threshold;
             config.iou_threshold = iou_threshold;
             config.roi_expand_ratio = roi_expand_ratio;
             config.max_hands = max_hands;
             config.firmware = firmware;
             config.gesture_classifier_model_spec = gesture_classifier_model;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.load(config, &error);
             }
             if (!ok) raise("hand gesture load failed: " + error);
           },
           nb::arg("detector_model"), nb::arg("keypoint_model"),
           nb::arg("hand_threshold") = 0.35f,
           nb::arg("iou_threshold") = 0.45f,
           nb::arg("roi_expand_ratio") = 0.25f,
           nb::arg("max_hands") = 2, nb::arg("firmware") = "",
           nb::arg("gesture_classifier_model") = "",
           "Load hand detection, 21 keypoints, and gesture classification."
           " An empty gesture_classifier_model selects keypoint_hand_gesture.mud"
           " beside keypoint_model.")
      .def("__init__",
           [](tdl_app::HandGestureRecognizer *self,
              const std::string &detector_model,
              const std::string &keypoint_model, float hand_threshold,
              int max_hands, const std::string &firmware,
              const std::string &gesture_classifier_model) {
             new (self) tdl_app::HandGestureRecognizer();
             tdl_app::HandGestureRecognizer::Config config;
             config.detector_model_spec = detector_model;
             config.keypoint_model_spec = keypoint_model;
             config.hand_threshold = hand_threshold;
             config.max_hands = max_hands;
             config.firmware = firmware;
             config.gesture_classifier_model_spec = gesture_classifier_model;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self->load(config, &error);
             }
             if (!ok) raise("hand gesture load failed: " + error);
           },
           nb::arg("detector_model"), nb::arg("keypoint_model"),
           nb::arg("hand_threshold") = 0.35f, nb::arg("max_hands") = 2,
           nb::arg("firmware") = "", nb::arg("gesture_classifier_model") = "",
           "Create and load an online hand gesture recognizer.")
      .def("recognize",
           [](tdl_app::HandGestureRecognizer &self, PyFrame &frame) {
             const tdl_app::Frame sdk_frame = sdkFrameFrom(frame);
             std::vector<tdl_app::HandGestureResult> results;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.recognizeFrame(sdk_frame, &results, &error);
             }
             if (!ok) raise("hand gesture inference failed: " + error);
             return results;
           },
           nb::arg("frame"),
           "Recognize gestures on one live camera frame (GIL released).")
      .def("reset", [](tdl_app::HandGestureRecognizer &self) {
        // HandGestureRecognizer has no public reset; replacing it is the
        // supported way to release model resources from Python.
        self.~HandGestureRecognizer();
        new (&self) tdl_app::HandGestureRecognizer();
      }, "Unload the hand gesture models.")
      .def_prop_ro("initialized", &tdl_app::HandGestureRecognizer::initialized);

  nb::class_<tdl_app::SelfLearningClassifier>(m, "SelfLearningClassifier",
      "Sipeed/Maix-style self-learning image classifier. Samples are stored "
      "as feature vectors and matched with cosine similarity.")
      .def(nb::init<>())
      .def("__init__",
           [](tdl_app::SelfLearningClassifier *self,
              const std::string &model_spec, const std::string &firmware) {
             new (self) tdl_app::SelfLearningClassifier();
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self->load(model_spec, firmware, &error);
             }
             if (!ok) raise("self-learning model load failed: " + error);
           },
           nb::arg("model_spec"), nb::arg("firmware") = "",
           "Create and load a self-learning feature model.")
      .def("load",
           [](tdl_app::SelfLearningClassifier &self,
              const std::string &model_spec, const std::string &firmware) {
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.load(model_spec, firmware, &error);
             }
             if (!ok) raise("self-learning model load failed: " + error);
           },
           nb::arg("model_spec"), nb::arg("firmware") = "",
           "Load the feature model.")
      .def("add_sample",
           [](tdl_app::SelfLearningClassifier &self, const std::string &label,
              const std::string &image_path) {
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.addSample(label, image_path, &error);
             }
             if (!ok) raise("self-learning add_sample failed: " + error);
           },
           nb::arg("label"), nb::arg("image"),
           "Extract and add one labeled image sample.")
      .def("add_frame",
           [](tdl_app::SelfLearningClassifier &self, const std::string &label,
              PyFrame &frame) {
             const tdl_app::Frame sdk_frame = sdkFrameFrom(frame);
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.addFrame(label, sdk_frame, &error);
             }
             if (!ok) raise("self-learning add_frame failed: " + error);
           },
           nb::arg("label"), nb::arg("frame"),
           "Extract and add one labeled live camera frame sample.")
      .def("add_frame_crop",
           [](tdl_app::SelfLearningClassifier &self, const std::string &label,
              PyFrame &frame, nb::handle roi) {
             const tdl_app::Frame sdk_frame = sdkFrameFrom(frame);
             const tdl_app::Box sdk_roi = boxFromPython(roi);
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.addFrameCrop(label, sdk_frame, sdk_roi, &error);
             }
             if (!ok) raise("self-learning add_frame_crop failed: " + error);
           }, nb::arg("label"), nb::arg("frame"), nb::arg("roi"),
           "Extract and add one labeled hardware-cropped frame sample.")
      .def("classify",
           [](tdl_app::SelfLearningClassifier &self, PyFrame &frame, int top_k) {
             const tdl_app::Frame sdk_frame = sdkFrameFrom(frame);
             tdl_app::SelfLearningClassificationResult result;
             tdl_app::SelfLearningClassificationProfile profile;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.classifyFrame(sdk_frame, top_k, &result, &profile,
                                       &error);
             }
             if (!ok) raise("self-learning classify failed: " + error);
             return result;
           },
           nb::arg("frame"), nb::arg("top_k") = 1,
           "Classify one live camera frame.")
      .def("classify_crop",
           [](tdl_app::SelfLearningClassifier &self, PyFrame &frame,
              nb::handle roi, int top_k) {
             const tdl_app::Frame sdk_frame = sdkFrameFrom(frame);
             const tdl_app::Box sdk_roi = boxFromPython(roi);
             tdl_app::SelfLearningClassificationResult result;
             tdl_app::SelfLearningClassificationProfile profile;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.classifyFrameCrop(sdk_frame, sdk_roi, top_k, &result,
                                           &profile, &error);
             }
             if (!ok) raise("self-learning classify_crop failed: " + error);
             return result;
           }, nb::arg("frame"), nb::arg("roi"), nb::arg("top_k") = 1,
           "Classify one hardware-cropped ROI from a live camera frame.")
      .def("classify_image",
           [](tdl_app::SelfLearningClassifier &self, const std::string &path,
              int top_k) {
             tdl_app::SelfLearningClassificationResult result;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.classify(path, top_k, &result, &error);
             }
             if (!ok) raise("self-learning classify failed: " + error);
             return result;
           },
           nb::arg("image"), nb::arg("top_k") = 1,
           "Classify one image file.")
      .def("save_bank",
           [](const tdl_app::SelfLearningClassifier &self,
              const std::string &path) {
             std::string error;
             if (!self.saveBank(path, &error)) {
               raise("self-learning save_bank failed: " + error);
             }
           }, nb::arg("path"))
      .def("load_bank",
           [](tdl_app::SelfLearningClassifier &self, const std::string &path) {
             std::string error;
             if (!self.loadBank(path, &error)) {
               raise("self-learning load_bank failed: " + error);
             }
           }, nb::arg("path"))
      .def("save", [](const tdl_app::SelfLearningClassifier &self,
                      const std::string &path) {
             std::string error;
             if (!self.saveBank(path, &error)) {
               raise("self-learning save failed: " + error);
             }
           }, nb::arg("path"), "Maix-compatible alias for save_bank().")
      .def("clear", &tdl_app::SelfLearningClassifier::clearBank)
      .def("learn", [](tdl_app::SelfLearningClassifier &) {},
           "Maix-compatible no-op: prototypes are updated as samples are added.")
      .def_prop_ro("initialized", &tdl_app::SelfLearningClassifier::initialized)
      .def_prop_ro("feature_dim", &tdl_app::SelfLearningClassifier::featureDim)
      .def_prop_ro("sample_count", &tdl_app::SelfLearningClassifier::sampleCount)
      .def_prop_ro("class_count", &tdl_app::SelfLearningClassifier::classCount);

  nb::class_<tdl_app::InstanceSegmenter>(m, "InstanceSegmenter",
      "Instance segmentation (YOLOv8-seg etc.). segment() runs on a "
      "VpssCamera frame; segment_image() on a file path. Each instance "
      "exposes its box and polygon outline (per-pixel masks are not "
      "exported).")
      .def(nb::init<>())
      .def("load",
           [](tdl_app::InstanceSegmenter &self, const std::string &model_spec,
              const std::string &firmware) {
             loadModel(self, "instance segmenter", model_spec, firmware);
           },
           nb::arg("model_spec"), nb::arg("firmware") = "",
           "Load a .mud model-spec (GIL released)")
      .def("segment",
           [](tdl_app::InstanceSegmenter &self, PyFrame &frame) {
             const tdl_app::Frame sdk_frame = sdkFrameFrom(frame);
             tdl_app::InstanceSegmentationResult result;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.runFrame(sdk_frame, &result, &error);
             }
             if (!ok) {
               raise("instance segmentation failed: " + error);
             }
             return result;
           },
           nb::arg("frame"),
           "Segment a camera frame (zero-copy, GIL released)")
      .def("segment_image",
           [](tdl_app::InstanceSegmenter &self, const std::string &path) {
             tdl_app::InstanceSegmentationResult result;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.run(path, &result, &error);
             }
             if (!ok) {
               raise("instance segmentation failed: " + error);
             }
             return result;
           },
           nb::arg("path"), "Segment an image file (GIL released)")
      .def("reset", &tdl_app::InstanceSegmenter::reset, "Unload the model")
      .def_prop_ro("initialized", &tdl_app::InstanceSegmenter::initialized)
      .def_prop_ro("model_type", &tdl_app::InstanceSegmenter::modelType);

  nb::class_<tdl_app::PlateRecognizer>(m, "PlateRecognizer",
      "OCR / plate recognizer (PP-OCR, LPR, ...). recognize() runs on a "
      "VpssCamera frame; recognize_image() on a file path.")
      .def(nb::init<>())
      .def("load",
           [](tdl_app::PlateRecognizer &self, const std::string &model_spec,
              const std::string &firmware) {
             loadModel(self, "plate recognizer", model_spec, firmware);
           },
           nb::arg("model_spec"), nb::arg("firmware") = "",
           "Load a .mud model-spec (GIL released)")
      .def("recognize",
           [](tdl_app::PlateRecognizer &self, PyFrame &frame,
              float threshold) {
             const tdl_app::InferOptions options =
                 tdl_app::InferOptions::withThreshold(threshold);
             const tdl_app::Frame sdk_frame = sdkFrameFrom(frame);
             tdl_app::AlgorithmResult result;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.runFrame(sdk_frame, options, &result, &error);
             }
             if (!ok) {
               raise("plate recognition failed: " + error);
             }
             return result;
           },
           nb::arg("frame"), nb::arg("threshold") = 0.5f,
           "Recognize text/plates in a camera frame (zero-copy, GIL released)")
      .def("recognize_image",
           [](tdl_app::PlateRecognizer &self, const std::string &path,
              float threshold) {
             const tdl_app::InferOptions options =
                 tdl_app::InferOptions::withThreshold(threshold);
             tdl_app::AlgorithmResult result;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.run(path, options, &result, &error);
             }
             if (!ok) {
               raise("plate recognition failed: " + error);
             }
             return result;
           },
           nb::arg("path"), nb::arg("threshold") = 0.5f,
           "Recognize text/plates in an image file (GIL released)")
      .def("reset", &tdl_app::PlateRecognizer::reset, "Unload the model")
      .def_prop_ro("initialized", &tdl_app::PlateRecognizer::initialized)
      .def_prop_ro("model_type", &tdl_app::PlateRecognizer::modelType);

  nb::class_<tdl_app::FaceEmotionResult>(m, "FaceEmotionResult",
      "One detected face with the emotion returned by the attribute model.")
      .def_ro("box", &tdl_app::FaceEmotionResult::box)
      .def_ro("emotion", &tdl_app::FaceEmotionResult::emotion)
      .def_ro("emotion_id", &tdl_app::FaceEmotionResult::emotion_id)
      .def_ro("emotion_score", &tdl_app::FaceEmotionResult::emotion_score)
      .def_ro("detection_score", &tdl_app::FaceEmotionResult::detection_score)
      .def_ro("gender", &tdl_app::FaceEmotionResult::gender)
      .def_ro("age", &tdl_app::FaceEmotionResult::age)
      .def_ro("glasses", &tdl_app::FaceEmotionResult::glasses)
      .def_ro("gender_label", &tdl_app::FaceEmotionResult::gender_label)
      .def_ro("age_years", &tdl_app::FaceEmotionResult::age_years)
      .def_ro("has_glasses", &tdl_app::FaceEmotionResult::has_glasses);

  nb::class_<tdl_app::FaceEmotionRecognizer>(m, "FaceEmotionRecognizer",
      "Online face emotion recognition: SCRFD detection plus the 7-class "
      "face attribute/emotion model.")
      .def(nb::init<>())
      .def("__init__",
           [](tdl_app::FaceEmotionRecognizer *self,
              const std::string &detector_model,
              const std::string &attribute_model, float threshold,
              int max_faces, const std::string &firmware) {
             new (self) tdl_app::FaceEmotionRecognizer();
             tdl_app::FaceEmotionRecognizer::Config config;
             config.detector_model_spec = detector_model;
             config.attribute_model_spec = attribute_model;
             config.face_threshold = threshold;
             config.max_faces = max_faces;
             config.firmware = firmware;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self->load(config, &error);
             }
             if (!ok) raise("face emotion recognizer load failed: " + error);
           },
           nb::arg("detector_model"), nb::arg("attribute_model"),
           nb::arg("threshold") = 0.35f, nb::arg("max_faces") = 3,
           nb::arg("firmware") = "",
           "Create and load the face detection and emotion models.")
      .def("load",
           [](tdl_app::FaceEmotionRecognizer &self,
              const std::string &detector_model,
              const std::string &attribute_model, float threshold,
              int max_faces, const std::string &firmware) {
             tdl_app::FaceEmotionRecognizer::Config config;
             config.detector_model_spec = detector_model;
             config.attribute_model_spec = attribute_model;
             config.face_threshold = threshold;
             config.max_faces = max_faces;
             config.firmware = firmware;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.load(config, &error);
             }
             if (!ok) raise("face emotion recognizer load failed: " + error);
           },
           nb::arg("detector_model"), nb::arg("attribute_model"),
           nb::arg("threshold") = 0.35f, nb::arg("max_faces") = 3,
           nb::arg("firmware") = "")
      .def("recognize",
           [](tdl_app::FaceEmotionRecognizer &self, PyFrame &frame) {
             const tdl_app::Frame sdk_frame = sdkFrameFrom(frame);
             std::vector<tdl_app::FaceEmotionResult> results;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.recognizeFrame(sdk_frame, &results, &error);
             }
             if (!ok) raise("face emotion recognition failed: " + error);
             return results;
           },
           nb::arg("frame"),
           "Detect faces and return their 7-class emotion results.")
      .def_prop_ro("initialized", &tdl_app::FaceEmotionRecognizer::initialized);

  nb::class_<tdl_app::FaceRecognitionResult>(m, "FaceRecognitionResult",
      "One face recognition result. Box coordinates are in frame space.")
      .def_ro("box", &tdl_app::FaceRecognitionResult::box)
      .def_ro("name", &tdl_app::FaceRecognitionResult::name)
      .def_ro("class_id", &tdl_app::FaceRecognitionResult::class_id)
      .def_ro("similarity", &tdl_app::FaceRecognitionResult::similarity)
      .def_ro("matched", &tdl_app::FaceRecognitionResult::matched)
      .def_prop_ro("x", [](const tdl_app::FaceRecognitionResult &self) {
        return self.box.x1;
      })
      .def_prop_ro("y", [](const tdl_app::FaceRecognitionResult &self) {
        return self.box.y1;
      })
      .def_prop_ro("w", [](const tdl_app::FaceRecognitionResult &self) {
        return self.box.width();
      })
      .def_prop_ro("h", [](const tdl_app::FaceRecognitionResult &self) {
        return self.box.height();
      })
      .def_prop_ro("score", [](const tdl_app::FaceRecognitionResult &self) {
        return self.similarity;
      })
      .def_prop_ro("points", [](const tdl_app::FaceRecognitionResult &self) {
        return self.box.landmarks;
      });

  nb::class_<tdl_app::FaceRecognizer>(m, "FaceRecognizer",
      "Online face recognition: SCRFD detection plus face embedding.")
      .def(nb::init<>())
      .def("__init__",
           [](tdl_app::FaceRecognizer *self, const std::string &detect_model,
              const std::string &feature_model, bool dual_buff,
              const std::string &firmware) {
             new (self) tdl_app::FaceRecognizer();
             tdl_app::FaceRecognizer::Config config;
             config.detector_model_spec = detect_model;
             config.feature_model_spec = feature_model;
             config.firmware = firmware;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self->load(config, &error);
             }
             if (!ok) raise("face recognizer load failed: " + error);
             (void) dual_buff;
           },
           nb::arg("detect_model"), nb::arg("feature_model"),
           nb::arg("dual_buff") = true, nb::arg("firmware") = "",
           "Create and load a two-model face recognizer. dual_buff is "
           "accepted for MaixPy API compatibility.")
      .def("load",
           [](tdl_app::FaceRecognizer &self,
              const std::string &detector_model,
              const std::string &feature_model, float face_threshold,
              float match_threshold, int max_faces,
              const std::string &firmware) {
             tdl_app::FaceRecognizer::Config config;
             config.detector_model_spec = detector_model;
             config.feature_model_spec = feature_model;
             config.face_threshold = face_threshold;
             config.match_threshold = match_threshold;
             config.max_faces = max_faces;
             config.firmware = firmware;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.load(config, &error);
             }
             if (!ok) raise("face recognizer load failed: " + error);
           },
           nb::arg("detector_model_spec"), nb::arg("feature_model_spec"),
           nb::arg("face_threshold") = 0.25f,
           nb::arg("match_threshold") = 0.50f,
           nb::arg("max_faces") = 3, nb::arg("firmware") = "",
           "Load SCRFD and feature models (GIL released).")
      .def("enroll",
           [](tdl_app::FaceRecognizer &self, PyFrame &frame,
              const std::string &name) {
             const tdl_app::Frame sdk_frame = sdkFrameFrom(frame);
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.enrollFrame(name, sdk_frame, &error);
             }
             if (!ok) raise("face enroll failed: " + error);
           },
           nb::arg("frame"), nb::arg("name"),
           "Enroll the largest face from one live frame.")
      .def("recognize",
           [](tdl_app::FaceRecognizer &self, PyFrame &frame, float conf_th,
              float iou_th, float match_th, bool get_feature) {
             const tdl_app::Frame sdk_frame = sdkFrameFrom(frame);
             std::vector<tdl_app::FaceRecognitionResult> results;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.recognizeFrame(sdk_frame, conf_th, iou_th, match_th,
                                        &results, &error);
             }
             if (!ok) raise("face recognition failed: " + error);
             if (!get_feature) {
               for (auto &result : results) result.feature.clear();
             }
             return results;
           },
           nb::arg("frame"), nb::arg("conf_th") = 0.5f,
           nb::arg("iou_th") = 0.45f, nb::arg("match_th") = 0.85f,
           nb::arg("get_feature") = false,
           "Recognize faces in one live frame (MaixPy-compatible controls).")
      .def("add_face",
           [](tdl_app::FaceRecognizer &self,
              const tdl_app::FaceRecognitionResult &face,
              const std::string &label) {
             std::string error;
             if (!self.addFace(face, label, &error)) {
               raise("add_face failed: " + error);
             }
           },
           nb::arg("face"), nb::arg("label"),
           "Add one result returned with get_feature=True to the face library.")
      .def("save_faces",
           [](const tdl_app::FaceRecognizer &self, const std::string &path) {
             std::string error;
             if (!self.saveFaces(path, &error)) {
               raise("save_faces failed: " + error);
             }
           }, nb::arg("path"), "Save the registered face library.")
      .def("load_faces",
           [](tdl_app::FaceRecognizer &self, const std::string &path) {
             std::string error;
             if (!self.loadFaces(path, &error)) {
               raise("load_faces failed: " + error);
             }
           }, nb::arg("path"), "Load a registered face library.")
      .def("remove", &tdl_app::FaceRecognizer::remove, nb::arg("name"))
      .def("clear", &tdl_app::FaceRecognizer::clear)
      .def("names", &tdl_app::FaceRecognizer::names)
      .def_prop_ro("labels", [](const tdl_app::FaceRecognizer &self) {
        std::vector<std::string> labels{"unknown"};
        const std::vector<std::string> names = self.names();
        labels.insert(labels.end(), names.begin(), names.end());
        return labels;
      })

      .def_prop_ro("initialized", &tdl_app::FaceRecognizer::initialized);
  nb::class_<PyFaceDenseLandmark>(m, "FaceDenseLandmark",
      "Dense facial landmark estimator (face_dense_real.mud). Second-stage "
      "model: detect faces first (Detector + scrfd_real.mud or "
      "yolov8_face_real.mud), then estimate() landmarks per face box on a "
      "VpssCamera frame. Returned points are in frame coordinates.")
      .def(nb::init<>())
      .def("load",
           [](PyFaceDenseLandmark &self, const std::string &model_spec,
              const std::string &firmware) {
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.load(model_spec, firmware, &error);
             }
             if (!ok) {
               raise("face dense landmark load failed: " + error);
             }
           },
           nb::arg("model_spec"), nb::arg("firmware") = "",
           "Load a .mud model-spec (GIL released)")
      .def("estimate",
           [](PyFaceDenseLandmark &self, PyFrame &frame,
              const tdl_app::Box &box, float expand) {
             frame.requireValid();
             std::vector<tdl_app::Point> points;
             std::string error;
             bool ok = false;
             {
               nb::gil_scoped_release guard;
               ok = self.estimate(frame.nativeInfo(), box, expand, &points,
                                  &error);
             }
             if (!ok) {
               raise("face dense landmark inference failed: " + error);
             }
             return points;
           },
           nb::arg("frame"), nb::arg("box"), nb::arg("expand") = 0.0f,
           "Estimate dense landmarks for one face box on a camera frame "
           "(zero-copy VPSS ROI, GIL released); expand enlarges the square "
           "ROI, e.g. 0.2 = +20%")
      .def("reset", [](PyFaceDenseLandmark &self) { self.close(); },
           "Unload the model")
      .def_prop_ro("initialized", &PyFaceDenseLandmark::initialized)
      .def_prop_ro("input_width", &PyFaceDenseLandmark::inputWidth,
                   "Model input width (ROI is resized to this)")
      .def_prop_ro("input_height", &PyFaceDenseLandmark::inputHeight)
      .def_prop_ro("landmark_count", &PyFaceDenseLandmark::landmarkCount,
                   "Number of landmarks produced per face");
#endif  // TDL_PY_WITH_NPU
}
