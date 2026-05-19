#pragma once

#include <string>

#include "cvi_comm_cif.h"

namespace tdl_app {

class MipiDevice {
 public:
  explicit MipiDevice(int device = 0);

  bool setMipiReset(bool reset, std::string *error = nullptr) const;
  bool setSensorClock(bool enable, std::string *error = nullptr) const;
  bool setClockEdge(bool up, std::string *error = nullptr) const;
  bool setSensorReset(unsigned int reset_port, unsigned int reset_pin,
                      unsigned int reset_polarity, bool reset_enable,
                      std::string *error = nullptr) const;
  bool setMipiAttr(int vi_pipe, const combo_dev_attr_s &attr,
                   std::string *error = nullptr) const;
  bool setSensorMclk(const mclk_pll_s &mclk, std::string *error = nullptr) const;

  int device() const { return device_; }

 private:
  int device_ = 0;
};

}  // namespace tdl_app
