#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "framework.hpp"

namespace tdl_bench {
namespace {

// 计时一个操作：先 warmup，再迭代 iters 次取平均，返回单次平均耗时(ms)。
template <typename Fn>
double benchOp(Fn &&fn, int warmup, int iters) {
  for (int i = 0; i < warmup; ++i) {
    fn();
  }
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < iters; ++i) {
    fn();
  }
  const auto t1 = std::chrono::steady_clock::now();
  const double total_us = static_cast<double>(
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
  return iters > 0 ? (total_us / iters / 1000.0) : 0.0;
}

void report(const std::string &item, double ms) {
  const double fps = ms > 0.0 ? (1000.0 / ms) : 0.0;
  std::cout << "  " << item << ": " << ms << " ms, " << fps << " fps"
            << std::endl;
}

// 合成一张彩色基准图：横向渐变 + 周期性方块，保证有边缘和颜色分布。
cv::Mat makeSyntheticImage(int width, int height) {
  cv::Mat img(height, width, CV_8UC3);
  for (int y = 0; y < height; ++y) {
    auto *row = img.ptr<cv::Vec3b>(y);
    for (int x = 0; x < width; ++x) {
      const bool block = (((x / 32) + (y / 32)) & 1) != 0;
      const std::uint8_t b = static_cast<std::uint8_t>((x * 255) / width);
      const std::uint8_t g = static_cast<std::uint8_t>((y * 255) / height);
      const std::uint8_t r = block ? 200 : 40;
      row[x] = cv::Vec3b(b, g, r);
    }
  }
  return img;
}

class CvBenchmark : public BenchmarkModule {
 public:
  const char *name() const override { return "CV(OpenCV)"; }

  bool load(BenchmarkContext &ctx, std::string *error) override {
    const RunConfig &cfg = ctx.config();
    if (!cfg.image.empty()) {
      base_bgr_ = cv::imread(cfg.image, cv::IMREAD_COLOR);
      if (base_bgr_.empty()) {
        if (error) *error = "failed to read image: " + cfg.image;
        return false;
      }
    } else {
      base_bgr_ = makeSyntheticImage(640, 480);
    }
    return true;
  }

  bool loop(BenchmarkContext &ctx, std::string *error) override {
    (void)error;
    const RunConfig &cfg = ctx.config();
    const int warmup = cfg.warmup;
    const int iters = cfg.iters;
    const std::vector<cv::Size> resolutions = {{320, 240}, {640, 480}};

    std::cout << "[CV] OpenCV ops, avg over " << iters
              << " iters (warmup " << warmup << ")" << std::endl;

    for (const cv::Size &res : resolutions) {
      const std::string tag =
          std::to_string(res.width) + "x" + std::to_string(res.height);

      // 源图按目标分辨率准备好 BGR / 灰度 / 二值图
      cv::Mat bgr;
      cv::resize(base_bgr_, bgr, res, 0, 0, cv::INTER_LINEAR);
      cv::Mat gray;
      cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
      cv::Mat binary;
      cv::adaptiveThreshold(gray, binary, 255, cv::ADAPTIVE_THRESH_MEAN_C,
                            cv::THRESH_BINARY_INV, 11, 2);

      // resize：从原始基准图缩放到目标分辨率
      report("resize " + tag, benchOp(
          [&]() {
            cv::Mat dst;
            cv::resize(base_bgr_, dst, res, 0, 0, cv::INTER_LINEAR);
          },
          warmup, iters));

      // cvtColor：BGR -> GRAY
      report("cvtColor BGR2GRAY " + tag, benchOp(
          [&]() {
            cv::Mat dst;
            cv::cvtColor(bgr, dst, cv::COLOR_BGR2GRAY);
          },
          warmup, iters));

      // inRange：颜色阈值二值化
      report("inRange " + tag, benchOp(
          [&]() {
            cv::Mat mask;
            cv::inRange(bgr, cv::Scalar(0, 0, 0), cv::Scalar(150, 150, 255),
                        mask);
          },
          warmup, iters));

      // adaptiveThreshold：自适应二值化（灰度）
      report("adaptiveThreshold gray " + tag, benchOp(
          [&]() {
            cv::Mat dst;
            cv::adaptiveThreshold(gray, dst, 255, cv::ADAPTIVE_THRESH_MEAN_C,
                                  cv::THRESH_BINARY, 11, 2);
          },
          warmup, iters));

      // calcHist：灰度直方图
      report("calcHist gray " + tag, benchOp(
          [&]() {
            cv::Mat hist;
            const int channels[] = {0};
            const int hist_size[] = {256};
            const float range[] = {0.0f, 256.0f};
            const float *ranges[] = {range};
            cv::calcHist(&gray, 1, channels, cv::Mat(), hist, 1, hist_size,
                         ranges);
          },
          warmup, iters));

      // findContours：轮廓查找（基于二值图）
      report("findContours " + tag, benchOp(
          [&]() {
            cv::Mat work = binary.clone();
            std::vector<std::vector<cv::Point>> contours;
            cv::findContours(work, contours, cv::RETR_EXTERNAL,
                             cv::CHAIN_APPROX_SIMPLE);
          },
          warmup, iters));
    }
    return true;
  }

  void exit(BenchmarkContext &ctx) override {
    (void)ctx;
    base_bgr_.release();
  }

 private:
  cv::Mat base_bgr_;
};

}  // namespace

std::unique_ptr<BenchmarkModule> createCvModule() {
  return std::unique_ptr<BenchmarkModule>(new CvBenchmark());
}

}  // namespace tdl_bench
