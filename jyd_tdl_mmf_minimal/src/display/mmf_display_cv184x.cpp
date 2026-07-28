#include <algorithm>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "mmf_cv184x_common.hpp"

using namespace mmf_cv184x;

struct mmf_display {
  mmf_display_config_t config;
  mmf_cvi::Display display;
  std::unique_ptr<mmf_cvi::OsdRegion> osd;
  uint32_t osd_handle = 0;
  std::string overlay_path;
  mmf_rect_t overlay_rect{};
  bool overlay_active = false;
  mmf_display_status_t status{};
};

namespace {

mmf_cvi::Display::Input to_display_input(mmf_camera_source_t source) {
  switch (source) {
    case MMF_CAMERA_SRC_AI:
      return mmf_cvi::Display::Input::Ai;
    case MMF_CAMERA_SRC_MAIN:
      return mmf_cvi::Display::Input::Main;
    case MMF_CAMERA_SRC_SUBRGB:
      return mmf_cvi::Display::Input::SubRgb;
    case MMF_CAMERA_SRC_LIVE:
    case MMF_CAMERA_SRC_SCREEN:
      return mmf_cvi::Display::Input::Screen;
    default:
      return mmf_cvi::Display::Input::Live;
  }
}

mmf_cvi::MediaChannel display_output_channel() {
  return mmf_cvi::MediaChannel::vpss(mmf_cvi::DualOsLayout::kDisplayVpssGroup,
                                     mmf_cvi::DualOsLayout::kDisplayChannel);
}

void purge_osd_region(uint32_t handle) {
  if (handle == 0) {
    return;
  }

  const int capture_channels[] = {
      mmf_cvi::DualOsLayout::kMainChannel,
      mmf_cvi::DualOsLayout::kAiChannel,
      mmf_cvi::DualOsLayout::kLiveChannel,
      mmf_cvi::DualOsLayout::kSubRgbChannel,
  };
  for (size_t i = 0; i < sizeof(capture_channels) / sizeof(capture_channels[0]); ++i) {
    MMF_CHN_S chn;
    std::memset(&chn, 0, sizeof(chn));
    chn.enModId = CVI_ID_VPSS;
    chn.s32DevId = mmf_cvi::DualOsLayout::kCaptureVpssGroup;
    chn.s32ChnId = capture_channels[i];
    (void)CVI_RGN_DetachFromChn(static_cast<RGN_HANDLE>(handle), &chn);
  }
  MMF_CHN_S chn;
  std::memset(&chn, 0, sizeof(chn));
  chn.enModId = CVI_ID_VPSS;
  chn.s32DevId = mmf_cvi::DualOsLayout::kDisplayVpssGroup;
  chn.s32ChnId = mmf_cvi::DualOsLayout::kDisplayChannel;
  (void)CVI_RGN_DetachFromChn(static_cast<RGN_HANDLE>(handle), &chn);
  (void)CVI_RGN_Destroy(static_cast<RGN_HANDLE>(handle));
}

}  // namespace

