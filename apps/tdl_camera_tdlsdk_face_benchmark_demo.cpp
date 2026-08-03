#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>

#include "camera_demo_support.hpp"
#include "c_apis/tdl_model_def.h"
#include "c_apis/tdl_sdk.h"
#include "c_apis/tdl_types.h"
#include "c_apis/tdl_utils.h"
#include "nn/tdl_model_list.h"
#include "tdl_app/model_descriptor.hpp"

namespace {

using SteadyClock = std::chrono::steady_clock;

struct Options {
  camera_demo_support::CommonOptions camera;
  std::string model_spec;
  std::string firmware;
  float threshold = 0.25f;
  int warmup = 5;
};

double elapsedMs(const SteadyClock::time_point &begin,
                 const SteadyClock::time_point &end) {
  return static_cast<double>(
             std::chrono::duration_cast<std::chrono::microseconds>(end - begin)
                 .count()) /
         1000.0;
}

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

std::string normalizeToken(std::string value) {
  for (char &ch : value) {
    if (ch >= 'a' && ch <= 'z') {
      ch = static_cast<char>(ch - 'a' + 'A');
    } else if (ch == '-') {
      ch = '_';
    }
  }
  if (value == "SCRFD") {
    return "SCRFD_DET_FACE";
  }
  return value;
}

bool resolveModelId(const std::string &model_token, TDLModel *model_id,
                    std::string *error) {
  if (!model_id) {
    setError(error, "model id pointer is null");
    return false;
  }
  const std::string normalized = normalizeToken(model_token);
#define X(name, comment)                                                    \
  if (normalized == #name || normalized == "TDL_MODEL_" #name) {           \
    *model_id = TDL_MODEL_##name;                                           \
    return true;                                                            \
  }
  MODEL_TYPE_LIST
#undef X
  setError(error, "unsupported TDL model_type: " + model_token);
  return false;
}

bool resolveModel(const Options &opt, std::string *model_token,
                  std::string *model_path, std::string *error) {
  tdl_app::ModelDescriptor descriptor;
  if (!tdl_app::loadModelDescriptor(opt.model_spec, &descriptor, error)) {
    return false;
  }
  *model_token = normalizeToken(
      descriptor.model_type.empty() ? std::string("SCRFD") : descriptor.model_type);
  *model_path = tdl_app::resolveModelPath(descriptor);
  if (model_path->empty()) {
    setError(error, "resolved model path is empty");
    return false;
  }
  return true;
}

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_camera_tdlsdk_face_benchmark_demo --model-spec FILE\n"
      << "                                        [--firmware FILE]\n"
      << "                                        [--threshold 0.25]\n"
      << "                                        [--warmup N]\n"
      << "                                        [--backend vi|vpss]\n"
      << "                                        [default: ai channel 640x640]\n"
      << "                                        [--use-mmf | --use-sensor-media]\n"
      << "                                        [--attach-existing]\n"
      << "                                        [--sensor-ini FILE] [--frames N]\n"
      << "                                        [--device N] [--group N] [--pipe N]\n"
      << "                                        [--channel N] [--width N] [--height N]\n"
      << "                                        [--pixel-format N] [--timeout-ms N]\n";
}

bool parseArgs(int argc, char **argv, Options *opt) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    bool handled = false;
    std::string parse_error;
    if (!camera_demo_support::parseCommonArgs(argc, argv, &i, &opt->camera,
                                              &handled, &parse_error)) {
      std::cerr << parse_error << "\n";
      return false;
    }
    if (handled) {
      continue;
    }

    auto value = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << name << "\n";
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--model-spec") {
      const char *v = value("--model-spec");
      if (!v) return false;
      opt->model_spec = v;
    } else if (arg == "--firmware") {
      const char *v = value("--firmware");
      if (!v) return false;
      opt->firmware = v;
    } else if (arg == "--threshold") {
      const char *v = value("--threshold");
      if (!v) return false;
      opt->threshold = static_cast<float>(std::atof(v));
    } else if (arg == "--warmup") {
      const char *v = value("--warmup");
      if (!v) return false;
      opt->warmup = std::atoi(v);
    } else if (arg == "-h" || arg == "--help") {
      printUsage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }
  return !opt->model_spec.empty();
}

