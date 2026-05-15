#include "media/private/sensor_media_impl.hpp"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <signal.h>
#include <thread>
#include <unordered_map>

#include "cvi_buffer.h"
#include "cvi_common.h"
#include "cvi_comm_vb.h"
#include "cvi_comm_sys.h"
#include "cvi_sensor.h"
#include "cvi_sys.h"
#include "cvi_vb.h"
#include "cvi_vi.h"
#include "cvi_vpss.h"
#include "media/private/media_common.hpp"

namespace tdl_app {
namespace private_media {

using detail::addPool;
using detail::setError;

namespace {

using IniSectionMap =
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>>;

std::string trim(const std::string &value) {
  std::size_t begin = 0;
  while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return value.substr(begin, end - begin);
}

std::string toLower(std::string value) {
  for (char &ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

IniSectionMap parseIniFile(const std::string &path) {
  IniSectionMap sections;
  std::ifstream input(path);
  if (!input.is_open()) {
    return sections;
  }

  std::string current_section;
  std::string line;
  while (std::getline(input, line)) {
    const std::size_t comment_pos = line.find_first_of(";#");
    if (comment_pos != std::string::npos) {
      line.erase(comment_pos);
    }
    line = trim(line);
    if (line.empty()) {
      continue;
    }
    if (line.front() == '[' && line.back() == ']') {
      current_section = toLower(trim(line.substr(1, line.size() - 2)));
      continue;
    }
    const std::size_t equal_pos = line.find('=');
    if (equal_pos == std::string::npos || current_section.empty()) {
      continue;
    }
    const std::string key = toLower(trim(line.substr(0, equal_pos)));
    const std::string value = trim(line.substr(equal_pos + 1));
    sections[current_section][key] = value;
  }
  return sections;
}

bool readIniValue(const IniSectionMap &sections, const std::string &section,
                  const std::string &key, std::string *value) {
  const auto sec_it = sections.find(toLower(section));
  if (sec_it == sections.end()) {
    return false;
  }
  const auto key_it = sec_it->second.find(toLower(key));
  if (key_it == sec_it->second.end()) {
    return false;
  }
  if (value) {
    *value = key_it->second;
  }
  return true;
}

bool parseIntValue(const std::string &text, int *value) {
  if (!value) {
    return false;
  }
  char *end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 0);
  if (!end || end == text.c_str()) {
    return false;
  }
  *value = static_cast<int>(parsed);
  return true;
}

void applyLaneList(const std::string &text, int *values, int count) {
  std::size_t begin = 0;
  for (int i = 0; i < count; ++i) {
    const std::size_t comma = text.find(',', begin);
    const std::string token = trim(text.substr(begin, comma - begin));
    int parsed = values[i];
    if (!token.empty() && parseIntValue(token, &parsed)) {
      values[i] = parsed;
    }
    if (comma == std::string::npos) {
      break;
    }
    begin = comma + 1;
  }
}

void applySensorIniOverrides(const std::string &path, SENSOR_CFG_S *sensor_cfg) {
  if (!sensor_cfg) {
    return;
  }

  const IniSectionMap sections = parseIniFile(path);
  if (sections.empty()) {
    return;
  }

  int dev_num = sensor_cfg->sns_ini_cfg.devNum;
  std::string value;
  if (readIniValue(sections, "source", "dev_num", &value)) {
    parseIntValue(value, &dev_num);
  } else if (readIniValue(sections, "sensor_config", "sensor_cnt", &value)) {
    parseIntValue(value, &dev_num);
  }
  if (dev_num > 0 && dev_num <= VI_MAX_DEV_NUM) {
    sensor_cfg->sns_ini_cfg.devNum = dev_num;
  }

  for (int i = 0; i < sensor_cfg->sns_ini_cfg.devNum; ++i) {
    const std::string full_section = "sensor_config" + std::to_string(i);
    const std::string simple_section = "sensor";
    const std::string &section =
        sections.find(full_section) != sections.end() ? full_section : simple_section;

    if (readIniValue(sections, section, "bus_id", &value)) {
      parseIntValue(value, &sensor_cfg->sns_ini_cfg.s32BusId[i]);
    }
    if (readIniValue(sections, section, "sns_i2c_addr", &value)) {
      parseIntValue(value, &sensor_cfg->sns_ini_cfg.s32SnsI2cAddr[i]);
    }
    if (readIniValue(sections, section, "mipi_dev", &value)) {
      parseIntValue(value, &sensor_cfg->sns_ini_cfg.MipiDev[i]);
    }
    if (readIniValue(sections, section, "hw_sync", &value)) {
      int parsed = sensor_cfg->sns_ini_cfg.u8HwSync[i];
      if (parseIntValue(value, &parsed)) {
        sensor_cfg->sns_ini_cfg.u8HwSync[i] = static_cast<CVI_U8>(parsed);
      }
    }
    if (readIniValue(sections, section, "mclk_en", &value)) {
      int parsed = sensor_cfg->sns_ini_cfg.stMclkAttr[i].bMclkEn ? 1 : 0;
      if (parseIntValue(value, &parsed)) {
        sensor_cfg->sns_ini_cfg.stMclkAttr[i].bMclkEn = parsed != 0;
      }
    }
    if (readIniValue(sections, section, "mclk", &value)) {
      int parsed = sensor_cfg->sns_ini_cfg.stMclkAttr[i].u8Mclk;
      if (parseIntValue(value, &parsed)) {
        sensor_cfg->sns_ini_cfg.stMclkAttr[i].u8Mclk = static_cast<CVI_U8>(parsed);
      }
    }
    if (readIniValue(sections, section, "hs_settle_en", &value)) {
      int parsed = sensor_cfg->sns_ini_cfg.bHsettlen[i];
      if (parseIntValue(value, &parsed)) {
        sensor_cfg->sns_ini_cfg.bHsettlen[i] = static_cast<CVI_U8>(parsed);
      }
    }
    if (readIniValue(sections, section, "hs_settle", &value)) {
      int parsed = sensor_cfg->sns_ini_cfg.u8Hsettle[i];
      if (parseIntValue(value, &parsed)) {
        sensor_cfg->sns_ini_cfg.u8Hsettle[i] = static_cast<CVI_U8>(parsed);
      }
    }
    if (readIniValue(sections, section, "orien", &value)) {
      int parsed = sensor_cfg->sns_ini_cfg.u8Orien[i];
      if (parseIntValue(value, &parsed)) {
        sensor_cfg->sns_ini_cfg.u8Orien[i] = static_cast<CVI_U8>(parsed);
      }
    }
    if (readIniValue(sections, section, "rst_port", &value)) {
      parseIntValue(value, &sensor_cfg->sns_ini_cfg.s32RstPort[i]);
    }
    if (readIniValue(sections, section, "rst_pin", &value)) {
      parseIntValue(value, &sensor_cfg->sns_ini_cfg.s32RstPin[i]);
    }
    if (readIniValue(sections, section, "rst_pol", &value)) {
      parseIntValue(value, &sensor_cfg->sns_ini_cfg.s32RstPol[i]);
    }

    if (readIniValue(sections, section, "lane_id", &value)) {
      int lanes[MIPI_LANE_NUM];
      for (int lane = 0; lane < MIPI_LANE_NUM; ++lane) {
        lanes[lane] = sensor_cfg->sns_ini_cfg.as16LaneId[i][lane];
      }
      applyLaneList(value, lanes, MIPI_LANE_NUM);
      for (int lane = 0; lane < MIPI_LANE_NUM; ++lane) {
        sensor_cfg->sns_ini_cfg.as16LaneId[i][lane] = lanes[lane];
      }
    }
    if (readIniValue(sections, section, "pn_swap", &value)) {
      int swaps[MIPI_LANE_NUM];
      for (int lane = 0; lane < MIPI_LANE_NUM; ++lane) {
        swaps[lane] = sensor_cfg->sns_ini_cfg.as8PNSwap[i][lane];
      }
      applyLaneList(value, swaps, MIPI_LANE_NUM);
      for (int lane = 0; lane < MIPI_LANE_NUM; ++lane) {
        sensor_cfg->sns_ini_cfg.as8PNSwap[i][lane] = static_cast<CVI_S8>(swaps[lane]);
      }
    }
    for (int lane = 0; lane < MIPI_LANE_NUM; ++lane) {
      if (readIniValue(sections, section, "laneid" + std::to_string(lane), &value)) {
        int parsed = sensor_cfg->sns_ini_cfg.as16LaneId[i][lane];
        if (parseIntValue(value, &parsed)) {
          sensor_cfg->sns_ini_cfg.as16LaneId[i][lane] = static_cast<CVI_S16>(parsed);
        }
      }
      if (readIniValue(sections, section, "swap" + std::to_string(lane), &value)) {
        int parsed = sensor_cfg->sns_ini_cfg.as8PNSwap[i][lane];
        if (parseIntValue(value, &parsed)) {
          sensor_cfg->sns_ini_cfg.as8PNSwap[i][lane] = static_cast<CVI_S8>(parsed);
        }
      }
    }
  }
}

bool fileContains(const char *path, const char *needle) {
  std::ifstream input(path);
  if (!input.is_open()) {
    return false;
  }
  std::string line;
  while (std::getline(input, line)) {
    if (line.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

int extractRecvPicCount() {
  std::ifstream input("/proc/soph/vi");
  if (!input.is_open()) {
    return -1;
  }
  std::string line;
  bool in_status = false;
  while (std::getline(input, line)) {
    if (line.find("VI CHN STATUS") != std::string::npos) {
      in_status = true;
      continue;
    }
    if (!in_status) {
      continue;
    }
    if (line.find("PipeID") != std::string::npos || line.empty()) {
      continue;
    }
    int pipe_id = 0;
    int chn_id = 0;
    char enable = 'N';
    int frame_rate = 0;
    int int_cnt = 0;
    int recv_pic = 0;
    int lost_frame = 0;
    int vb_fail = 0;
    int width = 0;
    int height = 0;
    if (std::sscanf(line.c_str(), "%d %d %c %d %d %d %d %d %d %d",
                    &pipe_id, &chn_id, &enable, &frame_rate, &int_cnt,
                    &recv_pic, &lost_frame, &vb_fail, &width, &height) == 10) {
      return recv_pic;
    }
  }
  return -1;
}

std::vector<SensorMedia::VpssOutputConfig> effectiveVpssOutputs(
    const SensorMedia::Config &config) {
  if (!config.vpss_outputs.empty()) {
    return config.vpss_outputs;
  }
  if (!config.enable_vpss) {
    return {};
  }

  std::vector<SensorMedia::VpssOutputConfig> outputs;
  outputs.push_back(SensorMedia::vpssOutput(
      config.vpss_group, config.vpss_channel, config.output_width,
      config.output_height, config.output_pixel_format, config.bind_vi_to_vpss));
  return outputs;
}

}  // namespace

SensorMediaImpl::SensorMediaImpl(const SensorMedia::Config &config)
    : config_(config) {
  for (const auto &item : effectiveVpssOutputs(config)) {
    ActiveVpssOutput active;
    active.config = item;
    vpss_outputs_.push_back(active);
  }
}

SensorMediaImpl::~SensorMediaImpl() { close(); }

bool SensorMediaImpl::open(std::string *error) {
  if (opened_) {
    return true;
  }

  std::fprintf(stderr, "sensor_media: open begin\n");
  std::memset(&sensor_cfg_, 0, sizeof(sensor_cfg_));

  if (config_.startup_mode == SensorMedia::StartupMode::AttachExisting) {
    int ret = CVI_SNS_SetIniPath(config_.sensor_ini.c_str());
    if (ret == CVI_SUCCESS) {
      ret = CVI_SNS_ParseIni(&sensor_cfg_);
      if (ret == CVI_SUCCESS) {
        ret = CVI_SNS_GetConfigInfo(&sensor_cfg_);
      }
    }
    if (ret != CVI_SUCCESS) {
      setError(error, "attach-existing parse sensor ini failed, ret=" +
                          std::to_string(ret));
      close();
      return false;
    }

    if (config_.enable_vpss && !startVpss(error)) {
      close();
      return false;
    }
    opened_ = true;
    return true;
  }

  if (config_.use_ipcamera_helper) {
    if (!startIpCameraHelper(error)) {
      close();
      return false;
    }
    opened_ = true;
    return true;
  }

  int ret = CVI_SNS_SetIniPath(config_.sensor_ini.c_str());
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_SNS_SetIniPath failed, ret=" + std::to_string(ret));
    return false;
  }

  ret = CVI_SNS_ParseIni(&sensor_cfg_);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_SNS_ParseIni failed, ret=" + std::to_string(ret));
    return false;
  }
  ret = CVI_SNS_GetConfigInfo(&sensor_cfg_);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_SNS_GetConfigInfo failed, ret=" + std::to_string(ret));
    return false;
  }
  applySensorIniOverrides(config_.sensor_ini, &sensor_cfg_);
  std::fprintf(stderr, "sensor_media: sensor cfg loaded, dev_num=%d\n",
               sensor_cfg_.sns_ini_cfg.devNum);
  ret = CVI_SNS_SetSnsDrvCfg(&sensor_cfg_);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_SNS_SetSnsDrvCfg failed, ret=" + std::to_string(ret));
    return false;
  }

