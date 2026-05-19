#pragma once

#include <string>
#include <vector>

namespace tdl_app {

class SensorMedia {
 public:
  enum class StartupMode {
    FullStack,
    AttachExisting,
  };

  struct VpssOutputConfig {
    int group = 0;
    int channel = 0;
    int output_width = 640;
    int output_height = 640;
    int output_pixel_format = 18;
    bool bind_from_vi = true;
    int vb_block_count = 4;
  };

  struct Config {
    std::string sensor_ini = "/mnt/data/sensor_cfg.ini";
    int vi_device = 0;
    int vi_pipe = 0;
    int vi_channel = 0;
    int vpss_group = 0;
    int vpss_channel = 0;
    int output_width = 640;
    int output_height = 640;
    int output_pixel_format = 18;
    bool enable_vpss = true;
    bool bind_vi_to_vpss = true;
    bool online_vpss = false;
    bool reuse_existing_sys = true;
    bool reuse_existing_vb = true;
    bool use_ipcamera_helper = false;
    std::string ipcamera_binary = "/mnt/sd/install/ipcamera";
    std::string ipcamera_ini = "/mnt/sd/install/cv1842hp_wevb_cv2003.ini";
    int ipcamera_startup_timeout_ms = 5000;
    std::vector<VpssOutputConfig> vpss_outputs;
    StartupMode startup_mode = StartupMode::FullStack;
  };

  SensorMedia();
  explicit SensorMedia(const Config &config);
  ~SensorMedia();

  SensorMedia(const SensorMedia &) = delete;
  SensorMedia &operator=(const SensorMedia &) = delete;

  bool open(std::string *error = nullptr);
  void close();
  bool isOpen() const;

  static VpssOutputConfig vpssOutput(int group = 0, int channel = 0,
                                     int output_width = 640,
                                     int output_height = 640,
                                     int output_pixel_format = 18,
                                     bool bind_from_vi = true,
                                     int vb_block_count = 4) {
    VpssOutputConfig config;
    config.group = group;
    config.channel = channel;
    config.output_width = output_width;
    config.output_height = output_height;
    config.output_pixel_format = output_pixel_format;
    config.bind_from_vi = bind_from_vi;
    config.vb_block_count = vb_block_count;
    return config;
  }

  static Config fullStackSensor(const std::string &sensor_ini,
                                bool use_vpss_backend = true,
                                int vi_device = 0, int vi_pipe = 0,
                                int vi_channel = 0, int vpss_group = 0,
                                int vpss_channel = 0, int output_width = 640,
                                int output_height = 640,
                                int output_pixel_format = 18) {
    Config config;
    config.sensor_ini = sensor_ini;
    config.vi_device = vi_device;
    config.vi_pipe = vi_pipe;
    config.vi_channel = vi_channel;
    config.vpss_group = vpss_group;
    config.vpss_channel = vpss_channel;
    config.output_width = output_width;
    config.output_height = output_height;
    config.output_pixel_format = output_pixel_format;
    // Keep a live VPSS group even when the application reads VI directly.
    // The working ipcamera graph uses an online VI->VPSS path (grp0/ch0),
    // and bringing up the same path helps align the media topology.
    config.enable_vpss = true;
    config.bind_vi_to_vpss = true;
    // Match the working ipcamera graph: VI runs in OFFLINE->ONLINE mode and
    // VPSS dev1 consumes ISP output even when the application later reads VI.
    config.online_vpss = true;
    // Full-stack startup should rebuild SYS/VB from a clean state instead of
    // inheriting a previous ipcamera graph.
    config.reuse_existing_sys = false;
    config.reuse_existing_vb = false;
    config.vpss_outputs.push_back(
        vpssOutput(vpss_group, vpss_channel, output_width, output_height,
                   output_pixel_format, true));
    config.startup_mode = StartupMode::FullStack;
    return config;
  }

  static Config attachExisting(const std::string &sensor_ini,
                               bool use_vpss_backend = true,
                               int vi_device = 0, int vi_pipe = 0,
                               int vi_channel = 0, int vpss_group = 0,
                               int vpss_channel = 0, int output_width = 640,
                               int output_height = 640,
                               int output_pixel_format = 18) {
    Config config =
        fullStackSensor(sensor_ini, use_vpss_backend, vi_device, vi_pipe,
                        vi_channel, vpss_group, vpss_channel, output_width,
                        output_height, output_pixel_format);
    config.startup_mode = StartupMode::AttachExisting;
    return config;
  }

  static Config ipcameraHelper(const std::string &sensor_ini,
                               const std::string &ipcamera_binary,
                               const std::string &ipcamera_ini,
                               bool use_vpss_backend = true,
                               int vi_device = 0, int vi_pipe = 0,
                               int vi_channel = 0, int vpss_group = 0,
                               int vpss_channel = 0, int output_width = 640,
                               int output_height = 640,
                               int output_pixel_format = 18) {
    Config config =
        fullStackSensor(sensor_ini, use_vpss_backend, vi_device, vi_pipe,
                        vi_channel, vpss_group, vpss_channel, output_width,
                        output_height, output_pixel_format);
    config.use_ipcamera_helper = true;
    config.ipcamera_binary = ipcamera_binary;
    config.ipcamera_ini = ipcamera_ini;
    return config;
  }

 private:
  class Impl;
  Config config_;
  Impl *impl_ = nullptr;
};

}  // namespace tdl_app
