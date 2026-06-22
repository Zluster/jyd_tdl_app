#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "framework.hpp"
#include "tdl_app/tdl_app.hpp"

namespace tdl_bench {
namespace {

constexpr const char *kDefaultImage = "/mnt/sd/test.jpg";

bool fileExists(const std::string &path) {
  struct stat st;
  return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

long readRssKb() {
  std::ifstream statm("/proc/self/statm");
  long total_pages = 0;
  long resident_pages = 0;
  statm >> total_pages >> resident_pages;
  (void)total_pages;
  const long page_size_kb = sysconf(_SC_PAGESIZE) / 1024;
  return resident_pages * page_size_kb;
}

template <typename Fn>
double benchOp(Fn &&fn, int warmup, int iters, std::string *summary,
               tdl_app::StageProfile *avg_profile, std::string *error) {
  std::string last_summary;
  tdl_app::StageProfile scratch;
  for (int i = 0; i < warmup; ++i) {
    if (!fn(&last_summary, &scratch, error)) {
      return -1.0;
    }
  }

  tdl_app::StageProfile sum;
  bool profile_valid = iters > 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < iters; ++i) {
    tdl_app::StageProfile p;
    if (!fn(&last_summary, &p, error)) {
      return -1.0;
    }
    sum.load_ms += p.load_ms;
    sum.preprocess_ms += p.preprocess_ms;
    sum.inference_ms += p.inference_ms;
    sum.postprocess_ms += p.postprocess_ms;
    if (!p.valid) {
      profile_valid = false;
    }
  }
  const auto t1 = std::chrono::steady_clock::now();
  if (summary) {
    *summary = last_summary;
  }
  if (avg_profile && iters > 0) {
    avg_profile->load_ms = sum.load_ms / iters;
    avg_profile->preprocess_ms = sum.preprocess_ms / iters;
    avg_profile->inference_ms = sum.inference_ms / iters;
    avg_profile->postprocess_ms = sum.postprocess_ms / iters;
    avg_profile->valid = profile_valid;
  }
  const double total_us = static_cast<double>(
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
  return iters > 0 ? (total_us / iters / 1000.0) : 0.0;
}

std::string scoreText(float score) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(3) << score;
  return oss.str();
}

std::string summarizeAlgorithmResult(const tdl_app::AlgorithmResult &result) {
  std::ostringstream oss;
  if (!result.boxes.empty()) {
    const auto best = std::max_element(
        result.boxes.begin(), result.boxes.end(),
        [](const tdl_app::Box &a, const tdl_app::Box &b) {
          return a.score < b.score;
        });
    oss << "boxes=" << result.boxCount() << ", best_cls=" << best->class_id
        << ", score=" << scoreText(best->score);
    return oss.str();
  }

  if (!result.classes.empty()) {
    const auto best = std::max_element(
        result.classes.begin(), result.classes.end(),
        [](const tdl_app::ClassificationItem &a,
           const tdl_app::ClassificationItem &b) {
          return a.score < b.score;
        });
    oss << "classes=" << result.classCount() << ", best_cls="
        << best->class_id << ", score=" << scoreText(best->score);
    return oss.str();
  }

  if (!result.feature.empty()) {
    oss << "feature_dim=" << result.featureCount();
    return oss.str();
  }

  if (!result.points.empty()) {
    oss << "points=" << result.pointCount();
    return oss.str();
  }

  oss << "empty";
  return oss.str();
}

class NpuTask {
 public:
  virtual ~NpuTask() = default;

  virtual const char *name() const = 0;
  virtual const char *defaultSpec() const = 0;
  virtual const char *defaultImage() const { return kDefaultImage; }

  void setInput(std::string spec, std::string image) {
    spec_ = std::move(spec);
    image_ = std::move(image);
  }

  const std::string &spec() const { return spec_; }
  const std::string &image() const { return image_; }

  virtual bool load(const std::string &firmware, std::string *error) = 0;
  virtual bool runOnce(std::string *summary, tdl_app::StageProfile *profile,
                       std::string *error) = 0;
  virtual void unload() = 0;

 protected:
  tdl_app::ModelSessionConfig sessionConfig(const std::string &firmware) const {
    return tdl_app::ModelSessionConfig::fromSpec(spec_, firmware);
  }

 private:
  std::string spec_;
  std::string image_;
};

class DetectTask : public NpuTask {
 public:
  DetectTask(const char *name, const char *model_type, const char *spec)
      : name_(name), model_type_(model_type), spec_(spec) {}

  const char *name() const override { return name_; }
  const char *defaultSpec() const override { return spec_; }

  bool load(const std::string &firmware, std::string *error) override {
    detector_.reset(new tdl_app::Detector(model_type_));
    return detector_->load(sessionConfig(firmware), error);
  }