void printPreprocessInfo(TDLHandle handle, TDLModel model_id) {
  TDLPreprocessParams params;
  std::memset(&params, 0, sizeof(params));
  if (TDL_GetPreprocessParameters(handle, model_id, &params) != 0) {
    std::cerr << "tdl preprocess: unavailable\n";
    return;
  }
  std::cerr << "tdl preprocess: fmt=" << static_cast<int>(params.dst_image_format)
            << " dtype=" << static_cast<int>(params.dst_pixdata_type)
            << " dst=" << params.dst_width << "x" << params.dst_height
            << " keep_aspect=" << (params.keep_aspect_ratio ? 1 : 0)
            << " crop=" << params.crop_x << "," << params.crop_y
            << " " << params.crop_width << "x" << params.crop_height
            << " mean=(" << params.mean[0] << "," << params.mean[1] << ","
            << params.mean[2] << ")"
            << " scale=(" << params.scale[0] << "," << params.scale[1] << ","
            << params.scale[2] << ")\n";
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    printUsage();
    return 1;
  }

  camera_demo_support::CameraRuntime runtime;
  std::string error;
  if (!camera_demo_support::openCameraRuntime(opt.camera, &runtime, &error)) {
    std::cerr << "camera runtime open failed: " << error << "\n";
    return 2;
  }

  const tdl_app::Camera::Config &camera_config = runtime.camera.config();
  std::cerr << "camera config: backend="
            << camera_demo_support::backendName(camera_config.backend)
            << " device=" << camera_config.device
            << " pipe=" << camera_config.pipe
            << " group=" << camera_config.group
            << " channel=" << camera_config.channel
            << " width=" << camera_config.width
            << " height=" << camera_config.height
            << " pixel_format=" << camera_config.pixel_format
            << " timeout_ms=" << camera_config.timeout_ms << "\n";

  std::string model_token;
  std::string model_path;
  if (!resolveModel(opt, &model_token, &model_path, &error)) {
    std::cerr << "resolve model failed: " << error << "\n";
    camera_demo_support::closeCameraRuntime(&runtime);
    return 3;
  }

  if (!opt.firmware.empty()) {
    setenv("BMRUNTIME_USING_FIRMWARE", opt.firmware.c_str(), 0);
  }

  TDLModel model_id = TDL_MODEL_INVALID;
  if (!resolveModelId(model_token, &model_id, &error)) {
    std::cerr << "resolve model id failed: " << error << "\n";
    camera_demo_support::closeCameraRuntime(&runtime);
    return 4;
  }

  TDLHandle handle = TDL_CreateHandle(0);
  if (!handle) {
    std::cerr << "TDL_CreateHandle failed\n";
    camera_demo_support::closeCameraRuntime(&runtime);
    return 5;
  }

  const int open_ret =
      TDL_OpenModel(handle, model_id, model_path.c_str(), nullptr, 0);
  if (open_ret != 0) {
    std::cerr << "TDL_OpenModel failed: ret=" << open_ret
              << " model_id=" << static_cast<int>(model_id)
              << " path=" << model_path << "\n";
    TDL_DestroyHandle(handle);
    camera_demo_support::closeCameraRuntime(&runtime);
    return 6;
  }

  const int threshold_ret = TDL_SetModelThreshold(handle, model_id, opt.threshold);
  if (threshold_ret != 0) {
    std::cerr << "TDL_SetModelThreshold failed: ret=" << threshold_ret << "\n";
  }
  printPreprocessInfo(handle, model_id);

  const int measured_frames = opt.camera.frames <= 0 ? 1 : opt.camera.frames;
  const int total_frames = opt.warmup + measured_frames;

  double read_sum_ms = 0.0;
  double infer_sum_ms = 0.0;
  double total_sum_ms = 0.0;
  double face_sum = 0.0;
  std::uint64_t last_sequence = 0;
  std::uint64_t last_pts = 0;
  int last_width = 0;
  int last_height = 0;
  int last_format = 0;
  std::uint32_t last_faces = 0;

  for (int index = 0; index < total_frames; ++index) {
    tdl_app::Frame frame;
    const auto read_begin = SteadyClock::now();
    if (!runtime.camera.read(&frame, &error)) {
      std::cerr << "camera read failed: " << error << "\n";
      TDL_CloseModel(handle, model_id);
      TDL_DestroyHandle(handle);
      camera_demo_support::closeCameraRuntime(&runtime);
      return 7;
    }
    const auto read_end = SteadyClock::now();
    const double read_ms = elapsedMs(read_begin, read_end);

    TDLImage image = TDL_WrapFrame(frame.native, false, false);
    if (!image) {
      std::cerr << "TDL_WrapFrame failed\n";
      TDL_CloseModel(handle, model_id);
      TDL_DestroyHandle(handle);
      camera_demo_support::closeCameraRuntime(&runtime);
      return 8;
    }

    TDLFace faces;
    std::memset(&faces, 0, sizeof(faces));
    const auto infer_begin = SteadyClock::now();
    const int infer_ret = TDL_FaceDetection(handle, model_id, image, &faces);
    const auto infer_end = SteadyClock::now();
    TDL_DestroyImage(image);
    if (infer_ret != 0) {
      std::cerr << "TDL_FaceDetection failed: ret=" << infer_ret << "\n";
      TDL_CloseModel(handle, model_id);
      TDL_DestroyHandle(handle);
      camera_demo_support::closeCameraRuntime(&runtime);
      return 9;
    }

    const double infer_ms = elapsedMs(infer_begin, infer_end);
    const double total_ms = read_ms + infer_ms;
    const bool warmup = index < opt.warmup;
    if (!warmup) {
      read_sum_ms += read_ms;
      infer_sum_ms += infer_ms;
      total_sum_ms += total_ms;
      face_sum += static_cast<double>(faces.size);
      last_sequence = frame.sequence;
      last_pts = frame.timestamp_us;
      last_width = frame.width;
      last_height = frame.height;
      last_format = frame.format;
      last_faces = faces.size;
    }
    TDL_ReleaseFaceMeta(&faces);
  }

  const double denom =
      measured_frames > 0 ? static_cast<double>(measured_frames) : 1.0;
  std::cout << std::fixed << std::setprecision(3)
            << "summary: frames=" << measured_frames
            << " avg_read=" << (read_sum_ms / denom)
            << " ms, avg_tdl_infer=" << (infer_sum_ms / denom)
            << " ms, avg_total=" << (total_sum_ms / denom)
            << " ms, avg_boxes=" << (face_sum / denom)
            << ", avg_fps="
            << ((total_sum_ms / denom) > 0.0 ? 1000.0 / (total_sum_ms / denom)
                                             : 0.0)
            << "\n";
  std::cout << "last_frame: seq=" << last_sequence
            << " pts=" << last_pts
            << " src=" << last_width << "x" << last_height
            << " fmt=" << last_format
            << " boxes=" << last_faces << "\n";

  TDL_CloseModel(handle, model_id);
  TDL_DestroyHandle(handle);
  camera_demo_support::closeCameraRuntime(&runtime);
  return 0;
}
