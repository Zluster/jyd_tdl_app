#pragma once

#include "tdl_app/camera.hpp"
#include "tdl_app/classifier.hpp"
#include "tdl_app/detector.hpp"
#include "tdl_app/face_attribute_classifier.hpp"
#include "tdl_app/face_detector.hpp"
#include "tdl_app/feature_extractor.hpp"
#include "tdl_app/frame_sink.hpp"
#include "tdl_app/frame_source.hpp"
#include "tdl_app/instance_segmenter.hpp"
#include "tdl_app/keypoint_detector.hpp"
#include "tdl_app/lane_detector.hpp"
#include "tdl_app/media_types.hpp"
#include "tdl_app/model_descriptor.hpp"
#include "tdl_app/multi_stage_pipeline.hpp"
#include "tdl_app/pipeline.hpp"
#include "tdl_app/plate_recognizer.hpp"
#include "tdl_app/semantic_segmenter.hpp"
#include "tdl_app/vision_task_types.hpp"
#include "tdl_app/voice_activity_detector.hpp"

// The umbrella header exposes the default application-facing API only:
// Detector / Classifier / FaceDetector / FaceAttributeClassifier /
// PlateRecognizer / FeatureExtractor / Camera / Pipeline.
// Media graph setup and board-level primitives live in advanced.hpp.
