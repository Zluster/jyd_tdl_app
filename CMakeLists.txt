cmake_minimum_required(VERSION 3.16)
project(tdl_app_sdk LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_SKIP_RPATH ON)

option(TDL_APP_BUILD_DEMOS "Build command line demo applications" ON)

set(TDL_APP_THIRD_PARTY_DIR
    "${CMAKE_CURRENT_LIST_DIR}/third_party/cv184x"
    CACHE PATH "Portable CV184X dependency bundle")

include(cmake/cv184x_tdl.cmake)

function(tdl_append_existing out_var)
  foreach(path IN LISTS ARGN)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/${path}")
      list(APPEND ${out_var} "${path}")
    endif()
  endforeach()
  set(${out_var} "${${out_var}}" PARENT_SCOPE)
endfunction()

set(TDL_APP_SOURCES
  third_party/vendor/ini/ini.c
  src/algorithm/algorithm_engine.cpp
  src/algorithm/classifier.cpp
  src/algorithm/detector.cpp
  src/algorithm/face_attribute_classifier.cpp
  src/algorithm/face_detector.cpp
  src/algorithm/feature_extractor.cpp
  src/algorithm/instance_segmenter.cpp
  src/algorithm/keypoint_detector.cpp
  src/algorithm/lane_detector.cpp
  src/algorithm/model_descriptor.cpp
  src/algorithm/nn_base.cpp
  src/algorithm/nn_classifier.cpp
  src/algorithm/nn_face_attribute.cpp
  src/algorithm/nn_feature.cpp
  src/algorithm/nn_plate_recognizer.cpp
  src/algorithm/nn_scrfd.cpp
  src/algorithm/nn_yolov5.cpp
  src/algorithm/nn_yolov8.cpp
  src/algorithm/plate_recognizer.cpp
  src/algorithm/semantic_segmenter.cpp
  src/algorithm/voice_activity_detector.cpp
  src/media/audio_decoder.cpp
  src/media/audio_encoder.cpp
  src/media/audio_input.cpp
  src/media/audio_output.cpp
  src/media/audio_system.cpp
  src/media/camera.cpp
  src/media/frame_reader.cpp
  src/media/frame_source.cpp
  src/media/frame_sink.cpp
  src/media/graphic_vo_layer.cpp
  src/media/media_ipc_client.cpp
  src/media/media_ipc_server.cpp
  src/media/media_link.cpp
  src/media/media_system.cpp
  src/media/mipi_device.cpp
  src/media/osd_region.cpp
  src/media/mmf.cpp
  src/multi_stage_pipeline.cpp
  src/pipeline.cpp
  src/media/private/audio_runtime.cpp
  src/media/private/sensor_media_impl.cpp
  src/media/private/sensor_media_vi_isp.cpp
  src/media/private/sensor_media_vpss.cpp
  src/media/region_overlay.cpp
  src/media/sensor_media.cpp
  src/simple_pipeline.cpp
  src/media/sys_context.cpp
  src/media/video_buffer.cpp
  src/media/vdec_channel.cpp
  src/media/venc_channel.cpp
  src/media/vo_output.cpp
  src/media/vi_channel.cpp
  src/media/vpss_group.cpp
)

tdl_append_existing(TDL_APP_SOURCES
  src/algorithm/keyword_spotter.cpp
  src/algorithm/self_learning_classifier.cpp
  src/algorithm/single_object_tracker.cpp
)

if(TDL_HAS_AUDIO_RUNTIME)
  tdl_append_existing(TDL_APP_SOURCES
    src/algorithm/speech_recognizer.cpp
    src/media/audio_frontend.cpp
  )
else()
  message(STATUS "Audio runtime dependencies not found; skip speech/audio frontend sources")
endif()

add_library(tdl_app_sdk STATIC ${TDL_APP_SOURCES})

target_include_directories(tdl_app_sdk
  PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/include"
  PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src"
    $<$<NOT:$<BOOL:${TDL_HAS_LINUX_FB_H}>>:${CMAKE_CURRENT_LIST_DIR}/src/compat>
    "${TDL_CV184X_INCLUDE_DIRS}"
)

target_link_libraries(tdl_app_sdk
  PUBLIC
    ${TDL_CV184X_LIBS}
)

