#include "tdl_app/mipi_device.hpp"

#include <string>

#include "cvi_mipi.h"

namespace tdl_app {
namespace {

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

}  // namespace

MipiDevice::MipiDevice(int device) : device_(device) {}

bool MipiDevice::setMipiReset(bool reset, std::string *error) const {
  const int ret = CVI_MIPI_SetMipiReset(device_, reset ? 1U : 0U);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_MIPI_SetMipiReset failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool MipiDevice::setSensorClock(bool enable, std::string *error) const {
  const int ret = CVI_MIPI_SetSensorClock(device_, enable ? 1U : 0U);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_MIPI_SetSensorClock failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool MipiDevice::setClockEdge(bool up, std::string *error) const {
  const int ret = CVI_MIPI_SetClkEdge(device_, up ? 1U : 0U);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_MIPI_SetClkEdge failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool MipiDevice::setSensorReset(unsigned int reset_port, unsigned int reset_pin,
                                unsigned int reset_polarity, bool reset_enable,
                                std::string *error) const {
  const int ret = CVI_MIPI_SetSensorReset(device_, reset_port, reset_pin,
                                          reset_polarity,
                                          reset_enable ? 1U : 0U);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_MIPI_SetSensorReset failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool MipiDevice::setMipiAttr(int vi_pipe, const combo_dev_attr_s &attr,
                             std::string *error) const {
  const int ret = CVI_MIPI_SetMipiAttr(vi_pipe, &attr);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_MIPI_SetMipiAttr failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool MipiDevice::setSensorMclk(const mclk_pll_s &mclk,
                               std::string *error) const {
  const int ret = CVI_MIPI_SetSnsMclk(const_cast<mclk_pll_s *>(&mclk));
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_MIPI_SetSnsMclk failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

}  // namespace tdl_app
