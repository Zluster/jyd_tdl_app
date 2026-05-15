#include "tdl_app/model_descriptor.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace tdl_app {
namespace {

std::string trim(std::string s) {
  auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(),
                                  [&](char c) { return !is_space(c); }));
  s.erase(std::find_if(s.rbegin(), s.rend(),
                       [&](char c) { return !is_space(c); })
              .base(),
          s.end());
  return s;
}

std::vector<std::string> splitTokens(const std::string &value) {
  std::vector<std::string> tokens;
  std::string current;
  std::istringstream ss(value);
  while (std::getline(ss, current, ',')) {
    current = trim(current);
    if (!current.empty()) {
      tokens.push_back(current);
    }
  }
  return tokens;
}

std::vector<float> parseFloatList(const std::string &value) {
  std::vector<float> out;
  for (const auto &token : splitTokens(value)) {
    try {
      out.push_back(std::stof(token));
    } catch (...) {
      return {};
    }
  }
  return out;
}

std::string parentDir(const std::string &path) {
  size_t pos = path.find_last_of("/\\");
  if (pos == std::string::npos) {
    return ".";
  }
  return path.substr(0, pos);
}

bool startsWith(const std::string &value, const std::string &prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

bool loadModelDescriptor(const std::string &path, ModelDescriptor *descriptor,
                         std::string *error) {
  if (!descriptor) {
    if (error) *error = "descriptor pointer is null";
    return false;
  }
  descriptor->descriptor_file = path;
  descriptor->descriptor_dir = parentDir(path);
  descriptor->basic.clear();
  descriptor->extra.clear();
  descriptor->basic_type.clear();
  descriptor->model_path.clear();
  descriptor->runtime.clear();
  descriptor->task_name.clear();
  descriptor->model_type.clear();
  descriptor->input_type.clear();
  descriptor->preprocess.clear();
  descriptor->normalize.clear();
  descriptor->mean.clear();
  descriptor->scale.clear();
  descriptor->anchors.clear();
  descriptor->labels.clear();

  std::ifstream ifs(path);
  if (!ifs) {
    if (error) *error = "failed to open model descriptor: " + path;
    return false;
  }

  enum class Section { None, Basic, Extra };
  Section section = Section::None;
  std::string line;

  while (std::getline(ifs, line)) {
    line = trim(line);
    if (line.empty()) {
      continue;
    }
    if (startsWith(line, "#") || startsWith(line, ";")) {
      continue;
    }
    if (line.front() == '[' && line.back() == ']') {
      std::string sec = trim(line.substr(1, line.size() - 2));
      if (sec == "basic") {
        section = Section::Basic;
      } else if (sec == "extra") {
        section = Section::Extra;
      } else {
        section = Section::None;
      }
      continue;
    }

    size_t eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    std::string key = trim(line.substr(0, eq));
    std::string value = trim(line.substr(eq + 1));
    if (key.empty()) {
      continue;
    }

    if (section == Section::Basic) {
      descriptor->basic[key] = value;
      if (key == "type") {
        descriptor->basic_type = value;
      } else if (key == "model") {
        descriptor->model_path = value;
      }
    } else if (section == Section::Extra) {
      descriptor->extra[key] = value;
      if (key == "runtime") {
        descriptor->runtime = value;
      } else if (key == "task") {
        descriptor->task_name = value;
      } else if (key == "model_type") {
        descriptor->model_type = value;
      } else if (key == "input_type") {
        descriptor->input_type = value;
      } else if (key == "preprocess") {
        descriptor->preprocess = value;
      } else if (key == "normalize") {
        descriptor->normalize = value;
      } else if (key == "mean") {
        descriptor->mean = parseFloatList(value);
      } else if (key == "scale") {
        descriptor->scale = parseFloatList(value);
      } else if (key == "anchors") {
        descriptor->anchors = parseFloatList(value);
      } else if (key == "labels") {
        descriptor->labels = splitTokens(value);
      }
    }
  }

  if (descriptor->model_path.empty()) {
    if (error) *error = "model descriptor is missing basic.model";
    return false;
  }
  if (!descriptor->model_path.empty() &&
      descriptor->model_path.front() != '/' &&
      !(descriptor->model_path.size() >= 2 &&
        descriptor->model_path[1] == ':')) {
    descriptor->model_path = descriptor->descriptor_dir + "/" + descriptor->model_path;
  }
  return true;
}

std::string resolveModelPath(const ModelDescriptor &descriptor) {
  return descriptor.model_path;
}

}  // namespace tdl_app