function(tdl_add_demo target)
  set(options)
  set(oneValueArgs)
  set(multiValueArgs SOURCES)
  cmake_parse_arguments(TDL_DEMO "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
  if(NOT TDL_DEMO_SOURCES)
    return()
  endif()
  foreach(src IN LISTS TDL_DEMO_SOURCES)
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/${src}")
      return()
    endif()
  endforeach()
  add_executable(${target} ${TDL_DEMO_SOURCES})
  target_sources(${target} PRIVATE third_party/vendor/ini/ini.c)
  target_include_directories(${target} PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(${target} PRIVATE tdl_app_sdk)
  set(TDL_APP_DEMO_TARGETS ${TDL_APP_DEMO_TARGETS} ${target} PARENT_SCOPE)
endfunction()

set(TDL_APP_DEMO_TARGETS)

if(TDL_APP_BUILD_DEMOS)
  tdl_add_demo(tdl_detect_demo SOURCES apps/tdl_detect_demo.cpp apps/demo_support.cpp)
  tdl_add_demo(tdl_yolov5_demo SOURCES apps/tdl_yolov5_demo.cpp apps/demo_support.cpp)
  tdl_add_demo(tdl_yolov8_demo SOURCES apps/tdl_yolov8_demo.cpp apps/demo_support.cpp)
  tdl_add_demo(tdl_classify_demo SOURCES apps/tdl_classify_demo.cpp apps/demo_support.cpp)
  tdl_add_demo(tdl_feature_demo SOURCES apps/tdl_feature_demo.cpp apps/demo_support.cpp)
  tdl_add_demo(tdl_keypoint_demo SOURCES apps/tdl_keypoint_demo.cpp)
  tdl_add_demo(tdl_face_dense_keypoint_demo SOURCES apps/tdl_face_dense_keypoint_demo.cpp apps/demo_support.cpp)
  tdl_add_demo(tdl_semantic_seg_demo SOURCES apps/tdl_semantic_seg_demo.cpp)
  tdl_add_demo(tdl_instance_seg_demo SOURCES apps/tdl_instance_seg_demo.cpp)
  tdl_add_demo(tdl_single_object_tracker_demo SOURCES apps/tdl_single_object_tracker_demo.cpp)
  tdl_add_demo(tdl_self_learn_classify_demo SOURCES apps/tdl_self_learn_classify_demo.cpp)
  tdl_add_demo(tdl_lane_demo SOURCES apps/tdl_lane_demo.cpp)
  tdl_add_demo(tdl_audio_aio_demo SOURCES apps/tdl_audio_aio_demo.cpp)
  tdl_add_demo(tdl_face_detect_demo SOURCES apps/tdl_face_detect_demo.cpp apps/demo_support.cpp)
  tdl_add_demo(tdl_face_attribute_demo SOURCES apps/tdl_face_attribute_demo.cpp apps/demo_support.cpp)
  tdl_add_demo(tdl_face_attr_pipeline_demo SOURCES apps/tdl_face_attr_pipeline_demo.cpp apps/demo_support.cpp)
  tdl_add_demo(tdl_plate_recognize_demo SOURCES apps/tdl_plate_recognize_demo.cpp apps/demo_support.cpp)
  tdl_add_demo(tdl_camera_capture_demo SOURCES apps/tdl_camera_capture_demo.cpp apps/camera_demo_support.cpp)
  tdl_add_demo(tdl_camera_detect_demo SOURCES apps/tdl_camera_detect_demo.cpp apps/demo_support.cpp apps/camera_demo_support.cpp)
  tdl_add_demo(tdl_camera_classify_demo SOURCES apps/tdl_camera_classify_demo.cpp apps/demo_support.cpp apps/camera_demo_support.cpp)
  tdl_add_demo(tdl_camera_feature_demo SOURCES apps/tdl_camera_feature_demo.cpp apps/demo_support.cpp apps/camera_demo_support.cpp)
  tdl_add_demo(tdl_camera_vo_demo SOURCES apps/tdl_camera_vo_demo.cpp apps/camera_demo_support.cpp)
  tdl_add_demo(tdl_camera_mjpg_demo SOURCES apps/tdl_camera_mjpg_demo.cpp apps/camera_demo_support.cpp)
  tdl_add_demo(tdl_multi_vpss_demo SOURCES apps/tdl_multi_vpss_demo.cpp apps/camera_demo_support.cpp)
  tdl_add_demo(tdl_media_smoke_demo SOURCES apps/tdl_media_smoke_demo.cpp)
  tdl_add_demo(tdl_osd_demo SOURCES apps/tdl_osd_demo.cpp)
  tdl_add_demo(tdl_pp_ocr_demo SOURCES apps/tdl_pp_ocr_demo.cpp apps/demo_support.cpp)
  tdl_add_demo(tdl_graphic_vo_demo SOURCES apps/tdl_graphic_vo_demo.cpp)
  tdl_add_demo(tdl_mmf_osd_fill_demo SOURCES apps/tdl_mmf_osd_fill_demo.cpp)
  tdl_add_demo(tdl_mmf_vo_fill_demo SOURCES apps/tdl_mmf_vo_fill_demo.cpp)
  tdl_add_demo(tdl_mmf_vpss_probe_demo SOURCES apps/tdl_mmf_vpss_probe_demo.cpp)
  tdl_add_demo(tdl_vdec_demo SOURCES apps/tdl_vdec_demo.cpp)
  if(TDL_HAS_AUDIO_RUNTIME)
    tdl_add_demo(tdl_asr_demo SOURCES apps/tdl_asr_demo.cpp)
    tdl_add_demo(tdl_kws_demo SOURCES apps/tdl_kws_demo.cpp)
    tdl_add_demo(tdl_vad_demo SOURCES apps/tdl_vad_demo.cpp)
  else()
    message(STATUS "Audio runtime dependencies not found; skip ASR/KWS/VAD demos")
  endif()
endif()

install(TARGETS tdl_app_sdk
  ARCHIVE DESTINATION lib
)

if(TDL_APP_BUILD_DEMOS AND TDL_APP_DEMO_TARGETS)
  install(TARGETS ${TDL_APP_DEMO_TARGETS}
    RUNTIME DESTINATION bin
  )
endif()

install(DIRECTORY include/
  DESTINATION include
)

install(DIRECTORY configs/
  DESTINATION configs
)

install(FILES
  "${CMAKE_CURRENT_LIST_DIR}/configs/sensor_cfg_cv2003.ini"
  "${CMAKE_CURRENT_LIST_DIR}/configs/sensor_cfg_cv2003_switch_high_addr54_mclk1.ini"
  "${CMAKE_CURRENT_LIST_DIR}/configs/sensor_cfg_cv2003_switch_high_addr54_mclk1_lane21.ini"
  "${CMAKE_CURRENT_LIST_DIR}/configs/sensor_cfg_cv2003_switch_master_addr53_mclk1.ini"
  DESTINATION configs
)