  std::fprintf(stderr, "sensor_media: reset existing path\n");
  resetExistingMediaPath();

  std::fprintf(stderr, "sensor_media: init sys\n");
  if (!initSys(error)) {
    close();
    return false;
  }
  std::fprintf(stderr, "sensor_media: start mipi\n");
  if (!startMipi(error)) {
    close();
    return false;
  }
  std::fprintf(stderr, "sensor_media: probe sensor\n");
  if (!probeSensor(error)) {
    close();
    return false;
  }
  std::fprintf(stderr, "sensor_media: start vi dev\n");
  if (!startViDev(error)) {
    close();
    return false;
  }
  std::fprintf(stderr, "sensor_media: start vi pipe\n");
  if (!startViPipe(error)) {
    close();
    return false;
  }
  std::fprintf(stderr, "sensor_media: start isp\n");
  if (!startIsp(error)) {
    close();
    return false;
  }
  std::fprintf(stderr, "sensor_media: start sensor init\n");
  if (!startSensorInit(error)) {
    close();
    return false;
  }
  std::fprintf(stderr, "sensor_media: start vi channel\n");
  if (!startViChannel(error)) {
    close();
    return false;
  }
  std::fprintf(stderr, "sensor_media: start vpss\n");
  if (!startVpss(error)) {
    close();
    return false;
  }

