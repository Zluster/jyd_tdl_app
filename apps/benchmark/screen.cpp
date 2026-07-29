#include "screen.hpp"

#include <cstring>

#include "cvi_region.h"
#include "cvi_sys.h"
#include "tdl_app/layout.hpp"
#include "tdl_app/media_types.hpp"

namespace tdl_bench {
namespace {

tdl_app::MediaChannel displayOutputChannel() {
  return tdl_app::MediaChannel::vpss(tdl_app::DualOsLayout::kDisplayVpssGroup,
                                     tdl_app::DualOsLayout::kDisplayChannel);
}

MOD_ID_E toModId(tdl_app::MediaModule module) {
  switch (module) {
    case tdl_app::MediaModule::Vi:
      return CVI_ID_VI;
    case tdl_app::MediaModule::Vpss:
      return CVI_ID_VPSS;
    case tdl_app::MediaModule::Venc:
      return CVI_ID_VENC;
    case tdl_app::MediaModule::Vo:
      return CVI_ID_VO;
    case tdl_app::MediaModule::Vdec:
      return CVI_ID_VDEC;
    default:
      return CVI_ID_BUTT;
  }
}

MMF_CHN_S toMmfChn(const tdl_app::MediaChannel &channel) {
  MMF_CHN_S chn;
  std::memset(&chn, 0, sizeof(chn));
  chn.enModId = toModId(channel.module);
  chn.s32DevId = channel.device;
  chn.s32ChnId = channel.channel;
  return chn;
}

// 防御性解绑：清掉上次异常退出残留的同源/同目的绑定（返回值忽略）。
void unbindStale(const tdl_app::MediaChannel &src,
                 const tdl_app::MediaChannel &dst) {
  MMF_CHN_S s = toMmfChn(src);
  MMF_CHN_S d = toMmfChn(dst);
  CVI_SYS_UnBind(&s, &d);
}

bool sameChannel(const MMF_CHN_S &lhs, const MMF_CHN_S &rhs) {
  return lhs.enModId == rhs.enModId && lhs.s32DevId == rhs.s32DevId &&
         lhs.s32ChnId == rhs.s32ChnId;
}

// 清掉上次异常退出残留的同号 RGN：先从 grp1/ch0 解绑再销毁。
void purgeStaleRegion(int handle) {
  MMF_CHN_S chn = toMmfChn(displayOutputChannel());
  CVI_RGN_DetachFromChn(handle, &chn);
  CVI_RGN_Destroy(handle);
}

}  // namespace

ScreenDisplay::ScreenDisplay() = default;

ScreenDisplay::~ScreenDisplay() { close(); }

bool ScreenDisplay::open(const Config &config, std::string *error) {
  // 1) live 相机：dual 默认 attach 小核已有的 grp0/live 通道。
  if (!camera_demo_support::setCameraPreset(&camera_options_, "live", error)) {
    return false;
  }
  if (!camera_demo_support::openCameraRuntime(camera_options_, &runtime_,
                                              error)) {
    return false;
  }

  // 2) 打开屏幕尺寸 VO（旋转走 DualOsLayout 默认 90°）。
  tdl_app::VoOutput::Config vo_config;
  vo_config.device = config.vo_dev;
  vo_config.layer = config.layer;
  vo_config.channel = config.vo_chn;
  vo_config.width = config.screen_width;
  vo_config.height = config.screen_height;
  vo_config.pixel_format = tdl_app::PixelFormat::NV12;
  vo_config.interface_type = config.interface_type;
  vo_config.interface_sync = config.interface_sync;
  vo_.reset(new tdl_app::VoOutput(vo_config));
  if (!vo_->open(error)) {
    close();
    return false;
  }

  // 3) 复用小核的 grp0(live) -> grp1/ch0，只创建缺失的绑定。
  const tdl_app::MediaChannel preview_src = camera_demo_support::previewChannel(
      camera_options_, runtime_.camera.config());
  const tdl_app::MediaChannel display_dst =
      tdl_app::MediaChannel::vo(config.layer, config.vo_chn);

  const MMF_CHN_S expected_preview_src = toMmfChn(preview_src);
  const MMF_CHN_S preview_dst = toMmfChn(displayOutputChannel());
  MMF_CHN_S current_preview_src;
  std::memset(&current_preview_src, 0, sizeof(current_preview_src));
  if (CVI_SYS_GetBindbyDest(&preview_dst, &current_preview_src) == CVI_SUCCESS) {
    if (!sameChannel(current_preview_src, expected_preview_src)) {
      if (error) *error = "display VPSS input is bound to an unexpected source";
      close();
      return false;
    }
  } else {
    const int ret = CVI_SYS_Bind(&expected_preview_src, &preview_dst);
    if (ret != CVI_SUCCESS) {
      if (error) {
        *error = "CVI_SYS_Bind display VPSS input failed, ret=" +
                 std::to_string(ret);
      }
      close();
      return false;
    }
  }

  unbindStale(displayOutputChannel(), display_dst);
  display_link_.reset(
      new tdl_app::MediaLink({displayOutputChannel(), display_dst}));
  if (!display_link_->bind(error)) {
    close();
    return false;
  }

  // 4) grp1/ch0 上挂全屏 ARGB OSD（create 前清残留同号 RGN）。
  purgeStaleRegion(config.osd_handle);
  canvas_width_ = tdl_app::DualOsLayout::kLiveWidth;
  canvas_height_ = tdl_app::DualOsLayout::kLiveHeight;
  osd_.reset(new tdl_app::OsdRegion(tdl_app::OsdRegion::canvas(
      config.osd_handle, canvas_width_, canvas_height_,
      tdl_app::PixelFormat::ARGB8888, 2, 0)));
  if (!osd_->create(error)) {
    close();
    return false;
  }
  if (!osd_->attach(displayOutputChannel(), 0, 0, 10, error)) {
    close();
    return false;
  }
  return true;
}

void ScreenDisplay::close() {
  if (osd_) {
    osd_->detach();
    osd_->destroy();
    osd_.reset();
  }
  if (display_link_) {
    display_link_->unbind();
    display_link_.reset();
  }
  if (preview_link_) {
    preview_link_->unbind();
    preview_link_.reset();
  }
  if (vo_) {
    vo_->close();
    vo_.reset();
  }
  camera_demo_support::closeCameraRuntime(&runtime_);
  canvas_width_ = 0;
  canvas_height_ = 0;
}

bool ScreenDisplay::showColor(std::uint32_t argb, std::string *error) {
  if (!osd_) {
    if (error) *error = "screen display not open";
    return false;
  }
  tdl_app::OsdCanvas canvas;
  if (!osd_->getCanvas(&canvas, error)) {
    return false;
  }
  if (!canvas.data || canvas.width <= 0 || canvas.height <= 0) {
    if (error) *error = "invalid osd canvas";
    return false;
  }
  for (int y = 0; y < canvas.height; ++y) {
    auto *row = reinterpret_cast<std::uint32_t *>(
        static_cast<std::uint8_t *>(canvas.data) +
        static_cast<std::size_t>(y) * canvas.stride);
    for (int x = 0; x < canvas.width; ++x) {
      row[x] = argb;
    }
  }
  if (!osd_->updateCanvas(error)) {
    return false;
  }
  return osd_->setVisible(true, error);
}

}  // namespace tdl_bench
