#pragma once

#include <string>

#include "tdl_app/algorithm_engine.hpp"

namespace ocr_overlay_support {

bool saveAnnotatedImage(const std::string &input, const std::string &output,
                        const tdl_app::AlgorithmResult &result,
                        const std::string &font_path,
                        std::string *error = nullptr);

}  // namespace ocr_overlay_support
