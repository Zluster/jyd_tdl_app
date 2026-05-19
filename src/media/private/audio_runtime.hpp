#pragma once

#include <string>

namespace tdl_app {

bool retainAudioRuntime(std::string *error = nullptr);
void releaseAudioRuntime();

}  // namespace tdl_app
