#ifndef MMF_CVI_CAMERA_HPP
#define MMF_CVI_CAMERA_HPP

#include "mmf_cvi_base.hpp"

namespace mmf_cvi {

enum class CameraSourceId { Ai, Live, Main, SubRgb, Screen };

bool ensureMmfRuntimeInitialized(std::string* error = nullptr);
void setError(std::string* error, const std::string& message);
MMF_CHN_S toMmfChannel(const MediaChannel& channel);

class Camera {
 public:
  struct Config {
    int group = DualOsLayout::kCaptureVpssGroup;
    int channel = DualOsLayout::kLiveChannel;
    int width = DualOsLayout::kLiveWidth;
    int height = DualOsLayout::kLiveHeight;
    int pixel_format = PixelFormat::NV12;
    int timeout_ms = 1000;
  };
  Camera();
  explicit Camera(const Config& config);
  ~Camera();
  Camera(const Camera&) = delete;
  Camera& operator=(const Camera&) = delete;
  static Config forSource(CameraSourceId source, int timeout_ms = 1000);
  bool open(std::string* error = nullptr);
  bool read(Frame* frame, std::string* error = nullptr);
  void releaseFrame();
  void close();
  bool snapshot(const std::string& path, std::string* error = nullptr);

 private:
  Config config_;
  VIDEO_FRAME_INFO_S frame_info_{};
  bool opened_ = false;
  bool frame_valid_ = false;
};

class Display {
 public:
  enum class Input { None, Live, Ai, Main, SubRgb, Screen };
  struct Config {
    int device = DualOsLayout::kVoDevice;
    int layer = 0;
    int channel = DualOsLayout::kVoChannel;
    int width = DualOsLayout::kScreenWidth;
    int height = DualOsLayout::kScreenHeight;
    int pixel_format = PixelFormat::NV12;
    int interface_type = VO_INTF_MIPI;
    int interface_sync = VO_OUTPUT_480P60;
    int display_buf_len = 3;
    int frame_rate = 25;
    int channel_x = 0;
    int channel_y = 0;
    int priority = 0;
    int rotation = DualOsLayout::kVoRotation;
    bool preserve_hardware_on_close = true;
  };
  Display();
  explicit Display(const Config& config);
  ~Display();
  bool open(std::string* error = nullptr);
  void close();
  bool show(Input input, std::string* error = nullptr);
  bool hideLive(std::string* error = nullptr);
  bool snapshot(const std::string& path, int timeout_ms = 1000, std::string* error = nullptr);
  bool isLiveVisible() const {
    return bound_;
  }
  static CameraSourceId toCameraSource(Input input);

 private:
  Config config_;
  Input input_ = Input::None;
  MediaChannel bound_source_{};
  bool bound_ = false;
  bool channel_enabled_ = false;
  bool layer_enabled_ = false;
  bool device_enabled_ = false;
};

struct OsdCanvas {
  void* data = nullptr;
  int width = 0;
  int height = 0;
  int stride = 0;
  int pixel_format = 0;
};
class OsdRegion {
 public:
  struct Config {
    int handle = 0;
    MediaSize size{0, 0};
    int pixel_format = PixelFormat::ARGB8888;
    int canvas_count = 2;
    uint32_t bg_color = 0;
  };
  static Config canvas(int handle, int width, int height, int pixel_format, int canvas_count,
                       uint32_t bg_color) {
    Config cfg;
    cfg.handle = handle;
    cfg.size = MediaSize::make(width, height);
    cfg.pixel_format = pixel_format;
    cfg.canvas_count = canvas_count;
    cfg.bg_color = bg_color;
    return cfg;
  }
  OsdRegion();
  explicit OsdRegion(const Config& config);
  ~OsdRegion();
  bool create(std::string* error = nullptr);
  bool attach(const MediaChannel& channel, int x, int y, int layer, std::string* error = nullptr);
  bool getCanvas(OsdCanvas* canvas, std::string* error = nullptr);
  bool updateCanvas(std::string* error = nullptr);
  bool setVisible(bool visible, std::string* error = nullptr);
  void detach();
  void destroy();

 private:
  Config config_;
  MediaChannel attached_channel_{};
  bool created_ = false;
  bool attached_ = false;
};

}  // namespace mmf_cvi

#endif  // MMF_CVI_CAMERA_HPP
