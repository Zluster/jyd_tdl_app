#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "tdl_app/advanced.hpp"

namespace {

struct Options {
  int width = 640;
  int height = 640;
  int pixel_format = 18;
  int common_blocks = 4;
  int dynamic_blocks = 2;
  int venc_channel = 0;
  int vdec_channel = 0;
  int osd_handle = 0;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_media_smoke_demo [--width N] [--height N] [--pixel-format N]\n"
      << "                       [--common-blocks N] [--dynamic-blocks N]\n"
      << "                       [--venc-channel N] [--vdec-channel N]\n"
      << "                       [--osd-handle N]\n";
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

    if (arg == "--width") {
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
    } else if (arg == "--common-blocks") {
      const char *v = value("--common-blocks");
      if (!v) return false;
      opt->common_blocks = std::atoi(v);
    } else if (arg == "--dynamic-blocks") {
      const char *v = value("--dynamic-blocks");
      if (!v) return false;
      opt->dynamic_blocks = std::atoi(v);
    } else if (arg == "--venc-channel") {
      const char *v = value("--venc-channel");
      if (!v) return false;
      opt->venc_channel = std::atoi(v);
    } else if (arg == "--vdec-channel") {
      const char *v = value("--vdec-channel");
      if (!v) return false;
      opt->vdec_channel = std::atoi(v);
    } else if (arg == "--osd-handle") {
      const char *v = value("--osd-handle");
      if (!v) return false;
      opt->osd_handle = std::atoi(v);
    } else if (arg == "-h" || arg == "--help") {
      printUsage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
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

  std::string error;

  tdl_app::SysContext sys;
  if (!sys.open(&error)) {
    std::cerr << "sys open failed: " << error << "\n";
    return 2;
  }

  std::string version;
  std::uint32_t chip_id = 0;
  std::uint64_t pts_us = 0;
  if (sys.getVersion(&version, &error)) {
    std::cout << "sys: version=" << version << "\n";
  }
  if (sys.getChipId(&chip_id, &error)) {
    std::cout << "sys: chip_id=" << chip_id << "\n";
  }
  if (sys.getCurrentPts(&pts_us, &error)) {
    std::cout << "sys: pts_us=" << pts_us << "\n";
  }

  tdl_app::VideoBufferManager::Config vb_manager_config;
  vb_manager_config.common_pools.push_back(
      tdl_app::VideoBufferPoolConfig{{opt.width, opt.height},
                                     opt.pixel_format,
                                     opt.common_blocks,
                                     64,
                                     true});
  tdl_app::VideoBufferManager vb_manager(vb_manager_config);
  if (!vb_manager.open(&error)) {
    std::cerr << "vb manager open failed: " << error << "\n";
    return 3;
  }
  std::cout << "vb: common pools ready\n";

  tdl_app::VideoBufferPool::Config pool_config;
  pool_config.size = {opt.width, opt.height};
  pool_config.pixel_format = opt.pixel_format;
  pool_config.block_count = opt.dynamic_blocks;
  pool_config.name = "tdl_smoke_pool";
  tdl_app::VideoBufferPool pool(pool_config);
  if (!pool.create(&error)) {
    std::cerr << "vb pool create failed: " << error << "\n";
    return 4;
  }

  tdl_app::VideoBufferBlock block;
  if (!pool.acquire(&block, &error)) {
    std::cerr << "vb block acquire failed: " << error << "\n";
    return 5;
  }
  if (!pool.map(&block, &error)) {
    std::cerr << "vb block map failed: " << error << "\n";
    return 6;
  }
  std::cout << "vb: pool=" << block.pool_id
            << " size=" << block.size
            << " phys=0x" << std::hex << block.physical << std::dec << "\n";
  if (!pool.release(&block, &error)) {
    std::cerr << "vb block release failed: " << error << "\n";
    return 7;
  }

  tdl_app::VencChannel::Config venc_config;
  venc_config.channel = opt.venc_channel;
  venc_config.width = opt.width;
  venc_config.height = opt.height;
  std::cerr << "smoke: open venc\n";
  tdl_app::VencChannel venc(venc_config);
  if (!venc.open(&error)) {
    std::cerr << "venc open failed: " << error << "\n";
    return 8;
  }
  std::cout << "venc: channel " << opt.venc_channel << " opened\n";
  venc.close();

  tdl_app::VdecChannel::Config vdec_config;
  vdec_config.channel = opt.vdec_channel;
  vdec_config.width = opt.width;
  vdec_config.height = opt.height;
  std::cerr << "smoke: open vdec\n";
  tdl_app::VdecChannel vdec(vdec_config);
  if (!vdec.open(&error)) {
    std::cerr << "vdec open failed: " << error << "\n";
    return 9;
  }
  std::cerr << "smoke: query vdec status\n";
  tdl_app::VdecChannel::Status status;
  if (vdec.queryStatus(&status, &error)) {
    std::cout << "vdec: channel " << opt.vdec_channel
              << " started=" << (status.started ? 1 : 0)
              << " width=" << status.width
              << " height=" << status.height << "\n";
  } else {
    std::cerr << "vdec query status failed: " << error << "\n";
  }
  std::cerr << "smoke: close vdec\n";
  vdec.close();

  tdl_app::OsdRegion::Config osd_config;
  osd_config.handle = opt.osd_handle;
  osd_config.size = {64, 32};
  osd_config.pixel_format = 4;
  std::cerr << "smoke: create osd\n";
  tdl_app::OsdRegion osd(osd_config);
  if (!osd.create(&error)) {
    std::cerr << "osd create failed: " << error << "\n";
    return 10;
  }
  std::cerr << "smoke: create osd done\n";
  std::cout << "osd: region " << opt.osd_handle << " created\n";
  std::cerr << "smoke: destroy osd\n";
  osd.destroy();
  std::cerr << "smoke: destroy osd done\n";

  pool.destroy();
  vb_manager.close();
  sys.close();
  std::cout << "media smoke: success\n";
  return 0;
}
