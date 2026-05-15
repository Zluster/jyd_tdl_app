#pragma once

#include "tdl_app/camera.hpp"
#include "tdl_app/classifier.hpp"
#include "tdl_app/detector.hpp"
#include "tdl_app/feature_extractor.hpp"
#include "tdl_app/frame_sink.hpp"
#include "tdl_app/frame_source.hpp"
#include "tdl_app/media_types.hpp"
#include "tdl_app/model_descriptor.hpp"
#include "tdl_app/pipeline.hpp"

// The umbrella header exposes the default application-facing API only:
// Detector / Classifier / FeatureExtractor / Camera / Pipeline.
// Media graph setup and board-level primitives live in advanced.hpp.
