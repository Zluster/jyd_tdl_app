#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "cvi_buffer.h"
#include "tdl_app/advanced.hpp"

namespace {

struct Options {
  std::string input;
  std::string codec = "jpeg";
  int channel = 0;
  int width = 1920;
  int height = 1080;
  int pixel_format = -1;
  int timeout_ms = 1000;
  int max_reads = 10;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_vdec_demo --input FILE [--codec jpeg|mjpeg|h264|h265]\n"
      << "                [--channel N] [--width N] [--height N]\n"
      << "                [--pixel-format N] [--timeout-ms N] [--max-reads N]\n";
}

bool parseArgs(int argc, char **argv, Options *opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << name << "\n";
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--input") {
      const char *v = value("--input");
      if (!v) return false;
      opt->input = v;
    } else if (arg == "--codec") {
      const char *v = value("--codec");
      if (!v) return false;
      opt->codec = v;
    } else if (arg == "--channel") {
      const char *v = value("--channel");
      if (!v) return false;
      opt->channel = std::atoi(v);
    } else if (arg == "--width") {
      const char *v = value("--width");
      if (!v) return false;
      opt->width = std::atoi(v);
    } else if (arg == "--height") {
      const char *v = value("--height");
      if (!v) return false;
      opt->height = std::atoi(v);
    } else if (arg == "--pixel-format") {
      const char *v = value("--pixel-format");
      if (!v) return false;
      opt->pixel_format = std::atoi(v);
    } else if (arg == "--timeout-ms") {
      const char *v = value("--timeout-ms");
      if (!v) return false;
      opt->timeout_ms = std::atoi(v);
    } else if (arg == "--max-reads") {
      const char *v = value("--max-reads");
      if (!v) return false;
      opt->max_reads = std::atoi(v);
    } else if (arg == "-h" || arg == "--help") {
      printUsage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }

  return !opt->input.empty();
}

bool readFile(const std::string &path, std::vector<unsigned char> *data) {
  std::ifstream ifs(path.c_str(), std::ios::binary);
  if (!ifs) {
    return false;
  }
  ifs.seekg(0, std::ios::end);
  const std::streamoff size = ifs.tellg();
  ifs.seekg(0, std::ios::beg);
  if (size < 0) {
    return false;
  }
  data->resize(static_cast<std::size_t>(size));
  if (size == 0) {
    return true;
  }
  ifs.read(reinterpret_cast<char *>(data->data()), size);
  return ifs.good() || ifs.eof();
}

bool parseJpegFrame(const std::vector<unsigned char> &data, std::size_t *start,
                    std::size_t *length) {
  if (!start || !length || data.size() < 4) {
    return false;
  }

  bool found_start = false;
  bool found_end = false;
  std::size_t frame_start = 0;
  std::size_t i = 0;

  for (; i + 1 < data.size(); ++i) {
    if (data[i] == 0xFF && data[i + 1] == 0xD8) {
      frame_start = i;
      found_start = true;
      i += 2;
      break;
    }
  }
  for (; i + 3 < data.size(); ++i) {
    if (data[i] == 0xFF && (data[i + 1] & 0xF0) == 0xE0) {
      const std::size_t segment_len =
          (static_cast<std::size_t>(data[i + 2]) << 8) + data[i + 3];
      i += 1 + segment_len;
      continue;
    }
    break;
  }
  for (; i + 1 < data.size(); ++i) {
    if (data[i] == 0xFF && data[i + 1] == 0xD9) {
      found_end = true;
      *start = frame_start;
      *length = (i + 2) - frame_start;
      break;
    }
  }

  return found_start && found_end;
}

tdl_app::VdecChannel::Codec parseCodec(const std::string &codec) {
  if (codec == "h264") return tdl_app::VdecChannel::Codec::H264;
  if (codec == "h265") return tdl_app::VdecChannel::Codec::H265;
  if (codec == "mjpeg") return tdl_app::VdecChannel::Codec::Mjpeg;
  return tdl_app::VdecChannel::Codec::Jpeg;
}

bool isPictureCodec(tdl_app::VdecChannel::Codec codec) {
  return codec == tdl_app::VdecChannel::Codec::Jpeg ||
         codec == tdl_app::VdecChannel::Codec::Mjpeg;
}

