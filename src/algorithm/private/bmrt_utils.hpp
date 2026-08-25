#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>

#include "bmlib_runtime.h"
#include "bmruntime_interface.h"
#include "cvi_buffer.h"
#include "cvi_comm_vpss.h"
#include "cvi_vpss.h"
#include "tpu_fp16.h"

#include "tdl_app/algorithm_engine.hpp"
#include "tdl_app/model_descriptor.hpp"

namespace tdl_app {
namespace bmrt_runtime {

constexpr int kInputChannels = 3;

inline void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

// BMRT on CV184X permits several runtimes on one device but its a53lite
// teardown is not reliable when every model owns and frees a separate handle.
// Keep one process-wide device handle alive until the final model closes.
struct SharedDeviceState {
  std::mutex mutex;
  bm_handle_t handle = nullptr;
  unsigned int users = 0;
  unsigned int runtimes = 0;
  bool retain_until_process_exit = false;
};

inline SharedDeviceState &sharedDeviceState() {
  static SharedDeviceState state;
  return state;
}

inline bool acquireDevice(bm_handle_t *handle, std::string *error) {
  if (!handle) {
    setError(error, "BMRT device handle output is null");
    return false;
  }
  SharedDeviceState &state = sharedDeviceState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.handle && bm_dev_request(&state.handle, 0) != BM_SUCCESS) {
    setError(error, "bm_dev_request failed");
    return false;
  }
  ++state.users;
  if (std::getenv("TDL_BENCH_PROFILE"))
    std::fprintf(stderr, "[bmrt] acquireDevice handle=%p users=%u\n",
                 static_cast<void *>(state.handle), state.users);
  *handle = state.handle;
  return true;
}

inline void releaseDevice(bm_handle_t *handle) noexcept {
  if (!handle || !*handle) return;
  SharedDeviceState &state = sharedDeviceState();
  std::lock_guard<std::mutex> lock(state.mutex);
  *handle = nullptr;
  if (state.users == 0) return;
  --state.users;
  if (std::getenv("TDL_BENCH_PROFILE"))
    std::fprintf(stderr, "[bmrt] releaseDevice users=%u\n", state.users);
  if (state.users != 0 || !state.handle) return;
  if (state.retain_until_process_exit) {
    if (std::getenv("TDL_BENCH_PROFILE"))
      std::fprintf(stderr,
                   "[bmrt] keeping final device handle until process exit\n");
    return;
  }
  if (std::getenv("TDL_BENCH_PROFILE"))
    std::fprintf(stderr, "[bmrt] bm_dev_free begin handle=%p\n",
                 static_cast<void *>(state.handle));
  try {
    bm_dev_free(state.handle);
  } catch (const std::exception &exception) {
    std::fprintf(stderr, "bm_dev_free ignored during shutdown: %s\n",
                 exception.what());
  } catch (...) {
    std::fprintf(stderr,
                 "bm_dev_free ignored an unknown shutdown exception\n");
  }
  state.handle = nullptr;
  if (std::getenv("TDL_BENCH_PROFILE"))
    std::fprintf(stderr, "[bmrt] bm_dev_free end\n");
}

inline void *createRuntime(bm_handle_t handle) {
  void *runtime = bmrt_create(handle);
  if (!runtime) return nullptr;
  SharedDeviceState &state = sharedDeviceState();
  std::lock_guard<std::mutex> lock(state.mutex);
  ++state.runtimes;
  if (std::getenv("TDL_BENCH_PROFILE"))
    std::fprintf(stderr, "[bmrt] createRuntime runtime=%p runtimes=%u\n",
                 runtime, state.runtimes);
  return runtime;
}

inline void destroyRuntime(void *runtime) noexcept {
  if (!runtime) return;
  SharedDeviceState &state = sharedDeviceState();
  bool final_runtime = false;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.runtimes > 0) --state.runtimes;
    final_runtime = state.runtimes == 0;
    if (std::getenv("TDL_BENCH_PROFILE"))
      std::fprintf(stderr,
                   "[bmrt] destroyRuntime runtime=%p final=%d remaining=%u\n",
                   runtime, final_runtime ? 1 : 0, state.runtimes);
  }

  // On CV184X, destruction of the final runtime unloads an a53lite kernel
  // module. That library uses a noexcept destructor and terminates the host
  // process when the small core rejects the unload, so an outer catch cannot
  // recover. Keep the final runtime and its device handle until process exit.
  // This is bounded to one application lifetime, not one inference frame.
  if (final_runtime) {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.retain_until_process_exit = true;
    if (std::getenv("TDL_BENCH_PROFILE"))
      std::fprintf(stderr,
                   "[bmrt] keeping final runtime until process exit\n");
    return;
  }

  try {
    bmrt_destroy(runtime);
  } catch (const std::exception &exception) {
    std::fprintf(stderr, "bmrt_destroy ignored during shutdown: %s\n",
                 exception.what());
  } catch (...) {
    std::fprintf(stderr, "bmrt_destroy ignored an unknown shutdown exception\n");
  }
}