static mmf_result_t show_image_via_osd(mmf_display_t* display, const char* path,
                                       const mmf_display_show_options_t* options) {
  if (display == nullptr || path == nullptr)
    return MMF_EINVAL;
  cv::Mat image = cv::imread(path, cv::IMREAD_UNCHANGED);
  if (image.empty()) {
    set_last_error(std::string("failed to load image: ") + path);
    return MMF_EIO;
  }

  int width = options && options->dst_rect.width > 0 ? static_cast<int>(options->dst_rect.width)
                                                     : image.cols;
  int height = options && options->dst_rect.height > 0 ? static_cast<int>(options->dst_rect.height)
                                                       : image.rows;
  int x = options ? options->dst_rect.x : 0;
  int y = options ? options->dst_rect.y : 0;
  if (width <= 0 || height <= 0)
    return MMF_EINVAL;

  cv::Mat bgra;
  if (image.channels() == 4) {
    bgra = image;
  } else if (image.channels() == 3) {
    cv::cvtColor(image, bgra, cv::COLOR_BGR2BGRA);
  } else if (image.channels() == 1) {
    cv::cvtColor(image, bgra, cv::COLOR_GRAY2BGRA);
  } else {
    set_last_error("unsupported image channel count");
    return MMF_EINVAL;
  }
  if (bgra.cols != width || bgra.rows != height) {
    cv::resize(bgra, bgra, cv::Size(width, height), 0.0, 0.0, cv::INTER_NEAREST);
  }

  const uint32_t handle = options && options->osd_handle != 0 ? options->osd_handle : 122;
  if (display->osd && display->osd_handle != handle) {
    display->osd->detach();
    display->osd->destroy();
    display->osd.reset();
  }
  if (!display->osd) {
    display->osd.reset(new mmf_cvi::OsdRegion(mmf_cvi::OsdRegion::canvas(
        static_cast<int>(handle), width, height, mmf_cvi::PixelFormat::ARGB8888, 2, 0)));
    std::string error;
    if (!display->osd->create(&error)) {
      purge_osd_region(handle);
      if (!display->osd->create(&error))
        return ok_or_error(false, error);
    }
    const int layer = options ? static_cast<int>(options->osd_layer) : 10;
    mmf_cvi::MediaChannel osd_channel = display_output_channel();
    if (display->status.mode == MMF_DISPLAY_MODE_CAMERA_BIND) {
      int group = 0;
      int channel = 0;
      if (source_to_vpss(display->status.bound_camera_source, &group, &channel)) {
        osd_channel = mmf_cvi::MediaChannel::vpss(group, channel);
      }
    }
    if (!display->osd->attach(osd_channel, x, y, layer, &error)) {
      return ok_or_error(false, error);
    }
    display->osd_handle = handle;
  }

  mmf_cvi::OsdCanvas canvas;
  std::string error;
  if (!display->osd->getCanvas(&canvas, &error)) {
    return ok_or_error(false, error);
  }
  if (canvas.data == nullptr || canvas.width != width || canvas.height != height ||
      canvas.stride < width * 4) {
    set_last_error("invalid osd canvas");
    return MMF_EIO;
  }

  for (int row = 0; row < height; ++row) {
    auto* dst =
        reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(canvas.data) + row * canvas.stride);
    const cv::Vec4b* src = bgra.ptr<cv::Vec4b>(row);
    for (int col = 0; col < width; ++col) {
      const uint8_t b = src[col][0];
      const uint8_t g = src[col][1];
      const uint8_t r = src[col][2];
      const uint8_t a = src[col][3];
      dst[col] = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
                 (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
    }
  }
  if (!display->osd->updateCanvas(&error))
    return ok_or_error(false, error);
  if (!display->osd->setVisible(true, &error))
    return ok_or_error(false, error);
  return MMF_OK;
}

static mmf_result_t compose_overlay_snapshot(const char* base_path, const char* out_path,
                                             const char* overlay_path, const mmf_rect_t& rect,
                                             uint32_t quality) {
  cv::Mat base = cv::imread(base_path, cv::IMREAD_COLOR);
  cv::Mat overlay = cv::imread(overlay_path, cv::IMREAD_UNCHANGED);
  if (base.empty() || overlay.empty()) {
    set_last_error("failed to load snapshot/overlay for composition");
    return MMF_EIO;
  }

  const int x = rect.x;
  const int y = rect.y;
  const int width = rect.width > 0 ? static_cast<int>(rect.width) : overlay.cols;
  const int height = rect.height > 0 ? static_cast<int>(rect.height) : overlay.rows;
  if (width <= 0 || height <= 0 || x >= base.cols || y >= base.rows || x + width <= 0 ||
      y + height <= 0) {
    set_last_error("overlay rect is outside snapshot");
    return MMF_EINVAL;
  }

  if (overlay.cols != width || overlay.rows != height) {
    cv::resize(overlay, overlay, cv::Size(width, height), 0.0, 0.0, cv::INTER_NEAREST);
  }
  if (overlay.channels() == 3) {
    cv::cvtColor(overlay, overlay, cv::COLOR_BGR2BGRA);
  } else if (overlay.channels() == 1) {
    cv::cvtColor(overlay, overlay, cv::COLOR_GRAY2BGRA);
  } else if (overlay.channels() != 4) {
    set_last_error("unsupported overlay channel count");
    return MMF_EINVAL;
  }

  const int copy_x0 = std::max(0, x);
  const int copy_y0 = std::max(0, y);
  const int copy_x1 = std::min(base.cols, x + width);
  const int copy_y1 = std::min(base.rows, y + height);
  for (int row = copy_y0; row < copy_y1; ++row) {
    cv::Vec3b* dst = base.ptr<cv::Vec3b>(row);
    const cv::Vec4b* src = overlay.ptr<cv::Vec4b>(row - y);
    for (int col = copy_x0; col < copy_x1; ++col) {
      const cv::Vec4b px = src[col - x];
      const float alpha = px[3] / 255.0f;
      dst[col][0] = static_cast<uint8_t>(dst[col][0] * (1.0f - alpha) + px[0] * alpha);
      dst[col][1] = static_cast<uint8_t>(dst[col][1] * (1.0f - alpha) + px[1] * alpha);
      dst[col][2] = static_cast<uint8_t>(dst[col][2] * (1.0f - alpha) + px[2] * alpha);
    }
  }

  std::vector<int> params;
  params.push_back(cv::IMWRITE_JPEG_QUALITY);
  params.push_back(quality > 0 ? static_cast<int>(quality) : 92);
  if (!cv::imwrite(out_path, base, params)) {
    set_last_error(std::string("failed to write snapshot: ") + out_path);
    return MMF_EIO;
  }
  return MMF_OK;
}

