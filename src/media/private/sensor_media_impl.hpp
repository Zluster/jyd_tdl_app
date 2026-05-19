#pragma once

#include <array>
#include <pthread.h>
#include <sys/types.h>
#include <string>
#include <vector>

#include "cvi_comm_vb.h"
#include "sensor_cfg.h"
#include "tdl_app/sensor_media.hpp"

namespace tdl_app {
namespace private_media {

class SensorMediaImpl {
 public:
  explicit SensorMediaImpl(const SensorMedia::Config &config);
  ~SensorMediaImpl();

  SensorMediaImpl(const SensorMediaImpl &) = delete;
  SensorMediaImpl &operator=(const SensorMediaImpl &) = delete;

  bool open(std::string *error);
  void close();
  bool isOpen() const;

 private:
  struct ActiveVpssOutput {
    SensorMedia::VpssOutputConfig config;
    int common_pool_index = -1;
    bool created = false;
    bool channel_enabled = false;
    bool started = false;
    bool vi_bound = false;
  };

  bool initSys(std::string *error);
  bool startIpCameraHelper(std::string *error);
  bool waitForIpCameraReady(std::string *error);
  void stopIpCameraHelper();
  void resetExistingMediaPath();
  bool configureViVpssMode(std::string *error);
  bool startViDev(std::string *error);
  bool startMipi(std::string *error);
  bool probeSensor(std::string *error);
  bool startViPipe(std::string *error);
  bool startIsp(std::string *error);
  bool configureIsp(int pipe, std::string *error);
  bool startSensorInit(std::string *error);
  bool startViChannel(std::string *error);
  bool startVpss(std::string *error);

  void stopVpss();
  void stopIsp();
  void stopVi();

  SensorMedia::Config config_;
  SENSOR_CFG_S sensor_cfg_ {};
  std::array<bool, VI_MAX_DEV_NUM> vi_dev_enabled_ {};
  std::array<bool, VI_MAX_DEV_NUM> vi_pipe_created_ {};
  std::array<bool, VI_MAX_DEV_NUM> vi_pipe_started_ {};
  std::array<bool, VI_MAX_DEV_NUM> vi_chn_enabled_ {};
  std::array<bool, VI_MAX_DEV_NUM> ae_registered_ {};
  std::array<bool, VI_MAX_DEV_NUM> awb_registered_ {};
  std::array<bool, VI_MAX_DEV_NUM> isp_running_ {};
  std::array<pthread_t, VI_MAX_DEV_NUM> isp_threads_ {};
  std::array<bool, VI_MAX_DEV_NUM> isp_thread_started_ {};
  bool sys_inited_ = false;
  bool vb_inited_ = false;
  bool opened_ = false;
  bool own_sys_ = false;
  bool own_vb_ = false;
  std::vector<ActiveVpssOutput> vpss_outputs_;
  pid_t ipcamera_pid_ = -1;
  bool ipcamera_started_ = false;
};

}  // namespace private_media
}  // namespace tdl_app
