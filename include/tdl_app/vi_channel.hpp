#pragma once

#include <string>

namespace tdl_app {

class ViChannel {
 public:
  struct Config {
    int pipe = 0;
    int channel = 0;
    int pixel_format = 18;
    int width = 0;
    int height = 0;
    int depth = 0;
    bool mirror = false;
    bool flip = false;
    bool single_vb = true;
  };

  ViChannel();
  explicit ViChannel(const Config &config);
  ~ViChannel();

  ViChannel(const ViChannel &) = delete;
  ViChannel &operator=(const ViChannel &) = delete;

  bool open(std::string *error = nullptr);
  void close();
  bool isOpen() const;

 private:
  Config config_;
  bool opened_ = false;
};

}  // namespace tdl_app
