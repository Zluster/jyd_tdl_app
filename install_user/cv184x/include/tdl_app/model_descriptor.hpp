#pragma once

#include <map>
#include <string>
#include <vector>

namespace tdl_app {

struct ModelDescriptor {
  std::string descriptor_file;
  std::string descriptor_dir;

  std::string basic_type;
  std::string model_path;

  std::string runtime;
  std::string task_name;
  std::string model_type;
  std::string input_type;
  std::string preprocess;
  std::string normalize;
  std::vector<float> mean;
  std::vector<float> scale;
  std::vector<float> anchors;
  std::vector<std::string> labels;

  std::map<std::string, std::string> basic;
  std::map<std::string, std::string> extra;
};

using ModelSpec = ModelDescriptor;

bool loadModelDescriptor(const std::string &path, ModelDescriptor *descriptor,
                         std::string *error = nullptr);

std::string resolveModelPath(const ModelDescriptor &descriptor);

}  // namespace tdl_app
