#pragma once

#include <string>

#include "tdl_app/algorithm_engine.hpp"

namespace demo_support {

void printLabels(const tdl_app::AlgorithmResult &result);
void dumpResult(const tdl_app::AlgorithmResult &result);
bool saveAnnotatedImage(const std::string &image_path,
                        const std::string &output_path,
                        const tdl_app::AlgorithmResult &result,
                        std::string *error = nullptr);

}  // namespace demo_support
