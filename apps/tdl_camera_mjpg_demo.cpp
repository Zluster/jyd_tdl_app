#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>

#include "camera_demo_support.hpp"
#include "tdl_app/advanced.hpp"

namespace {

struct Options {
  camera_demo_support::CommonOptions camera;
  std::string output = "capture.mjpg";
  int venc_chn = 0;
  int bitrate_kbps = 4096;
  int src_fps = 25;
  int dst_fps = 25;
  bool preview_vo = false;
  int vo_dev = 0;
  int layer = 0;
  int vo_chn = 0;
  int screen_width = 0;
  int screen_height = 0;
  int interface_type = tdl_app::VoInterfaceType::Mipi;
  int interface_sync = tdl_app::VoInterfaceSync::P480_640_60;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_camera_mjpg_demo [--backend vi|vpss]\n"
      << "                       [--use-mmf | --use-sensor-media]\n"
      << "                       [--attach-existing]\n"
      << "                       [--sensor-ini FILE] [--frames N]\n"
      << "                       [--device N] [--group N] [--pipe N] [--channel N]\n"
      << "                       [--width N] [--height N] [--pixel-format N]\n"
      << "                       [--timeout-ms N] [--hold-ms N]\n"
      << "                       [--output FILE] [--venc-chn N]\n"
      << "                       [--bitrate-kbps N] [--src-fps N] [--dst-fps N]\n"
      << "                       [--preview-vo] [--vo-dev N] [--layer N] [--vo-chn N]\n"
      << "                       [--screen-width N] [--screen-height N]\n"
      << "                       [--interface-type N] [--interface-sync N]\n";
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

    if (arg == "--output") {
      const char *v = value("--output");
      if (!v) return false;
      opt->output = v;
    } else if (arg == "--venc-chn") {
      const char *v = value("--venc-chn");
      if (!v) return false;
      opt->venc_chn = std::atoi(v);
    } else if (arg == "--bitrate-kbps") {
      const char *v = value("--bitrate-kbps");
      if (!v) return false;
      opt->bitrate_kbps = std::atoi(v);
    } else if (arg == "--src-fps") {
      const char *v = value("--src-fps");
      if (!v) return false;
      opt->src_fps = std::atoi(v);
    } else if (arg == "--dst-fps") {
      const char *v = value("--dst-fps");
      if (!v) return false;
      opt->dst_fps = std::atoi(v);
    } else if (arg == "--preview-vo") {
      opt->preview_vo = true;
    } else if (arg == "--vo-dev") {
      const char *v = value("--vo-dev");
      if (!v) return false;
      opt->vo_dev = std::atoi(v);
    } else if (arg == "--layer") {
      const char *v = value("--layer");
      if (!v) return false;
      opt->layer = std::atoi(v);
    } else if (arg == "--vo-chn") {
      const char *v = value("--vo-chn");
      if (!v) return false;
      opt->vo_chn = std::atoi(v);
    } else if (arg == "--screen-width") {
      const char *v = value("--screen-width");
      if (!v) return false;
      opt->screen_width = std::atoi(v);
    } else if (arg == "--screen-height") {
      const char *v = value("--screen-height");
      if (!v) return false;
      opt->screen_height = std::atoi(v);
    } else if (arg == "--interface-type") {
      const char *v = value("--interface-type");
      if (!v) return false;
      opt->interface_type = std::atoi(v);
    } else if (arg == "--interface-sync") {
      const char *v = value("--interface-sync");
      if (!v) return false;
      opt->interface_sync = std::atoi(v);
    } else if (arg == "-h" || arg == "--help") {
      printUsage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }
  return !opt->output.empty();
}

bool writePacket(std::ofstream *ofs, const tdl_app::VencChannel::EncodedPacket &packet,
                 std::uint64_t *bytes_written) {
  for (const auto &block : packet.blocks) {
    if (!block.empty()) {
      ofs->write(reinterpret_cast<const char *>(block.data()),
                 static_cast<std::streamsize>(block.size()));
      if (!ofs->good()) {
        return false;
      }
      if (bytes_written) {
        *bytes_written += static_cast<std::uint64_t>(block.size());
      }
    }
  }
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    printUsage();
    return 1;
  }
  if (opt.camera.frames <= 0) {
    opt.camera.frames = std::numeric_limits<int>::max();
  }