extern "C" {

mmf_result_t mmf_display_open(const mmf_display_config_t* config, mmf_display_t** display) {
  if (config == nullptr || display == nullptr)
    return MMF_EINVAL;
  std::unique_ptr<mmf_display_t> ptr(new mmf_display_t);
  ptr->config = *config;
  std::string error;
  if (!ptr->display.open(&error)) {
    return ok_or_error(false, error);
  }
  ptr->status.opened = MMF_TRUE;
  ptr->status.layer = config->layer;
  ptr->status.channel = config->channel;
  ptr->status.window = config->window;
  *display = ptr.release();
  return MMF_OK;
}

void mmf_display_close(mmf_display_t* display) {
  if (display == nullptr)
    return;
  if (display->osd) {
    display->osd->detach();
    display->osd->destroy();
    display->osd.reset();
  }
  display->display.close();
  delete display;
}

void mmf_display_get_default_config(mmf_display_config_t* config) {
  if (config == nullptr)
    return;
  std::memset(config, 0, sizeof(*config));
  config->panel_width = mmf_cvi::DualOsLayout::kScreenWidth;
  config->panel_height = mmf_cvi::DualOsLayout::kScreenHeight;
  config->layer = 0;
  config->channel = mmf_cvi::DualOsLayout::kVoChannel;
  config->scale_mode = MMF_SCALE_FIT_BLACK;
  config->window.width = config->panel_width;
  config->window.height = config->panel_height;
}

void mmf_display_get_default_show_options(mmf_display_show_options_t* options) {
  if (options == nullptr)
    return;
  std::memset(options, 0, sizeof(*options));
  options->target = MMF_DISPLAY_TARGET_VO;
  options->dst_rect.width = mmf_cvi::DualOsLayout::kScreenWidth;
  options->dst_rect.height = mmf_cvi::DualOsLayout::kScreenHeight;
}

mmf_result_t mmf_display_get_status(mmf_display_t* display, mmf_display_status_t* status) {
  if (display == nullptr || status == nullptr)
    return MMF_EINVAL;
  *status = display->status;
  return MMF_OK;
}

mmf_result_t mmf_display_set_window(mmf_display_t* display, const mmf_rect_t* window,
                                    mmf_scale_mode_t scale_mode, uint32_t bg_color) {
  if (display == nullptr || window == nullptr)
    return MMF_EINVAL;
  display->config.window = *window;
  display->config.scale_mode = scale_mode;
  display->config.bg_color = bg_color;
  display->status.window = *window;
  return MMF_OK;
}

mmf_result_t mmf_display_bind_camera(mmf_display_t* display, mmf_camera_source_t source) {
  if (display == nullptr)
    return MMF_EINVAL;
  std::string error;
  bool ok = display->display.show(to_display_input(source), &error);
  if (!ok)
    return ok_or_error(false, error);
  display->status.showing = MMF_TRUE;
  display->status.mode = MMF_DISPLAY_MODE_CAMERA_BIND;
  display->status.bound_camera_source = source;
  return MMF_OK;
}

mmf_result_t mmf_display_unbind(mmf_display_t* display) {
  if (display == nullptr)
    return MMF_EINVAL;
  std::string error;
  bool ok = display->display.hideLive(&error);
  if (!ok)
    return ok_or_error(false, error);
  display->status.showing = MMF_FALSE;
  display->status.mode = MMF_DISPLAY_MODE_NONE;
  return MMF_OK;
}

mmf_result_t mmf_display_show_frame(mmf_display_t* display, const mmf_video_frame_t* frame,
                                    const mmf_display_show_options_t* options) {
  if (display == nullptr || frame == nullptr || frame->priv == nullptr) {
    return MMF_EINVAL;
  }
  if (options != nullptr && options->target == MMF_DISPLAY_TARGET_OSD) {
    set_last_error(
        "display_show_frame target=OSD is not supported; use display_show_image_file for RGN "
        "overlay");
    return MMF_ENOTSUP;
  }

  std::string error;
  if (!display->display.open(&error)) {
    return ok_or_error(false, error);
  }
  if (!display->display.hideLive(&error)) {
    return ok_or_error(false, error);
  }

  auto* native = static_cast<VIDEO_FRAME_INFO_S*>(frame->priv);
  const int ret = CVI_VO_SendFrame(static_cast<VO_LAYER>(display->config.layer),
                                   static_cast<VO_CHN>(display->config.channel), native,
                                   options != nullptr ? 1000 : 1000);
  if (ret != CVI_SUCCESS) {
    set_last_error("CVI_VO_SendFrame failed, ret=" + std::to_string(ret));
    return MMF_EIO;
  }
  display->status.showing = MMF_TRUE;
  display->status.mode = MMF_DISPLAY_MODE_FRAME_PUSH;
  display->status.frame_count += 1;
  return MMF_OK;
}

mmf_result_t mmf_display_show_image_file(mmf_display_t* display, const char* path,
                                         const mmf_display_show_options_t* options) {
  if (display == nullptr || path == nullptr)
    return MMF_EINVAL;
  mmf_result_t ret = show_image_via_osd(display, path, options);
  if (ret != MMF_OK)
    return ret;
  display->overlay_path = path;
  display->overlay_rect = options ? options->dst_rect : mmf_rect_t{0, 0, 0, 0};
  display->overlay_active = true;
  display->status.showing = MMF_TRUE;
  display->status.mode = MMF_DISPLAY_MODE_OVERLAY;
  return MMF_OK;
}

mmf_result_t mmf_display_snapshot(mmf_display_t* display,
                                  const mmf_display_snapshot_config_t* config) {
  if (display == nullptr || config == nullptr || config->path == nullptr) {
    return MMF_EINVAL;
  }
  std::string error;
  if (!config->include_osd || !display->overlay_active || display->overlay_path.empty()) {
    return ok_or_error(
        display->display.snapshot(config->path, static_cast<int>(config->timeout_ms), &error),
        error);
  }

  char temp_path[128];
  std::snprintf(temp_path, sizeof(temp_path), "/tmp/mmf_snapshot_base_%ld.jpg",
                static_cast<long>(::getpid()));
  if (!display->display.snapshot(temp_path, static_cast<int>(config->timeout_ms), &error)) {
    return ok_or_error(false, error);
  }
  mmf_result_t ret =
      compose_overlay_snapshot(temp_path, config->path, display->overlay_path.c_str(),
                               display->overlay_rect, config->jpeg_quality);
  (void)::unlink(temp_path);
  return ret;
}

mmf_result_t mmf_display_clear(mmf_display_t* display) {
  if (display == nullptr)
    return MMF_EINVAL;
  std::string error;
  if (!display->display.hideLive(&error))
    return ok_or_error(false, error);
  if (display->osd) {
    (void)display->osd->setVisible(false, &error);
    display->osd->detach();
    display->osd->destroy();
    display->osd.reset();
  }
  display->overlay_active = false;
  display->overlay_path.clear();
  display->status.showing = MMF_FALSE;
  display->status.mode = MMF_DISPLAY_MODE_NONE;
  return MMF_OK;
}

mmf_result_t mmf_display_clear_overlay(mmf_display_t* display, uint32_t osd_handle) {
  if (display == nullptr)
    return MMF_EINVAL;
  if (display->osd && (osd_handle == 0 || osd_handle == display->osd_handle)) {
    display->osd->detach();
    display->osd->destroy();
    display->osd.reset();
    display->overlay_active = false;
    display->overlay_path.clear();
    display->status.mode = MMF_DISPLAY_MODE_NONE;
    display->status.showing = MMF_FALSE;
  }
  return MMF_OK;
}

}  // extern "C"