  bool runOnce(std::string *summary, tdl_app::StageProfile *profile,
               std::string *error) override {
    tdl_app::InferOptions options = tdl_app::InferOptions::detection();
    tdl_app::AlgorithmResult result;
    if (!detector_->run(image(), options, &result, error)) {
      return false;
    }
    if (summary) {
      *summary = summarizeAlgorithmResult(result);
    }
    if (profile) {
      *profile = result.profile;
    }
    return true;
  }

  void unload() override { detector_.reset(); }

 private:
  const char *name_;
  const char *model_type_;
  const char *spec_;
  std::unique_ptr<tdl_app::Detector> detector_;
};

class ClassifyTask : public NpuTask {
 public:
  const char *name() const override { return "classifier"; }
  const char *defaultSpec() const override {
    return "./configs/model_specs/cls_hand_gesture.mud";
  }

  bool load(const std::string &firmware, std::string *error) override {
    classifier_.reset(new tdl_app::Classifier());
    return classifier_->load(sessionConfig(firmware), error);
  }

  bool runOnce(std::string *summary, tdl_app::StageProfile *profile,
               std::string *error) override {
    tdl_app::InferOptions options = tdl_app::InferOptions::classification();
    tdl_app::AlgorithmResult result;
    if (!classifier_->run(image(), options, &result, error)) {
      return false;
    }
    if (summary) {
      *summary = summarizeAlgorithmResult(result);
    }
    if (profile) {
      *profile = result.profile;
    }
    return true;
  }

  void unload() override { classifier_.reset(); }

 private:
  std::unique_ptr<tdl_app::Classifier> classifier_;
};

class FaceTask : public NpuTask {
 public:
  const char *name() const override { return "scrfd_face"; }
  const char *defaultSpec() const override {
    return "./configs/model_specs/scrfd_real.mud";
  }

  bool load(const std::string &firmware, std::string *error) override {
    detector_.reset(new tdl_app::FaceDetector("SCRFD"));
    return detector_->load(sessionConfig(firmware), error);
  }

  bool runOnce(std::string *summary, tdl_app::StageProfile *profile,
               std::string *error) override {
    tdl_app::InferOptions options = tdl_app::InferOptions::detection();
    tdl_app::AlgorithmResult result;
    if (!detector_->run(image(), options, &result, error)) {
      return false;
    }
    if (summary) {
      *summary = "faces=" + std::to_string(result.boxCount());
    }
    if (profile) {
      *profile = result.profile;
    }
    return true;
  }

  void unload() override { detector_.reset(); }

 private:
  std::unique_ptr<tdl_app::FaceDetector> detector_;
};

class KeypointTask : public NpuTask {
 public:
  const char *name() const override { return "yolov8_pose"; }
  const char *defaultSpec() const override {
    return "./configs/model_specs/pose_yolov8.mud";
  }

  bool load(const std::string &firmware, std::string *error) override {
    detector_.reset(
        new tdl_app::KeypointDetector("KEYPOINT_YOLOV8POSE_PERSON17"));
    return detector_->load(sessionConfig(firmware), error);
  }

  bool runOnce(std::string *summary, tdl_app::StageProfile *profile,
               std::string *error) override {
    tdl_app::KeypointResult result;
    if (!detector_->run(image(), &result, error)) {
      return false;
    }
    if (summary) {
      *summary = "points=" + std::to_string(result.pointCount());
    }
    if (profile) {
      *profile = result.profile;
    }
    return true;
  }

  void unload() override { detector_.reset(); }

 private:
  std::unique_ptr<tdl_app::KeypointDetector> detector_;
};

class FeatureTask : public NpuTask {
 public:
  const char *name() const override { return "feature"; }
  const char *defaultSpec() const override {
    return "./configs/model_specs/feature_cviface.mud";
  }

  bool load(const std::string &firmware, std::string *error) override {
    extractor_.reset(new tdl_app::FeatureExtractor());
    return extractor_->load(sessionConfig(firmware), error);
  }

  bool runOnce(std::string *summary, tdl_app::StageProfile *profile,
               std::string *error) override {
    tdl_app::InferOptions options;
    tdl_app::AlgorithmResult result;
    if (!extractor_->run(image(), options, &result, error)) {
      return false;
    }
    if (summary) {
      *summary = summarizeAlgorithmResult(result);
    }
    if (profile) {
      *profile = result.profile;
    }
    return true;
  }

  void unload() override { extractor_.reset(); }

