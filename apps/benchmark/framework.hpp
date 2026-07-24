#pragma once

#include <memory>
#include <string>

#include "tdl_app/sys_context.hpp"
#include "tdl_app/media_types.hpp"

namespace tdl_bench {

// 全局运行配置：由命令行解析后注入 Context，被各模块读取。
struct RunConfig {
  // 通用生命周期控制
  int repeat = 1;       // 重复 load->loop->exit 的次数（重入/泄露验证）
  int loop_count = 1;   // 每次 load 后调用 loop 的次数
  int warmup = 3;       // 单个用例预热次数
  int iters = 40;       // 单个用例计时迭代次数

  // CV 模块
  std::string image;    // 可选输入图路径，留空则用内置合成图

  // NPU 模块
  std::string task;     // 可选任务名，留空则运行全部 NPU 任务
  bool profile = false; // 打印算法内部耗时分解

  // Display 模块（VoOutput 路径）
  int vo_dev = 0;
  int layer = 0;
  int vo_chn = 0;
  int screen_width = 720;
  int screen_height = 480;
  int interface_type = tdl_app::VoInterfaceType::Mipi;
  int interface_sync = tdl_app::VoInterfaceSync::P720_480_60;
  int frames = 60;      // display 每个分辨率送帧数
  int buffers = 3;      // display 帧池中的 buffer 数
};

// 单例上下文：集中持有跨模块共享资源（此处为 MMF/SYS）。
// 复用已存在的 MMF 初始化，避免每个模块重复 open/close 系统。
class BenchmarkContext {
 public:
  BenchmarkContext() = default;
  BenchmarkContext(const BenchmarkContext &) = delete;
  BenchmarkContext &operator=(const BenchmarkContext &) = delete;

  bool startup(std::string *error = nullptr);
  void shutdown();

  tdl_app::SysContext &sys() { return sys_; }
  const RunConfig &config() const { return config_; }
  void setConfig(const RunConfig &config) { config_ = config; }

 private:
  tdl_app::SysContext sys_;
  bool sys_opened_ = false;
  RunConfig config_;
};

// 统一模块接口：等价于 DESIGN_PATTERN 的 load/loop/exit 四元组。
class BenchmarkModule {
 public:
  virtual ~BenchmarkModule() = default;

  virtual const char *name() const = 0;
  // 一次性建资源；返回 false 表示失败，调度器会回收并停止。
  virtual bool load(BenchmarkContext &ctx, std::string *error) = 0;
  // 反复执行：跑一遍 benchmark 并打印结果。
  virtual bool loop(BenchmarkContext &ctx, std::string *error) = 0;
  // 一次性释放资源；必须与 load 严格配对，保证可重入无泄露。
  virtual void exit(BenchmarkContext &ctx) = 0;
};

using ModuleFactory = std::unique_ptr<BenchmarkModule> (*)();

// Registry 调度表查询接口。
int moduleCount();
const char *moduleName(int id);
int findModuleByName(const std::string &name);  // 找不到返回 -1
std::unique_ptr<BenchmarkModule> createModule(int id);  // 非法 id 返回 nullptr

// 调度器：对指定模块按 repeat 次反复执行 load -> loop(loop_count) -> exit。
bool runModule(int id, BenchmarkContext &ctx, std::string *error);

}  // namespace tdl_bench
