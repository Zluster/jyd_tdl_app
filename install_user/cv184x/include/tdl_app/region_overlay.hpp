#pragma once

#include <string>

#include "tdl_app/media_types.hpp"

namespace tdl_app {

class RegionOverlay {
 public:
  RegionOverlay();
  explicit RegionOverlay(const OverlayRegionConfig &config);
  ~RegionOverlay();

  RegionOverlay(const RegionOverlay &) = delete;
  RegionOverlay &operator=(const RegionOverlay &) = delete;

  bool create(std::string *error = nullptr);
  bool attach(const MediaChannel &channel, int x, int y, int layer,
              std::string *error = nullptr);
  bool setBitmap(const OverlayBitmap &bitmap, std::string *error = nullptr);
  void detach();
  void destroy();
  bool isCreated() const;

 private:
  OverlayRegionConfig config_;
  MediaChannel attached_channel_;
  bool created_ = false;
  bool attached_ = false;
};

}  // namespace tdl_app
