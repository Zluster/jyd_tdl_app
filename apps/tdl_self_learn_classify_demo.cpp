#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "image_demo_support.hpp"
#include "tdl_app/tdl_app.hpp"

namespace {

struct SampleSpec {
  std::string label;
  std::string image;
};

struct Options {
  image_demo_support::CommonOptions common;
  std::string bank;
  std::vector<SampleSpec> adds;
  int top_k = 3;
};

bool saveTopKOverlay(
    const std::string &input, const std::string &output,
    const tdl_app::SelfLearningClassificationResult &result,
    std::string *error) {
  cv::Mat image = cv::imread(input, cv::IMREAD_COLOR);
  if (image.empty()) {
    if (error) *error = "failed to read self-learning query image";
    return false;
  }
  int y = 36;
  for (size_t index = 0; index < result.classes.size(); ++index) {
    const auto &item = result.classes[index];
    const std::string text = std::to_string(index + 1) + ". " + item.label +
                             cv::format(" %.3f (%d)", item.score,
                                        item.sample_count);
    cv::putText(image, text, cv::Point(14, y), cv::FONT_HERSHEY_SIMPLEX,
                0.75, cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
    y += 34;
  }
  if (!cv::imwrite(output, image)) {
    if (error) *error = "failed to write self-learning top-k overlay";
    return false;
  }
  return true;
}

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_self_learn_classify_demo --model-spec FILE --bank FILE\n"
      << "      [--add LABEL=IMAGE]...\n"
      << "      [--image FILE] [--top-k N]\n"
      << "      [--firmware FILE]\n";
}

bool parseAddSpec(const std::string &text, SampleSpec *spec) {
  if (!spec) {
    return false;
  }
  const size_t pos = text.find('=');
  if (pos == std::string::npos || pos == 0 || pos + 1 >= text.size()) {
    return false;
  }
  spec->label = text.substr(0, pos);
  spec->image = text.substr(pos + 1);
  return !spec->label.empty() && !spec->image.empty();
}

bool parseArgs(int argc, char **argv, Options *opt) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    bool handled = false;
    std::string parse_error;
    if (!image_demo_support::parseCommonArgs(argc, argv, &i, &opt->common,
                                             &handled, &parse_error)) {
      std::cerr << parse_error << "\n";
      return false;
    }
    if (handled) {
      continue;
    }

    auto requireValue = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << name << " requires a value\n";
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--bank") {
      const char *value = requireValue("--bank");
      if (!value) return false;
      opt->bank = value;
    } else if (arg == "--add") {
      const char *value = requireValue("--add");
      if (!value) return false;
      SampleSpec spec;
      if (!parseAddSpec(value, &spec)) {
        std::cerr << "invalid --add, expected LABEL=IMAGE\n";
        return false;
      }
      opt->adds.push_back(std::move(spec));
    } else if (arg == "-h" || arg == "--help") {
      printUsage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }

  if (opt->common.model_spec.empty()) {
    std::cerr << "model-spec is required\n";
    return false;
  }
  if (opt->bank.empty()) {
    std::cerr << "bank is required\n";
    return false;
  }
  if (opt->adds.empty() && opt->common.image.empty()) {
    std::cerr << "either --add or --image is required\n";
    return false;
  }
  opt->top_k = opt->common.top_k;
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    printUsage();
    return 1;
  }

  std::string error;
  tdl_app::SelfLearningClassifier classifier;
  if (!classifier.load(image_demo_support::featureConfig(opt.common), &error)) {
    std::cerr << "initialize failed: " << error << "\n";
    return 2;
  }

  classifier.loadBank(opt.bank, nullptr);

  for (const auto &sample : opt.adds) {
    if (!classifier.addSample(sample.label, sample.image, &error)) {
      std::cerr << "add sample failed: " << error << "\n";
      return 3;
    }
    std::cout << "added: label=" << sample.label << " image=" << sample.image
              << "\n";
  }

  if (!opt.adds.empty()) {
    if (!classifier.saveBank(opt.bank, &error)) {
      std::cerr << "save bank failed: " << error << "\n";
      return 4;
    }
    std::cout << "bank saved: " << opt.bank << "\n";
    std::cout << "classes: " << classifier.classCount()
              << " samples: " << classifier.sampleCount()
              << " feature_dim: " << classifier.featureDim() << "\n";
  }

  if (!opt.common.image.empty()) {
    tdl_app::SelfLearningClassificationResult result;
    if (!classifier.classify(opt.common.image, opt.top_k, &result, &error)) {
      std::cerr << "classify failed: " << error << "\n";
      return 5;
    }
    std::cout << "feature_dim: " << result.feature_dim << "\n";
    std::cout << "classes: " << result.classes.size() << "\n";
    for (size_t i = 0; i < result.classes.size(); ++i) {
      const auto &item = result.classes[i];
      std::cout << "  [" << i << "] label=" << item.label
                << " score=" << item.score
                << " samples=" << item.sample_count << "\n";
    }
    if (!opt.common.output.empty()) {
      if (!saveTopKOverlay(opt.common.image, opt.common.output, result,
                           &error)) {
        std::cerr << "save failed: " << error << "\n";
        return 6;
      }
      std::cout << "saved: " << opt.common.output << "\n";
    }
  }

  // The CV184X BMRT feature runtime can fault during process-global teardown
  // after this one-shot file demo has already released every result. Flush the
  // user-visible output and bypass that runtime shutdown path.
  std::cout.flush();
  std::fflush(nullptr);
  std::_Exit(0);
}