 private:
  std::unique_ptr<tdl_app::FeatureExtractor> extractor_;
};

void report(const std::string &item, double ms,
            const tdl_app::StageProfile &profile, const std::string &summary) {
  const double fps = ms > 0.0 ? (1000.0 / ms) : 0.0;
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(3);
  oss << "  " << item << ": total " << ms << " ms, " << fps << " fps";
  if (!summary.empty()) {
    oss << " | " << summary;
  }
  oss << "\n";
  if (profile.valid) {
    oss << "      load=" << profile.load_ms
        << "  preprocess=" << profile.preprocess_ms
        << "  inference=" << profile.inference_ms
        << "  postprocess=" << profile.postprocess_ms << "  (ms)\n";
  }
  std::cout << oss.str();
}

class NpuBenchmark : public BenchmarkModule {
 public:
  const char *name() const override { return "NPU(TDL)"; }

  // 只做文件校验和候选收集，不实际打开设备或加载模型。
  // 这样每次 loop() 中每个任务独占一个 bm_handle_t，
  // 避免多个 handle 并存时 bmruntime 内部全局状态被污染导致 SIGSEGV。
  bool load(BenchmarkContext &ctx, std::string *error) override {
    const RunConfig &cfg = ctx.config();

    std::vector<std::unique_ptr<NpuTask>> candidates;
    candidates.emplace_back(new DetectTask(
        "yolov8_detect", "YOLOV8", "./configs/model_specs/yolov8n_det_coco80.mud"));
    candidates.emplace_back(new DetectTask(
        "yolov5_detect", "YOLOV5", "./configs/model_specs/yolov5s_det_coco80.mud"));
    candidates.emplace_back(new ClassifyTask());
    candidates.emplace_back(new FaceTask());
    candidates.emplace_back(new KeypointTask());
    candidates.emplace_back(new FeatureTask());

    pending_tasks_.clear();
    for (auto &task : candidates) {
      const std::string image =
          cfg.image.empty() ? task->defaultImage() : cfg.image;
      const std::string spec = task->defaultSpec();
      if (!fileExists(spec)) {
        std::cout << "  skip " << task->name() << ": missing spec " << spec
                  << std::endl;
        continue;
      }
      if (!fileExists(image)) {
        std::cout << "  skip " << task->name() << ": missing image " << image
                  << std::endl;
        continue;
      }
      task->setInput(spec, image);
      pending_tasks_.push_back(std::move(task));
    }

    if (pending_tasks_.empty()) {
      if (error) {
        *error = "no NPU task available; check model specs and image path";
      }
      return false;
    }
    return true;
  }

  // 每个任务独立经历 load → bench → unload，全程只有一个 bm_handle_t 存在，
  // 从根本上规避 bmruntime 多 handle 析构时的内部状态腐化问题。
  bool loop(BenchmarkContext &ctx, std::string *error) override {
    const RunConfig &cfg = ctx.config();
    const int warmup = cfg.warmup > 0 ? cfg.warmup : 0;
    const int iters = cfg.iters > 0 ? cfg.iters : 1;

    std::cout << "[NPU] TDL inference, avg over " << iters
              << " iters (warmup " << warmup << ")" << std::endl;

    bool any_ran = false;
    for (auto &task : pending_tasks_) {
      std::string load_error;
      const long rss_before = readRssKb();
      if (!task->load(std::string(), &load_error)) {
        std::cout << "  skip " << task->name()
                  << ": load failed: " << load_error << std::endl;
        task->unload();
        continue;
      }
      std::cout << "  [load] " << task->name()
                << " rss=" << readRssKb() << " KB (+" 
                << (readRssKb() - rss_before) << " KB)" << std::endl;

      std::string summary;
      std::string run_error;
      tdl_app::StageProfile profile;
      const double ms = benchOp(
          [&](std::string *out_summary, tdl_app::StageProfile *out_profile,
              std::string *out_error) {
            return task->runOnce(out_summary, out_profile, out_error);
          },
          warmup, iters, &summary, &profile, &run_error);

      task->unload();
      std::cout << "  [unload] " << task->name()
                << " rss=" << readRssKb() << " KB" << std::endl;

      if (ms < 0.0) {
        if (error) {
          *error = std::string(task->name()) + " failed: " + run_error;
        }
        return false;
      }
      report(task->name(), ms, profile, summary);
      any_ran = true;
    }

    if (!any_ran) {
      if (error) {
        *error = "all NPU tasks failed to load";
      }
      return false;
    }
    return true;
  }

  void exit(BenchmarkContext &ctx) override {
    (void)ctx;
    pending_tasks_.clear();
  }

 private:
  std::vector<std::unique_ptr<NpuTask>> pending_tasks_;
};

}  // namespace

std::unique_ptr<BenchmarkModule> createNpuModule() {
  return std::unique_ptr<BenchmarkModule>(new NpuBenchmark());
}

}  // namespace tdl_bench