inline std::string toUpper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::toupper(c));
                 });
  return value;
}

inline bool startsWith(const std::string &value, const std::string &prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

template <typename T>
inline T clampCast(float value) {
  const float low = static_cast<float>(std::numeric_limits<T>::lowest());
  const float high = static_cast<float>(std::numeric_limits<T>::max());
  value = std::max(low, std::min(high, value));
  return static_cast<T>(std::lrint(value));
}

inline bool parseInputShape(const bm_shape_t &shape, int *height, int *width,
                            bool *nchw, std::string *error) {
  if (!height || !width || !nchw) {
    setError(error, "input shape output pointer is null");
    return false;
  }
  if (shape.num_dims != 4) {
    setError(error, "only 4D input tensor is supported");
    return false;
  }
  if (shape.dims[1] == kInputChannels) {
    *height = shape.dims[2];
    *width = shape.dims[3];
    *nchw = true;
    return true;
  }
  if (shape.dims[3] == kInputChannels) {
    *height = shape.dims[1];
    *width = shape.dims[2];
    *nchw = false;
    return true;
  }
  setError(error, "unable to infer input layout from tensor shape");
  return false;
}

inline std::vector<float> expandChannelValues(const std::vector<float> &values,
                                              float default_value) {
  if (values.empty()) {
    return std::vector<float>(kInputChannels, default_value);
  }
  if (values.size() == 1) {
    return std::vector<float>(kInputChannels, values[0]);
  }
  std::vector<float> expanded = values;
  expanded.resize(kInputChannels, expanded.back());
  return expanded;
}

inline bool wantsRgbInput(const ModelDescriptor &descriptor,
                          bool default_rgb = true) {
  if (descriptor.input_type.empty()) {
    return default_rgb;
  }
  return toUpper(descriptor.input_type) != "BGR";
}

inline float quantizeInputValue(float value, float input_scale,
                                int input_zero_point) {
  if (input_scale == 0.0f) {
    return value;
  }
  if (std::fabs(input_scale) > 1.0f) {
    return value * input_scale + input_zero_point;
  }
  return value / input_scale + input_zero_point;
}

inline void writeImageToTensor(const cv::Mat &image, bool rgb_input,
                               bool nchw_layout,
                               const std::vector<float> &mean,
                               const std::vector<float> &scale,
                               std::vector<float> *tensor) {
  cv::Mat prepared;
  if (rgb_input) {
    cv::cvtColor(image, prepared, cv::COLOR_BGR2RGB);
  } else {
    prepared = image;
  }

  tensor->assign(static_cast<size_t>(image.cols * image.rows * kInputChannels),
                 0.0f);
  if (nchw_layout) {
    size_t index = 0;
    for (int c = 0; c < kInputChannels; ++c) {
      for (int y = 0; y < prepared.rows; ++y) {
        for (int x = 0; x < prepared.cols; ++x) {
          const float value = prepared.at<cv::Vec3b>(y, x)[c];
          (*tensor)[index++] = (value - mean[c]) * scale[c];
        }
      }
    }
    return;
  }

  size_t index = 0;
  for (int y = 0; y < prepared.rows; ++y) {
    for (int x = 0; x < prepared.cols; ++x) {
      const cv::Vec3b pixel = prepared.at<cv::Vec3b>(y, x);
      for (int c = 0; c < kInputChannels; ++c) {
        (*tensor)[index++] =
            (static_cast<float>(pixel[c]) - mean[c]) * scale[c];
      }
    }
  }
}

inline cv::Rect clampRoi(const Box &box, int image_width, int image_height) {
  if (image_width <= 0 || image_height <= 0) {
    return cv::Rect();
  }
  const int x1 =
      std::max(0, std::min(static_cast<int>(std::floor(box.x1)), image_width - 1));
  const int y1 =
      std::max(0, std::min(static_cast<int>(std::floor(box.y1)), image_height - 1));
  const int x2 =
      std::max(0, std::min(static_cast<int>(std::ceil(box.x2)), image_width));
  const int y2 =
      std::max(0, std::min(static_cast<int>(std::ceil(box.y2)), image_height));
  const int width = std::max(1, x2 - x1);
  const int height = std::max(1, y2 - y1);
  return cv::Rect(x1, y1, width, height);
}

struct OutputTensor {
  bm_shape_t shape{};
  std::vector<float> data;
};

struct LaunchProfile {
  double input_prepare_ms = 0.0;
  double output_prepare_ms = 0.0;
  double bmrt_launch_ms = 0.0;
  double output_convert_ms = 0.0;
  double total_ms = 0.0;
};

inline double elapsedMs(std::chrono::steady_clock::time_point begin,
                        std::chrono::steady_clock::time_point end) {
  return static_cast<double>(
             std::chrono::duration_cast<std::chrono::microseconds>(end - begin)
                 .count()) /
         1000.0;
}

class Session {
 public:
  Session() = default;
  ~Session() { close(); }

  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;

  bool open(const EngineConfig &config, const ModelDescriptor &descriptor,
            std::string *error) {
    close();

    if (!acquireDevice(&handle_, error)) {
      return false;
    }

    if (!config.bmrt_firmware.empty()) {
      setenv("BMRUNTIME_USING_FIRMWARE", config.bmrt_firmware.c_str(), 0);
    }

    runtime_ = createRuntime(handle_);
    if (!runtime_) {
      setError(error, "bmrt_create failed");
      return false;
    }

    const std::string model_path = resolveModelPath(descriptor);
    if (!bmrt_load_bmodel(runtime_, model_path.c_str())) {
      setError(error, "bmrt_load_bmodel failed: " + model_path);
      return false;
    }

    const char **net_names = nullptr;
    bmrt_get_network_names(runtime_, &net_names);
    if (!net_names || bmrt_get_network_number(runtime_) <= 0) {
      setError(error, "bmodel has no network");
      if (net_names) {
        std::free(net_names);
      }
      return false;
    }

    net_name_ = net_names[0];
    std::free(net_names);

    net_info_ = bmrt_get_network_info(runtime_, net_name_.c_str());
    if (!net_info_) {
      setError(error, "bmrt_get_network_info failed");
      return false;
    }
    if (net_info_->input_num != 1) {
      setError(error, "runtime currently supports exactly one input");
      return false;
    }
    if (net_info_->output_num < 1) {
      setError(error, "runtime requires at least one output");
      return false;
    }
    if (net_info_->stage_num < 1) {
      setError(error, "invalid network stage info");
      return false;
    }

    const bm_shape_t &input_shape = net_info_->stages[0].input_shapes[0];
    // Most models use a 3-channel image tensor. Some compact classifiers,
    // such as hand-gesture classification, consume a generic feature vector.
    // Those still use launch(), but have no image dimensions or layout.
    if (input_shape.num_dims == 4 &&
        (input_shape.dims[1] == kInputChannels ||
         input_shape.dims[3] == kInputChannels)) {
      if (!parseInputShape(input_shape, &input_height_, &input_width_,
                           &nchw_layout_, error)) {
        return false;
      }
    } else {
      input_height_ = 0;
      input_width_ = 0;
      nchw_layout_ = true;
    }

    input_dtype_ = net_info_->input_dtypes[0];
    opened_ = true;
    return true;
  }

  void close() {
    if (std::getenv("TDL_BENCH_PROFILE"))
      std::fprintf(stderr, "[bmrt] Session close begin runtime=%p handle=%p opened=%d\n",
                   static_cast<void *>(runtime_), static_cast<void *>(handle_),
                   opened_ ? 1 : 0);
    if (!runtime_ && !handle_ && !opened_) return;
    if (std::getenv("TDL_BENCH_PROFILE"))
      std::fprintf(stderr, "[bmrt] releaseDeviceOutputs begin\n");
    releaseDeviceOutputs();
    if (std::getenv("TDL_BENCH_PROFILE"))
      std::fprintf(stderr, "[bmrt] releaseDeviceOutputs end\n");
    if (runtime_) {
      if (handle_) {
        if (std::getenv("TDL_BENCH_PROFILE"))
          std::fprintf(stderr, "[bmrt] bm_thread_sync begin\n");
        const bm_status_t sync_status = bm_thread_sync(handle_);
        if (std::getenv("TDL_BENCH_PROFILE"))
          std::fprintf(stderr, "[bmrt] bm_thread_sync end status=%d\n",
                       static_cast<int>(sync_status));
      }
      // Some CV184X a53lite bmodels throw while unloading their final kernel
      // module. All device buffers have already been released; avoid turning
      // an otherwise successful application shutdown into std::terminate.
      if (std::getenv("TDL_BENCH_PROFILE"))
        std::fprintf(stderr, "[bmrt] bmrt_destroy begin runtime=%p\n",
                     static_cast<void *>(runtime_));
      try {
        destroyRuntime(runtime_);
      } catch (const std::exception &exception) {
        std::fprintf(stderr, "bmrt_destroy ignored during shutdown: %s\n",
                     exception.what());
      } catch (...) {
        std::fprintf(stderr,
                     "bmrt_destroy ignored an unknown shutdown exception\n");
      }
      runtime_ = nullptr;
      if (std::getenv("TDL_BENCH_PROFILE"))
        std::fprintf(stderr, "[bmrt] bmrt_destroy end\n");
    }
    releaseDevice(&handle_);
    net_info_ = nullptr;
    net_name_.clear();
    input_height_ = 0;
    input_width_ = 0;
    nchw_layout_ = true;
    input_dtype_ = BM_FLOAT32;
    opened_ = false;
    if (std::getenv("TDL_BENCH_PROFILE"))
      std::fprintf(stderr, "[bmrt] Session close end\n");
  }

  bool launch(const std::vector<float> &input_tensor,
              std::vector<OutputTensor> *outputs, std::string *error,
              LaunchProfile *profile = nullptr) const {
    const auto total_begin = std::chrono::steady_clock::now();
    if (!opened_) {
      setError(error, "runtime session is not initialized");
      return false;
    }
    if (!outputs) {
      setError(error, "output tensor vector is null");
      return false;
    }
    if (input_tensor.size() != inputElementCount()) {
      setError(error, "runtime input tensor element count does not match model");
      return false;
    }

    const bm_shape_t input_shape = net_info_->stages[0].input_shapes[0];
    std::vector<uint8_t> input_bytes;
    void *input_ptrs[1] = {nullptr};
    bm_shape_t input_shapes[1];
    input_shapes[0] = input_shape;

    const float input_scale =
        net_info_->input_scales ? net_info_->input_scales[0] : 1.0f;
    const int input_zero_point =
        net_info_->input_zero_point ? net_info_->input_zero_point[0] : 0;

    const auto input_begin = std::chrono::steady_clock::now();
    if (input_dtype_ == BM_FLOAT32) {
      input_ptrs[0] = const_cast<float *>(input_tensor.data());
    } else if (input_dtype_ == BM_INT8) {
      input_bytes.resize(input_tensor.size());
      auto *dst = reinterpret_cast<int8_t *>(input_bytes.data());
      for (size_t i = 0; i < input_tensor.size(); ++i) {
        const float q =
            quantizeInputValue(input_tensor[i], input_scale, input_zero_point);
        dst[i] = clampCast<int8_t>(q);
      }
      input_ptrs[0] = input_bytes.data();
    } else if (input_dtype_ == BM_UINT8) {
      input_bytes.resize(input_tensor.size());
      auto *dst = reinterpret_cast<uint8_t *>(input_bytes.data());
      for (size_t i = 0; i < input_tensor.size(); ++i) {
        const float q =
            quantizeInputValue(input_tensor[i], input_scale, input_zero_point);
        dst[i] = clampCast<uint8_t>(q);
      }
      input_ptrs[0] = input_bytes.data();
    } else {
      setError(error, "runtime does not support this input dtype");
      return false;
    }
    if (profile) {
      profile->input_prepare_ms =
          elapsedMs(input_begin, std::chrono::steady_clock::now());
    }

    return launchPreparedInternal(input_ptrs[0], outputs, error, profile,
                                  total_begin);
  }

  bool launchPrepared(const void *input_data, std::vector<OutputTensor> *outputs,
                      std::string *error,
                      LaunchProfile *profile = nullptr) const {
    const auto total_begin = std::chrono::steady_clock::now();
    if (!opened_) {
      setError(error, "runtime session is not initialized");
      return false;
    }
    if (!input_data) {
      setError(error, "prepared input pointer is null");
      return false;
    }
    if (!outputs) {
      setError(error, "output tensor vector is null");
      return false;
    }
    if (profile) {
      profile->input_prepare_ms = 0.0;
    }
    return launchPreparedInternal(input_data, outputs, error, profile,
                                  total_begin);
  }

  // The input is already device-backed by the VPSS preprocessor. Output
  // buffers are allocated once and reused for the lifetime of this session.
  bool launchDevice(bm_device_mem_t input_memory,
                    std::vector<OutputTensor> *outputs, std::string *error,
                    LaunchProfile *profile = nullptr) {
    const auto total_begin = std::chrono::steady_clock::now();
    if (!opened_) {
      setError(error, "runtime session is not initialized");
      return false;
    }
    if (input_memory.size == 0) {
      setError(error, "device input memory is empty");
      return false;
    }
    if (!outputs) {
      setError(error, "output tensor vector is null");
      return false;
    }
    if (!ensureDeviceOutputs(error)) {
      return false;
    }

    bm_tensor_t input{};
    bmrt_tensor_with_device(&input, input_memory, input_dtype_,
                            net_info_->stages[0].input_shapes[0]);
    std::vector<bm_tensor_t> device_outputs(
        static_cast<size_t>(net_info_->output_num), bm_tensor_t{});
    for (int i = 0; i < net_info_->output_num; ++i) {
      bmrt_tensor_with_device(&device_outputs[static_cast<size_t>(i)],
                              device_outputs_[static_cast<size_t>(i)],
                              net_info_->output_dtypes[i],
                              net_info_->stages[0].output_shapes[i]);
    }
    if (profile) {
      profile->input_prepare_ms = 0.0;
      profile->output_prepare_ms = 0.0;
    }

    const auto launch_begin = std::chrono::steady_clock::now();
    if (!bmrt_launch_tensor_ex(runtime_, net_name_.c_str(), &input, 1,
                               device_outputs.data(), net_info_->output_num,
                               true, false)) {
      setError(error, "bmrt_launch_tensor_ex failed");
      return false;
    }
    if (bm_thread_sync(handle_) != BM_SUCCESS) {
      setError(error, "bm_thread_sync failed");
      return false;
    }
    if (profile) {
      profile->bmrt_launch_ms =
          elapsedMs(launch_begin, std::chrono::steady_clock::now());
    }

    const auto output_convert_begin = std::chrono::steady_clock::now();
    outputs->clear();
    outputs->reserve(static_cast<size_t>(net_info_->output_num));
    for (int i = 0; i < net_info_->output_num; ++i) {
      const bm_tensor_t &tensor = device_outputs[static_cast<size_t>(i)];
      const size_t bytes = shapeElementCount(tensor.shape) *
                           bmrt_data_type_size(net_info_->output_dtypes[i]);
      std::vector<uint8_t> raw(bytes);
      if (bm_memcpy_d2s(handle_, raw.data(), tensor.device_mem) != BM_SUCCESS) {
        setError(error, "bm_memcpy_d2s failed for device output");
        return false;
      }
      if (!appendDecodedOutput(i, tensor.shape, raw.data(), outputs, error)) {
        return false;
      }
    }
    if (profile) {
      const auto total_end = std::chrono::steady_clock::now();
      profile->output_convert_ms =
          elapsedMs(output_convert_begin, total_end);
      profile->total_ms = elapsedMs(total_begin, total_end);
    }
    return true;
  }

  const bm_net_info_t *netInfo() const { return net_info_; }
  int inputHeight() const { return input_height_; }
  int inputWidth() const { return input_width_; }
  bool nchwLayout() const { return nchw_layout_; }
  bool opened() const { return opened_; }
  bm_data_type_t inputDtype() const { return input_dtype_; }
  float inputScale() const {
    return net_info_ && net_info_->input_scales ? net_info_->input_scales[0]
                                                : 1.0f;
  }
  int inputZeroPoint() const {
    return net_info_ && net_info_->input_zero_point
               ? net_info_->input_zero_point[0]
               : 0;
  }
  size_t inputElementCount() const {
    return net_info_ ? shapeElementCount(net_info_->stages[0].input_shapes[0])
                     : 0;
  }
  bm_handle_t handle() const { return handle_; }

 private:
  static size_t shapeElementCount(const bm_shape_t &shape) {
    size_t count = 1;
    for (int d = 0; d < shape.num_dims; ++d) {
      count *= static_cast<size_t>(shape.dims[d]);
    }
    return count;
  }

  bool ensureDeviceOutputs(std::string *error) {
    if (device_outputs_.size() ==
        static_cast<size_t>(net_info_->output_num)) {
      return true;
    }
    releaseDeviceOutputs();
    device_outputs_.resize(static_cast<size_t>(net_info_->output_num));
    for (int i = 0; i < net_info_->output_num; ++i) {
      size_t bytes = net_info_->max_output_bytes[i];
      if (bytes == 0) {
        bytes = shapeElementCount(net_info_->stages[0].output_shapes[i]) *
                bmrt_data_type_size(net_info_->output_dtypes[i]);
      }
      if (bytes == 0 ||
          bm_malloc_device_byte(handle_, &device_outputs_[static_cast<size_t>(i)],
                                bytes) != BM_SUCCESS) {
        setError(error, "bm_malloc_device_byte failed for runtime output");
        releaseDeviceOutputs();
        return false;
      }
    }
    return true;
  }

  void releaseDeviceOutputs() {
    if (!handle_) {
      device_outputs_.clear();
      return;
    }
    for (bm_device_mem_t &memory : device_outputs_) {
      if (memory.size > 0) {
        bm_free_device(handle_, memory);
        memory = bm_device_mem_t{};
      }
    }
    device_outputs_.clear();
  }

  bool appendDecodedOutput(int output_index, const bm_shape_t &shape,
                           const uint8_t *raw,
                           std::vector<OutputTensor> *outputs,
                           std::string *error) const {
    if (!raw) {
      setError(error, "raw output buffer is null");
      return false;
    }
    const size_t element_count = shapeElementCount(shape);
    OutputTensor out;
    out.shape = shape;
    out.data.resize(element_count, 0.0f);
    const float output_scale =
        net_info_->output_scales ? net_info_->output_scales[output_index] : 1.0f;
    const int output_zero_point = net_info_->output_zero_point
                                      ? net_info_->output_zero_point[output_index]
                                      : 0;
    if (net_info_->output_dtypes[output_index] == BM_FLOAT32) {
      const float *values = reinterpret_cast<const float *>(raw);
      out.data.assign(values, values + element_count);
    } else if (net_info_->output_dtypes[output_index] == BM_FLOAT16) {
      const fp16 *values = reinterpret_cast<const fp16 *>(raw);
      for (size_t j = 0; j < element_count; ++j) {
        out.data[j] = fp16_to_fp32(values[j]).fval;
      }
    } else if (net_info_->output_dtypes[output_index] == BM_BFLOAT16) {
      const bf16 *values = reinterpret_cast<const bf16 *>(raw);
      for (size_t j = 0; j < element_count; ++j) {
        out.data[j] = bf16_to_fp32(values[j]).fval;
      }
    } else if (net_info_->output_dtypes[output_index] == BM_INT8) {
      const int8_t *values = reinterpret_cast<const int8_t *>(raw);
      for (size_t j = 0; j < element_count; ++j) {
        out.data[j] =
            (static_cast<int>(values[j]) - output_zero_point) * output_scale;
      }
    } else if (net_info_->output_dtypes[output_index] == BM_UINT8) {
      const uint8_t *values = reinterpret_cast<const uint8_t *>(raw);
      for (size_t j = 0; j < element_count; ++j) {
        out.data[j] =
            (static_cast<int>(values[j]) - output_zero_point) * output_scale;
      }
    } else if (net_info_->output_dtypes[output_index] == BM_INT16) {
      const int16_t *values = reinterpret_cast<const int16_t *>(raw);
      for (size_t j = 0; j < element_count; ++j) {
        out.data[j] =
            (static_cast<int>(values[j]) - output_zero_point) * output_scale;
      }
    } else if (net_info_->output_dtypes[output_index] == BM_UINT16) {
      const uint16_t *values = reinterpret_cast<const uint16_t *>(raw);
      for (size_t j = 0; j < element_count; ++j) {
        out.data[j] =
            (static_cast<int>(values[j]) - output_zero_point) * output_scale;
      }
    } else if (net_info_->output_dtypes[output_index] == BM_INT32) {
      const int32_t *values = reinterpret_cast<const int32_t *>(raw);
      for (size_t j = 0; j < element_count; ++j) {
        out.data[j] =
            (static_cast<float>(values[j]) - output_zero_point) * output_scale;
      }
    } else if (net_info_->output_dtypes[output_index] == BM_UINT32) {
      const uint32_t *values = reinterpret_cast<const uint32_t *>(raw);
      for (size_t j = 0; j < element_count; ++j) {
        out.data[j] =
            (static_cast<float>(values[j]) - output_zero_point) * output_scale;
      }
    } else {
      setError(error, "runtime does not support this output dtype");
      return false;
    }
    outputs->push_back(std::move(out));
    return true;
  }

  bool launchPreparedInternal(const void *input_data,
                              std::vector<OutputTensor> *outputs,
                              std::string *error, LaunchProfile *profile,
                              std::chrono::steady_clock::time_point total_begin)
      const {
    const bm_shape_t input_shape = net_info_->stages[0].input_shapes[0];
    void *input_ptrs[1] = {const_cast<void *>(input_data)};
    bm_shape_t input_shapes[1];
    input_shapes[0] = input_shape;

    const auto output_prepare_begin = std::chrono::steady_clock::now();
    std::vector<std::vector<uint8_t>> output_bytes(
        static_cast<size_t>(net_info_->output_num), std::vector<uint8_t>());
    std::vector<void *> output_ptrs(static_cast<size_t>(net_info_->output_num),
                                    nullptr);
    std::vector<bm_shape_t> output_shapes(
        static_cast<size_t>(net_info_->output_num), bm_shape_t{});

    for (int i = 0; i < net_info_->output_num; ++i) {
      output_bytes[static_cast<size_t>(i)].resize(net_info_->max_output_bytes[i]);
      output_ptrs[static_cast<size_t>(i)] =
          output_bytes[static_cast<size_t>(i)].data();
      std::memset(&output_shapes[static_cast<size_t>(i)], 0, sizeof(bm_shape_t));
    }
    if (profile) {
      profile->output_prepare_ms =
          elapsedMs(output_prepare_begin, std::chrono::steady_clock::now());
    }

    const auto launch_begin = std::chrono::steady_clock::now();
    if (!bmrt_launch_data(runtime_, net_name_.c_str(), input_ptrs, input_shapes,
                          1, output_ptrs.data(), output_shapes.data(),
                          net_info_->output_num, true)) {
      setError(error, "bmrt_launch_data failed");
      return false;
    }
    if (profile) {
      profile->bmrt_launch_ms =
          elapsedMs(launch_begin, std::chrono::steady_clock::now());
    }

    const auto output_convert_begin = std::chrono::steady_clock::now();
    outputs->clear();
    outputs->reserve(static_cast<size_t>(net_info_->output_num));
    for (int i = 0; i < net_info_->output_num; ++i) {
      size_t element_count = 1;
      const bm_shape_t &shape = output_shapes[static_cast<size_t>(i)];
      for (int d = 0; d < shape.num_dims; ++d) {
        element_count *= static_cast<size_t>(shape.dims[d]);
      }

      OutputTensor out;
      out.shape = shape;
      out.data.resize(element_count, 0.0f);

      const float output_scale =
          net_info_->output_scales ? net_info_->output_scales[i] : 1.0f;
      const int output_zero_point =
          net_info_->output_zero_point ? net_info_->output_zero_point[i] : 0;

      if (net_info_->output_dtypes[i] == BM_FLOAT32) {
        const float *raw = reinterpret_cast<const float *>(
            output_bytes[static_cast<size_t>(i)].data());
        out.data.assign(raw, raw + element_count);
      } else if (net_info_->output_dtypes[i] == BM_FLOAT16) {
        const fp16 *raw = reinterpret_cast<const fp16 *>(
            output_bytes[static_cast<size_t>(i)].data());
        for (size_t j = 0; j < element_count; ++j) {
          out.data[j] = fp16_to_fp32(raw[j]).fval;
        }
      } else if (net_info_->output_dtypes[i] == BM_BFLOAT16) {
        const bf16 *raw = reinterpret_cast<const bf16 *>(
            output_bytes[static_cast<size_t>(i)].data());
        for (size_t j = 0; j < element_count; ++j) {
          out.data[j] = bf16_to_fp32(raw[j]).fval;
        }
      } else if (net_info_->output_dtypes[i] == BM_INT8) {
        const int8_t *raw = reinterpret_cast<const int8_t *>(
            output_bytes[static_cast<size_t>(i)].data());
        for (size_t j = 0; j < element_count; ++j) {
          out.data[j] =
              (static_cast<int>(raw[j]) - output_zero_point) * output_scale;
        }
      } else if (net_info_->output_dtypes[i] == BM_UINT8) {
        const uint8_t *raw = reinterpret_cast<const uint8_t *>(
            output_bytes[static_cast<size_t>(i)].data());
        for (size_t j = 0; j < element_count; ++j) {
          out.data[j] =
              (static_cast<int>(raw[j]) - output_zero_point) * output_scale;
        }
      } else if (net_info_->output_dtypes[i] == BM_INT16) {
        const int16_t *raw = reinterpret_cast<const int16_t *>(
            output_bytes[static_cast<size_t>(i)].data());
        for (size_t j = 0; j < element_count; ++j) {
          out.data[j] =
              (static_cast<int>(raw[j]) - output_zero_point) * output_scale;
        }
      } else if (net_info_->output_dtypes[i] == BM_UINT16) {
        const uint16_t *raw = reinterpret_cast<const uint16_t *>(
            output_bytes[static_cast<size_t>(i)].data());
        for (size_t j = 0; j < element_count; ++j) {
          out.data[j] =
              (static_cast<int>(raw[j]) - output_zero_point) * output_scale;
        }
      } else if (net_info_->output_dtypes[i] == BM_INT32) {
        const int32_t *raw = reinterpret_cast<const int32_t *>(
            output_bytes[static_cast<size_t>(i)].data());
        for (size_t j = 0; j < element_count; ++j) {
          out.data[j] =
              (static_cast<float>(raw[j]) - output_zero_point) * output_scale;
        }
      } else if (net_info_->output_dtypes[i] == BM_UINT32) {
        const uint32_t *raw = reinterpret_cast<const uint32_t *>(
            output_bytes[static_cast<size_t>(i)].data());
        for (size_t j = 0; j < element_count; ++j) {
          out.data[j] =
              (static_cast<float>(raw[j]) - output_zero_point) * output_scale;
        }
      } else {
        setError(error, "runtime does not support this output dtype");
        return false;
      }
      outputs->push_back(std::move(out));
    }
    if (profile) {
      const auto total_end = std::chrono::steady_clock::now();
      profile->output_convert_ms = elapsedMs(output_convert_begin, total_end);
      profile->total_ms = elapsedMs(total_begin, total_end);
    }

    return true;
  }

  bm_handle_t handle_ = nullptr;
  void *runtime_ = nullptr;
  const bm_net_info_t *net_info_ = nullptr;
  std::string net_name_;
  std::vector<bm_device_mem_t> device_outputs_;
  int input_height_ = 0;
  int input_width_ = 0;
  bool nchw_layout_ = true;
  bm_data_type_t input_dtype_ = BM_FLOAT32;
  bool opened_ = false;
};


}  // namespace bmrt_runtime
}  // namespace tdl_app