PAYLOAD_TYPE_E toPayload(tdl_app::VdecChannel::Codec codec) {
  switch (codec) {
    case tdl_app::VdecChannel::Codec::H265:
      return PT_H265;
    case tdl_app::VdecChannel::Codec::Jpeg:
      return PT_JPEG;
    case tdl_app::VdecChannel::Codec::Mjpeg:
      return PT_MJPEG;
    case tdl_app::VdecChannel::Codec::H264:
    default:
      return PT_H264;
  }
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    printUsage();
    return 1;
  }

  std::vector<unsigned char> bytes;
  if (!readFile(opt.input, &bytes)) {
    std::cerr << "failed to read input: " << opt.input << "\n";
    return 2;
  }
  if (bytes.empty()) {
    std::cerr << "input file is empty: " << opt.input << "\n";
    return 3;
  }

  std::string error;
  tdl_app::VdecChannel::Config config;
  config.channel = opt.channel;
  config.codec = parseCodec(opt.codec);
  config.width = opt.width;
  config.height = opt.height;
  if (opt.pixel_format >= 0) {
    config.output_pixel_format = opt.pixel_format;
  } else if (config.codec == tdl_app::VdecChannel::Codec::Jpeg ||
             config.codec == tdl_app::VdecChannel::Codec::Mjpeg) {
    config.output_pixel_format = 14;
  } else {
    config.output_pixel_format = 18;
  }
  config.timeout_ms = opt.timeout_ms;
  config.mode = config.codec == tdl_app::VdecChannel::Codec::Jpeg ||
                        config.codec == tdl_app::VdecChannel::Codec::Mjpeg
                    ? tdl_app::VdecChannel::Mode::Frame
                    : tdl_app::VdecChannel::Mode::Stream;

  tdl_app::SysContext sys;
  if (!sys.open(&error)) {
    std::cerr << "sys open failed: " << error << "\n";
    return 4;
  }

  tdl_app::VideoBufferManager::Config vb_manager_config;
  vb_manager_config.reuse_existing = true;
  vb_manager_config.common_pools.push_back(
      tdl_app::VideoBufferPoolConfig{{opt.width, opt.height},
                                     config.output_pixel_format,
                                     2,
                                     64,
                                     true});
  tdl_app::VideoBufferManager vb_manager(vb_manager_config);
  if (!vb_manager.open(&error)) {
    std::cerr << "vb manager open failed: " << error << "\n";
    return 5;
  }

  std::unique_ptr<tdl_app::VideoBufferPool> picture_pool;
  if (isPictureCodec(config.codec)) {
    tdl_app::VideoBufferPool::Config picture_pool_config;
    picture_pool_config.size = {opt.width, opt.height};
    picture_pool_config.pixel_format = config.output_pixel_format;
    picture_pool_config.block_count = 1;
    picture_pool_config.cached = true;
    picture_pool_config.remap_mode = 0;
    picture_pool_config.block_size = VDEC_GetPicBufferSize(
        toPayload(config.codec), static_cast<CVI_U32>(opt.width),
        static_cast<CVI_U32>(opt.height),
        static_cast<PIXEL_FORMAT_E>(config.output_pixel_format),
        DATA_BITWIDTH_8, COMPRESS_MODE_NONE);
    picture_pool_config.name = "tdl_vdec_pic";
    picture_pool.reset(new tdl_app::VideoBufferPool(picture_pool_config));
    if (!picture_pool->create(&error)) {
      std::cerr << "picture pool create failed: " << error << "\n";
      return 6;
    }
    config.picture_pool_id = picture_pool->poolId();
  }

  tdl_app::VdecChannel vdec(config);
  if (!vdec.open(&error)) {
    std::cerr << "vdec open failed: " << error << "\n";
    return 7;
  }

  tdl_app::VdecChannel::StreamPacket packet;
  std::size_t jpeg_start = 0;
  std::size_t jpeg_length = bytes.size();
  if (isPictureCodec(config.codec) &&
      !parseJpegFrame(bytes, &jpeg_start, &jpeg_length)) {
    std::cerr << "failed to parse jpeg frame boundaries: " << opt.input << "\n";
    return 8;
  }

  packet.data = bytes.data() + jpeg_start;
  packet.size = jpeg_length;
  packet.end_of_frame =
      config.codec == tdl_app::VdecChannel::Codec::H264 ||
      config.codec == tdl_app::VdecChannel::Codec::H265;
  packet.end_of_stream = true;
  packet.display = true;

  std::cerr << "vdec_demo: send stream size=" << packet.size
            << " start=" << jpeg_start
            << " eof=" << (packet.end_of_frame ? 1 : 0)
            << " eos=" << (packet.end_of_stream ? 1 : 0) << "\n";
  if (!vdec.sendStream(packet, &error)) {
    std::cerr << "vdec send failed: " << error << "\n";
    return 8;
  }

  for (int i = 0; i < opt.max_reads; ++i) {
    tdl_app::Frame frame;
    if (vdec.read(&frame, &error)) {
      std::cout << "decoded frame: "
                << "width=" << frame.width
                << " height=" << frame.height
                << " format=" << frame.format
                << " seq=" << frame.sequence
                << " pts_us=" << frame.timestamp_us << "\n";
      vdec.releaseFrame();
      vdec.close();
      return 0;
    }

    tdl_app::VdecChannel::Status status;
    if (vdec.queryStatus(&status, nullptr)) {
      std::cerr << "vdec_demo: read retry " << (i + 1)
                << " left_bytes=" << status.left_stream_bytes
                << " left_frames=" << status.left_stream_frames
                << " left_pics=" << status.left_pics
                << " started=" << (status.started ? 1 : 0) << "\n";
    } else {
      std::cerr << "vdec_demo: read retry " << (i + 1)
                << " err=" << error << "\n";
    }
  }

  std::cerr << "vdec read failed: " << error << "\n";
  vdec.close();
  return 9;
}
