#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace tdl_app {

class SysContext {
 public:
  struct Config {
    bool reuse_existing = true;

    static Config reuseExisting() {
      Config config;
      config.reuse_existing = true;
      return config;
    }

    static Config createNew() {
      Config config;
      config.reuse_existing = false;
      return config;
    }
  };

  struct IonBuffer {
    std::uint64_t physical = 0;
    void *virtual_addr = nullptr;
    std::size_t size = 0;
    bool cached = false;
  };

  SysContext();
  explicit SysContext(const Config &config);
  ~SysContext();

  SysContext(const SysContext &) = delete;
  SysContext &operator=(const SysContext &) = delete;

  bool open(std::string *error = nullptr);
  void close();
  bool isOpen() const;

  bool allocIon(std::size_t size, const std::string &name, bool cached,
                IonBuffer *buffer, std::string *error = nullptr) const;
  bool freeIon(IonBuffer *buffer, std::string *error = nullptr) const;
  bool flushIon(const IonBuffer &buffer, std::string *error = nullptr) const;
  bool invalidateIon(const IonBuffer &buffer,
                     std::string *error = nullptr) const;

  bool getVersion(std::string *version, std::string *error = nullptr) const;
  bool getChipId(std::uint32_t *chip_id,
                 std::string *error = nullptr) const;
  bool getChipVersion(std::uint32_t *chip_version,
                      std::string *error = nullptr) const;
  bool getCurrentPts(std::uint64_t *pts_us,
                     std::string *error = nullptr) const;

 private:
  Config config_;
  bool opened_ = false;
  bool own_sys_ = false;
};

}  // namespace tdl_app