  std::fprintf(stderr, "sensor_media: open done\n");
  opened_ = true;
  return true;
}

void SensorMediaImpl::close() {
  stopIpCameraHelper();
  stopVpss();
  stopIsp();
  stopVi();
  if (own_vb_ && vb_inited_) {
    CVI_VB_Exit();
    vb_inited_ = false;
  }
  if (own_sys_ && sys_inited_) {
    CVI_SYS_Exit();
    sys_inited_ = false;
  }
  opened_ = false;
}

bool SensorMediaImpl::isOpen() const { return opened_; }

bool SensorMediaImpl::startIpCameraHelper(std::string *error) {
  std::string command = "sh -c '";
  command += config_.ipcamera_binary;
  command += " -i ";
  command += config_.ipcamera_ini;
  command += " >/tmp/tdl_ipcamera_helper.log 2>/tmp/tdl_ipcamera_helper.err & echo $!'";

  FILE *pipe = popen(command.c_str(), "r");
  if (!pipe) {
    setError(error, "failed to launch ipcamera helper");
    return false;
  }
  char buffer[64] = {0};
  const size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, pipe);
  buffer[n] = '\0';
  pclose(pipe);
  ipcamera_pid_ = std::atoi(buffer);
  if (ipcamera_pid_ <= 0) {
    setError(error, "ipcamera helper did not return a valid pid");
    return false;
  }
  ipcamera_started_ = true;
  std::fprintf(stderr, "sensor_media: ipcamera helper started pid=%d ini=%s\n",
               static_cast<int>(ipcamera_pid_), config_.ipcamera_ini.c_str());
  return waitForIpCameraReady(error);
}

bool SensorMediaImpl::waitForIpCameraReady(std::string *error) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(config_.ipcamera_startup_timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (fileContains("/proc/mipi-rx", "raw10") && extractRecvPicCount() > 0) {
      std::fprintf(stderr, "sensor_media: ipcamera helper ready\n");
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  setError(error, "ipcamera helper did not become ready within timeout");
  return false;
}

void SensorMediaImpl::stopIpCameraHelper() {
  if (!ipcamera_started_ || ipcamera_pid_ <= 0) {
    return;
  }
  kill(ipcamera_pid_, SIGTERM);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  kill(ipcamera_pid_, SIGKILL);
  std::system("killall ipcamera >/dev/null 2>&1");
  std::system("killall -9 ipcamera >/dev/null 2>&1");
  ipcamera_pid_ = -1;
  ipcamera_started_ = false;
}

void SensorMediaImpl::resetExistingMediaPath() {
  for (int pipe = 0; pipe < VI_MAX_PIPE_NUM; ++pipe) {
    for (int channel = 0; channel < VI_MAX_CHN_NUM; ++channel) {
      MMF_CHN_S src;
      std::memset(&src, 0, sizeof(src));
      src.enModId = CVI_ID_VI;
      src.s32DevId = pipe;
      src.s32ChnId = channel;

      for (int group = 0; group < VPSS_MAX_GRP_NUM; ++group) {
        MMF_CHN_S dst;
        std::memset(&dst, 0, sizeof(dst));
        dst.enModId = CVI_ID_VPSS;
        dst.s32DevId = group;
        dst.s32ChnId = 0;
        CVI_SYS_UnBind(&src, &dst);
      }

      CVI_VI_DisableChn(pipe, channel);
    }

    CVI_VI_StopPipe(pipe);
    CVI_VI_DestroyPipe(pipe);
  }

  for (int dev = 0; dev < VI_MAX_DEV_NUM; ++dev) {
    CVI_VI_DisableDev(dev);
  }

  for (int group = 0; group < VPSS_MAX_GRP_NUM; ++group) {
    for (int channel = 0; channel < VPSS_MAX_PHY_CHN_NUM; ++channel) {
      CVI_VPSS_DisableChn(group, channel);
    }
    CVI_VPSS_StopGrp(group);
    CVI_VPSS_DestroyGrp(group);
  }
}

bool SensorMediaImpl::initSys(std::string *error) {
  vb_inited_ = false;
  sys_inited_ = false;
  own_vb_ = false;
  own_sys_ = false;

  if (!config_.reuse_existing_vb) {
    CVI_VB_Exit();
  }
  if (!config_.reuse_existing_sys) {
    CVI_SYS_Exit();
  }

  int ret = CVI_VI_SetDevNum(sensor_cfg_.sns_ini_cfg.devNum);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VI_SetDevNum failed, ret=" + std::to_string(ret));
    return false;
  }

  ret = CVI_SYS_Init();
  if (ret != CVI_SUCCESS) {
    if (config_.reuse_existing_sys) {
      std::fprintf(stderr,
                   "sensor_media: CVI_SYS_Init failed(ret=%d), reuse existing system state\n",
                   ret);
    } else {
      setError(error, "CVI_SYS_Init failed, ret=" + std::to_string(ret));
      return false;
    }
  } else {
    sys_inited_ = true;
    own_sys_ = !config_.reuse_existing_sys;
  }

  VB_CONFIG_S vb_config;
  std::memset(&vb_config, 0, sizeof(vb_config));
  for (int i = 0; i < sensor_cfg_.sns_ini_cfg.devNum; ++i) {
    const auto sensor_pixel_format =
        sensor_cfg_.sns_cfg.bBypassIsp[i] ? PIXEL_FORMAT_YUYV : PIXEL_FORMAT_NV21;
    const CVI_U32 sensor_blk_size =
        COMMON_GetPicBufferSize(sensor_cfg_.sns_cfg.u32ImageWigth[i],
                                sensor_cfg_.sns_cfg.u32ImageHeight[i], sensor_pixel_format,
                                DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
    const CVI_U32 sensor_blk_cnt =
        sensor_cfg_.sns_cfg.enFormatMode[i] == SNS_DATA_TYPE_YUV
            ? static_cast<CVI_U32>(3 * (sensor_cfg_.sns_cfg.enChnMode[i] + 1))
            : 5;
    if (!addPool(&vb_config, sensor_blk_size, sensor_blk_cnt)) {
      setError(error, "VB pool configuration overflow");
      return false;
    }

    // Align with the working ipcamera graph: keep a full-resolution NV12 pool
    // for the post-ISP VI output even when the application later reads VPSS.
    const CVI_U32 vi_blk_size =
        COMMON_GetPicBufferSize(sensor_cfg_.sns_cfg.u32ImageWigth[i],
                                sensor_cfg_.sns_cfg.u32ImageHeight[i], PIXEL_FORMAT_NV12,
                                DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
    if (!addPool(&vb_config, vi_blk_size, 4)) {
      setError(error, "VB pool configuration overflow");
      return false;
    }
  }

  if (config_.enable_vpss) {
    for (const auto &output : vpss_outputs_) {
      const CVI_U32 vpss_blk_size =
          COMMON_GetPicBufferSize(
              static_cast<CVI_U32>(output.config.output_width),
              static_cast<CVI_U32>(output.config.output_height),
              static_cast<PIXEL_FORMAT_E>(output.config.output_pixel_format),
              DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
      if (!addPool(&vb_config, vpss_blk_size,
                   static_cast<CVI_U32>(output.config.vb_block_count))) {
        setError(error, "VB pool configuration overflow");
        return false;
      }
    }
  }

  if (vb_config.u32MaxPoolCnt == 1) {
    vb_config.astCommPool[0].u32BlkCnt += 2;
  }

  ret = CVI_VB_SetConfig(&vb_config);
  if (ret != CVI_SUCCESS) {
    if (config_.reuse_existing_vb) {
      std::fprintf(stderr,
                   "sensor_media: CVI_VB_SetConfig failed(ret=%d), reuse existing VB pools\n",
                   ret);
    } else {
      setError(error, "CVI_VB_SetConfig failed, ret=" + std::to_string(ret));
      return false;
    }
  } else {
    ret = CVI_VB_Init();
    if (ret != CVI_SUCCESS) {
      if (config_.reuse_existing_vb) {
        std::fprintf(stderr,
                     "sensor_media: CVI_VB_Init failed(ret=%d), reuse existing VB state\n",
                     ret);
      } else {
        setError(error, "CVI_VB_Init failed, ret=" + std::to_string(ret));
        return false;
      }
    } else {
      vb_inited_ = true;
      own_vb_ = !config_.reuse_existing_vb;
    }
  }

  if (!configureViVpssMode(error)) {
    return false;
  }

  return true;
}

bool SensorMediaImpl::configureViVpssMode(std::string *error) {
  VI_VPSS_MODE_S vi_vpss_mode;
  std::memset(&vi_vpss_mode, 0, sizeof(vi_vpss_mode));

  const VI_VPSS_MODE_E pipe_mode =
      config_.online_vpss ? VI_OFFLINE_VPSS_ONLINE : VI_OFFLINE_VPSS_OFFLINE;
  std::fprintf(stderr,
               "sensor_media: configure vi/vpss mode online_vpss=%d pipe_mode=%d dev_num=%d\n",
               config_.online_vpss ? 1 : 0, static_cast<int>(pipe_mode),
               sensor_cfg_.sns_ini_cfg.devNum);
  for (int i = 0; i < sensor_cfg_.sns_ini_cfg.devNum; ++i) {
    vi_vpss_mode.aenMode[i] = pipe_mode;
  }

  int ret = CVI_SYS_SetVIVPSSMode(&vi_vpss_mode);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_SYS_SetVIVPSSMode failed, ret=" + std::to_string(ret));
    return false;
  }

  VPSS_MODE_S vpss_mode;
  std::memset(&vpss_mode, 0, sizeof(vpss_mode));
  vpss_mode.enMode = VPSS_MODE_DUAL;
  vpss_mode.aenInput[0] = VPSS_INPUT_MEM;
  vpss_mode.aenInput[1] = config_.online_vpss ? VPSS_INPUT_ISP : VPSS_INPUT_MEM;
  std::fprintf(stderr,
               "sensor_media: configure vpss mode enMode=%d input0=%d input1=%d\n",
               static_cast<int>(vpss_mode.enMode),
               static_cast<int>(vpss_mode.aenInput[0]),
               static_cast<int>(vpss_mode.aenInput[1]));

  ret = CVI_VPSS_SetMode(&vpss_mode);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VPSS_SetMode failed, ret=" + std::to_string(ret));
    return false;
  }

  return true;
}

}  // namespace private_media
}  // namespace tdl_app