  if (opt.preview_vo) {
    opt.camera.enable_preview_output = true;
    opt.camera.preview_group = opt.camera.group;
    opt.camera.preview_channel = 1;
    opt.camera.preview_width = opt.screen_width > 0 ? opt.screen_width : opt.camera.width;
    opt.camera.preview_height =
        opt.screen_height > 0 ? opt.screen_height : opt.camera.height;
    opt.camera.preview_pixel_format = opt.camera.pixel_format;
  }

  std::string error;
  camera_demo_support::CameraRuntime runtime;
  if (!camera_demo_support::openCameraRuntime(opt.camera, &runtime, &error)) {
    std::cerr << "camera runtime open failed: " << error << "\n";
    return 2;
  }

  const tdl_app::Camera::Config &camera_config = runtime.camera.config();
  const int screen_width = opt.screen_width > 0 ? opt.screen_width : camera_config.width;
  const int screen_height = opt.screen_height > 0 ? opt.screen_height : camera_config.height;

  std::unique_ptr<tdl_app::VoOutput> vo;
  std::unique_ptr<tdl_app::MediaLink> preview_link;
  if (opt.preview_vo) {
    tdl_app::VoOutput::Config vo_config;
    vo_config.device = opt.vo_dev;
    vo_config.layer = opt.layer;
    vo_config.channel = opt.vo_chn;
    vo_config.width = screen_width;
    vo_config.height = screen_height;
    vo_config.pixel_format = camera_config.pixel_format;
    vo_config.interface_type = opt.interface_type;
    vo_config.interface_sync = opt.interface_sync;
    vo.reset(new tdl_app::VoOutput(vo_config));
    if (!vo->open(&error)) {
      std::cerr << "vo open failed: " << error << "\n";
      camera_demo_support::closeCameraRuntime(&runtime);
      return 3;
    }

    tdl_app::MediaLink::Config link_config;
    link_config.source = camera_demo_support::previewChannel(opt.camera, camera_config);
    link_config.destination = tdl_app::MediaChannel::vo(opt.layer, opt.vo_chn);
    preview_link.reset(new tdl_app::MediaLink(link_config));
    if (!preview_link->bind(&error)) {
      std::cerr << "preview bind failed: " << error << "\n";
      camera_demo_support::closeCameraRuntime(&runtime);
      return 4;
    }
  }

  tdl_app::VencChannel::Config venc_config =
      tdl_app::VencChannel::mjpeg(opt.venc_chn, camera_config.width,
                                  camera_config.height, opt.bitrate_kbps,
                                  opt.src_fps, opt.dst_fps);
  tdl_app::VencChannel venc(venc_config);
  if (!venc.open(&error)) {
    std::cerr << "venc open failed: " << error << "\n";
    camera_demo_support::closeCameraRuntime(&runtime);
    return 5;
  }

  std::ofstream ofs(opt.output.c_str(), std::ios::binary | std::ios::trunc);
  if (!ofs) {
    std::cerr << "failed to open output: " << opt.output << "\n";
    camera_demo_support::closeCameraRuntime(&runtime);
    return 6;
  }

  std::uint64_t total_bytes = 0;
  int encoded_frames = 0;
  for (int index = 0; index < opt.camera.frames; ++index) {
    tdl_app::Frame frame;
    if (!runtime.camera.read(&frame, &error)) {
      std::cerr << "camera read failed: " << error << "\n";
      camera_demo_support::dumpCameraDiagnostics();
      camera_demo_support::closeCameraRuntime(&runtime);
      return 7;
    }

    tdl_app::VencChannel::EncodedPacket packet;
    if (!venc.encode(frame, &packet, &error)) {
      std::cerr << "venc encode failed: " << error << "\n";
      camera_demo_support::closeCameraRuntime(&runtime);
      return 8;
    }
    if (!writePacket(&ofs, packet, &total_bytes)) {
      std::cerr << "write output failed: " << opt.output << "\n";
      camera_demo_support::closeCameraRuntime(&runtime);
      return 9;
    }

    ++encoded_frames;
    std::cout << "mjpg frame[" << index << "] blocks=" << packet.blocks.size()
              << " key=" << (packet.key_frame ? 1 : 0)
              << " total_bytes=" << total_bytes << "\n";
  }

  ofs.close();
  std::cout << "saved mjpg: " << opt.output
            << " frames=" << encoded_frames
            << " total_bytes=" << total_bytes << "\n";

  camera_demo_support::closeCameraRuntime(&runtime);
  return 0;
}
