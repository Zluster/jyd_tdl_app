#include <cstdlib>
#include <iostream>
#include <string>

#include "benchmark/framework.hpp"

namespace {

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_benchmark_demo [--module cv|display|npu|all] [--list]\n"
      << "                     [--repeat N] [--loops N]\n"
      << "                     [--warmup N] [--iters N]\n"
      << "                     [--image PATH]\n"
      << "                     [--vo-dev N] [--layer N] [--vo-chn N]\n"
      << "                     [--screen-width N] [--screen-height N]\n"
      << "                     [--interface-type N] [--interface-sync N]\n"
      << "                     [--frames N] [--buffers N]\n"
      << "\n"
      << "Notes:\n"
      << "  --module all  run every registered module in order\n"
      << "  --repeat N    reload(load->loop->exit) N times (leak/reentry check)\n"
      << "  --loops N     call loop() N times per load\n";
}

struct Args {
  std::string module = "all";
  bool list = false;
  tdl_bench::RunConfig config;
};

bool parseArgs(int argc, char **argv, Args *args) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << name << "\n";
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--module") {
      const char *v = value("--module");
      if (!v) return false;
      args->module = v;
    } else if (arg == "--list") {
      args->list = true;
    } else if (arg == "--repeat") {
      const char *v = value("--repeat");
      if (!v) return false;
      args->config.repeat = std::atoi(v);
    } else if (arg == "--loops") {
      const char *v = value("--loops");
      if (!v) return false;
      args->config.loop_count = std::atoi(v);
    } else if (arg == "--warmup") {
      const char *v = value("--warmup");
      if (!v) return false;
      args->config.warmup = std::atoi(v);
    } else if (arg == "--iters") {
      const char *v = value("--iters");
      if (!v) return false;
      args->config.iters = std::atoi(v);
    } else if (arg == "--image") {
      const char *v = value("--image");
      if (!v) return false;
      args->config.image = v;
    } else if (arg == "--vo-dev") {
      const char *v = value("--vo-dev");
      if (!v) return false;
      args->config.vo_dev = std::atoi(v);
    } else if (arg == "--layer") {
      const char *v = value("--layer");
      if (!v) return false;
      args->config.layer = std::atoi(v);
    } else if (arg == "--vo-chn") {
      const char *v = value("--vo-chn");
      if (!v) return false;
      args->config.vo_chn = std::atoi(v);
    } else if (arg == "--screen-width") {
      const char *v = value("--screen-width");
      if (!v) return false;
      args->config.screen_width = std::atoi(v);
    } else if (arg == "--screen-height") {
      const char *v = value("--screen-height");
      if (!v) return false;
      args->config.screen_height = std::atoi(v);
    } else if (arg == "--interface-type") {
      const char *v = value("--interface-type");
      if (!v) return false;
      args->config.interface_type = std::atoi(v);
    } else if (arg == "--interface-sync") {
      const char *v = value("--interface-sync");
      if (!v) return false;
      args->config.interface_sync = std::atoi(v);
    } else if (arg == "--frames") {
      const char *v = value("--frames");
      if (!v) return false;
      args->config.frames = std::atoi(v);
    } else if (arg == "--buffers") {
      const char *v = value("--buffers");
      if (!v) return false;
      args->config.buffers = std::atoi(v);
    } else if (arg == "-h" || arg == "--help") {
      printUsage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  Args args;
  if (!parseArgs(argc, argv, &args)) {
    printUsage();
    return 1;
  }

  if (args.list) {
    std::cout << "registered modules:\n";
    for (int i = 0; i < tdl_bench::moduleCount(); ++i) {
      std::cout << "  [" << i << "] " << tdl_bench::moduleName(i) << "\n";
    }
    return 0;
  }

  tdl_bench::BenchmarkContext ctx;
  ctx.setConfig(args.config);

  std::string error;
  if (!ctx.startup(&error)) {
    std::cerr << "context startup failed: " << error << "\n";
    return 2;
  }

  int rc = 0;
  if (args.module == "all") {
    for (int id = 0; id < tdl_bench::moduleCount(); ++id) {
      if (!tdl_bench::runModule(id, ctx, &error)) {
        std::cerr << "module " << tdl_bench::moduleName(id)
                  << " failed: " << error << "\n";
        rc = 3;
        break;
      }
    }
  } else {
    const int id = tdl_bench::findModuleByName(args.module);
    if (id < 0) {
      std::cerr << "unknown module: " << args.module
                << " (use --list to see options)\n";
      ctx.shutdown();
      return 4;
    }
    if (!tdl_bench::runModule(id, ctx, &error)) {
      std::cerr << "module " << args.module << " failed: " << error << "\n";
      rc = 3;
    }
  }

  ctx.shutdown();
  std::cout << "benchmark done, rc=" << rc << std::endl;
  return rc;
}
