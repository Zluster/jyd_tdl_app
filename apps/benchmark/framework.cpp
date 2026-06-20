#include "framework.hpp"

#include <cstring>
#include <iostream>

namespace tdl_bench {

// 各模块在自身 .cpp 中提供的工厂函数。
std::unique_ptr<BenchmarkModule> createCvModule();
std::unique_ptr<BenchmarkModule> createDisplayModule();
std::unique_ptr<BenchmarkModule> createNpuModule();

namespace {

struct RegistryEntry {
  const char *name;
  ModuleFactory factory;
};

// ── 注册表：新增模块只需在此追加一行 ──
const RegistryEntry kRegistry[] = {
    {"cv", createCvModule},        // id = 0
    {"display", createDisplayModule},  // id = 1
    {"npu", createNpuModule},      // id = 2
};

constexpr int kRegistrySize =
    static_cast<int>(sizeof(kRegistry) / sizeof(kRegistry[0]));

}  // namespace

bool BenchmarkContext::startup(std::string *error) {
  if (!sys_opened_) {
    if (!sys_.open(error)) {
      return false;
    }
    sys_opened_ = true;
  }
  return true;
}

void BenchmarkContext::shutdown() {
  if (sys_opened_) {
    sys_.close();
    sys_opened_ = false;
  }
}

int moduleCount() { return kRegistrySize; }

const char *moduleName(int id) {
  if (id < 0 || id >= kRegistrySize) {
    return "";
  }
  return kRegistry[id].name;
}

int findModuleByName(const std::string &name) {
  for (int i = 0; i < kRegistrySize; ++i) {
    if (name == kRegistry[i].name) {
      return i;
    }
  }
  return -1;
}

std::unique_ptr<BenchmarkModule> createModule(int id) {
  if (id < 0 || id >= kRegistrySize || kRegistry[id].factory == nullptr) {
    return nullptr;
  }
  return kRegistry[id].factory();
}

bool runModule(int id, BenchmarkContext &ctx, std::string *error) {
  const RunConfig &cfg = ctx.config();
  const int repeat = cfg.repeat > 0 ? cfg.repeat : 1;
  const int loop_count = cfg.loop_count > 0 ? cfg.loop_count : 1;

  for (int r = 0; r < repeat; ++r) {
    std::unique_ptr<BenchmarkModule> module = createModule(id);
    if (!module) {
      if (error) *error = "invalid module id: " + std::to_string(id);
      return false;
    }

    std::cout << "[bench] === " << module->name() << " pass " << (r + 1) << "/"
              << repeat << " : load ===" << std::endl;
    std::string load_error;
    if (!module->load(ctx, &load_error)) {
      std::cerr << "[bench] " << module->name()
                << " load failed: " << load_error << std::endl;
      module->exit(ctx);
      if (error) *error = load_error;
      return false;
    }

    bool loop_ok = true;
    for (int i = 0; i < loop_count; ++i) {
      std::string loop_error;
      if (!module->loop(ctx, &loop_error)) {
        std::cerr << "[bench] " << module->name()
                  << " loop failed: " << loop_error << std::endl;
        if (error) *error = loop_error;
        loop_ok = false;
        break;
      }
    }

    module->exit(ctx);
    std::cout << "[bench] === " << module->name() << " pass " << (r + 1) << "/"
              << repeat << " : exit done ===" << std::endl;

    if (!loop_ok) {
      return false;
    }
  }
  return true;
}

}  // namespace tdl_bench
