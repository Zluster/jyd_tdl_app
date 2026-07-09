cmake_minimum_required(VERSION 3.16)
project(tdl_app_sdk LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_SKIP_RPATH ON)

option(TDL_APP_BUILD_DEMOS "Build command line demo applications" ON)
option(TDL_APP_MEDIA_MINIMAL "Build only media/audio targets and skip algorithm sources/demos" OFF)
option(TDL_APP_BUILD_MMF_MINIMAL "Build standalone C MMF API wrapper" ON)
set(TDL_APP_PROFILE "dual_os" CACHE STRING "Build profile")

if(NOT TDL_APP_PROFILE STREQUAL "dual_os")
  message(FATAL_ERROR "jyd_tdl_app only supports dual_os builds")
endif()

set(TDL_APP_THIRD_PARTY_DIR
    "${CMAKE_CURRENT_LIST_DIR}/third_party/cv184x/dual_os"
    CACHE PATH "Dual-OS CV184X dependency bundle")

include(cmake/cv184x_tdl.cmake)

set(TDL_APP_FULL_ONLY_SOURCES
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
  src/multi_stage_pipeline.cpp
  src/pipeline.cpp
  src/simple_pipeline.cpp
)

set(TDL_APP_MEDIA_SOURCES
  src/media/audio_decoder.cpp
  src/media/audio_encoder.cpp
  src/media/audio_input.cpp
  src/media/audio_output.cpp
  src/media/audio_system.cpp
  src/media/audio.cpp
  src/media/camera.cpp
  src/media/display.cpp
  src/media/frame_reader.cpp
  src/media/frame_source.cpp
  src/media/frame_sink.cpp
  src/media/graphic_vo_layer.cpp
  src/media/image.cpp
  src/media/media_ipc_client.cpp
  src/media/media_ipc_server.cpp
  src/media/media_link.cpp
  src/media/media_system.cpp
  src/media/mipi_device.cpp
  src/media/osd_region.cpp
  src/media/mmf.cpp
  src/media/private/audio_runtime.cpp
  src/media/region_overlay.cpp
  src/media/sys_context.cpp
  src/media/video_buffer.cpp
  src/media/vdec_channel.cpp
  src/media/venc_channel.cpp
  src/media/vo_output.cpp
  src/media/vi_channel.cpp
  src/media/vpss_group.cpp
)

set(TDL_APP_LIBRARY_SOURCES ${TDL_APP_MEDIA_SOURCES})
if(NOT TDL_APP_MEDIA_MINIMAL)
  list(APPEND TDL_APP_LIBRARY_SOURCES ${TDL_APP_FULL_ONLY_SOURCES})
else()
  message(STATUS "TDL_APP_MEDIA_MINIMAL=ON: building media/audio targets only")
endif()

add_library(tdl_app_sdk STATIC
  ${TDL_APP_LIBRARY_SOURCES}
)

add_library(tdl_app_ini_compat STATIC
  third_party/vendor/ini/ini.c
)

target_include_directories(tdl_app_sdk
  PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/include"
  PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${TDL_CV184X_INCLUDE_DIRS}"
)

target_link_libraries(tdl_app_sdk
  PUBLIC
    ${TDL_CV184X_LIBS}
    tdl_app_ini_compat
)

if(TDL_APP_BUILD_MMF_MINIMAL)
  add_subdirectory(jyd_tdl_mmf_minimal)
endif()

if(TDL_APP_BUILD_DEMOS)
  add_executable(tdl_audio_aio_demo apps/tdl_audio_aio_demo.cpp)
  target_include_directories(tdl_audio_aio_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_audio_aio_demo PRIVATE tdl_app_sdk)

  add_executable(tdl_audio_stream_demo apps/tdl_audio_stream_demo.cpp)
  target_include_directories(tdl_audio_stream_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_audio_stream_demo PRIVATE tdl_app_sdk)

  add_executable(tdl_camera_capture_demo
    apps/tdl_camera_capture_demo.cpp
    apps/camera_demo_support.cpp
  )
  target_include_directories(tdl_camera_capture_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_camera_capture_demo PRIVATE tdl_app_sdk)

  add_executable(sophpi_camera_demo
    apps/sophpi_camera_demo.cpp
    apps/camera_demo_support.cpp
  )
  target_include_directories(sophpi_camera_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(sophpi_camera_demo PRIVATE tdl_app_sdk)

  add_executable(tdl_multi_vpss_demo
    apps/tdl_multi_vpss_demo.cpp
    apps/camera_demo_support.cpp
  )
  target_include_directories(tdl_multi_vpss_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_multi_vpss_demo PRIVATE tdl_app_sdk)

  add_executable(tdl_media_smoke_demo apps/tdl_media_smoke_demo.cpp)
  target_include_directories(tdl_media_smoke_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_media_smoke_demo PRIVATE tdl_app_sdk)

  add_executable(tdl_osd_demo apps/tdl_osd_demo.cpp)
  target_include_directories(tdl_osd_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_osd_demo PRIVATE tdl_app_sdk)

  add_executable(tdl_graphic_vo_demo apps/tdl_graphic_vo_demo.cpp)
  target_include_directories(tdl_graphic_vo_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_graphic_vo_demo PRIVATE tdl_app_sdk)

  add_executable(tdl_vpss_format_probe_demo
    apps/tdl_vpss_format_probe_demo.cpp
    apps/camera_demo_support.cpp
  )
  target_include_directories(tdl_vpss_format_probe_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_vpss_format_probe_demo PRIVATE tdl_app_sdk)

  add_executable(tdl_vdec_demo apps/tdl_vdec_demo.cpp)
  target_include_directories(tdl_vdec_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_vdec_demo PRIVATE tdl_app_sdk)

  if(NOT TDL_APP_MEDIA_MINIMAL)
    add_executable(tdl_detect_demo apps/tdl_detect_demo.cpp apps/demo_support.cpp)
    target_include_directories(tdl_detect_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_detect_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_yolov5_demo apps/tdl_yolov5_demo.cpp apps/demo_support.cpp)
    target_include_directories(tdl_yolov5_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_yolov5_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_yolov8_demo apps/tdl_yolov8_demo.cpp apps/demo_support.cpp)
    target_include_directories(tdl_yolov8_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_yolov8_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_classify_demo apps/tdl_classify_demo.cpp apps/demo_support.cpp)
    target_include_directories(tdl_classify_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_classify_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_feature_demo apps/tdl_feature_demo.cpp apps/demo_support.cpp)
    target_include_directories(tdl_feature_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_feature_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_keypoint_demo apps/tdl_keypoint_demo.cpp)
    target_include_directories(tdl_keypoint_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_keypoint_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_face_dense_keypoint_demo
      apps/tdl_face_dense_keypoint_demo.cpp
      apps/demo_support.cpp
    )
    target_include_directories(tdl_face_dense_keypoint_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_face_dense_keypoint_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_semantic_seg_demo apps/tdl_semantic_seg_demo.cpp)
    target_include_directories(tdl_semantic_seg_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_semantic_seg_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_instance_seg_demo apps/tdl_instance_seg_demo.cpp)
    target_include_directories(tdl_instance_seg_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_instance_seg_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_lane_demo apps/tdl_lane_demo.cpp)
    target_include_directories(tdl_lane_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_lane_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_vad_demo apps/tdl_vad_demo.cpp)
    target_include_directories(tdl_vad_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_vad_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_face_detect_demo apps/tdl_face_detect_demo.cpp apps/demo_support.cpp)
    target_include_directories(tdl_face_detect_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_face_detect_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_face_attribute_demo apps/tdl_face_attribute_demo.cpp apps/demo_support.cpp)
    target_include_directories(tdl_face_attribute_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_face_attribute_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_face_attr_pipeline_demo
      apps/tdl_face_attr_pipeline_demo.cpp
      apps/demo_support.cpp
    )
    target_include_directories(tdl_face_attr_pipeline_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_face_attr_pipeline_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_plate_recognize_demo apps/tdl_plate_recognize_demo.cpp apps/demo_support.cpp)
    target_include_directories(tdl_plate_recognize_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_plate_recognize_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_camera_detect_demo
      apps/tdl_camera_detect_demo.cpp
      apps/demo_support.cpp
      apps/camera_demo_support.cpp
    )
    target_include_directories(tdl_camera_detect_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_camera_detect_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_camera_classify_demo
      apps/tdl_camera_classify_demo.cpp
      apps/demo_support.cpp
      apps/camera_demo_support.cpp
    )
    target_include_directories(tdl_camera_classify_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_camera_classify_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_camera_feature_demo
      apps/tdl_camera_feature_demo.cpp
      apps/demo_support.cpp
      apps/camera_demo_support.cpp
    )
    target_include_directories(tdl_camera_feature_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_camera_feature_demo PRIVATE tdl_app_sdk)

    add_executable(sophpi_ai_osd_demo
      apps/sophpi_ai_osd_demo.cpp
      apps/camera_demo_support.cpp
      apps/mjpeg_server.cpp
    )
    target_sources(sophpi_ai_osd_demo PRIVATE third_party/vendor/ini/ini.c)
    target_include_directories(sophpi_ai_osd_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(sophpi_ai_osd_demo PRIVATE tdl_app_sdk)
  endif()
endif()

install(TARGETS tdl_app_sdk
  ARCHIVE DESTINATION lib
)

if(TDL_APP_BUILD_DEMOS)
  install(TARGETS
    tdl_audio_aio_demo
    tdl_audio_stream_demo
    tdl_camera_capture_demo
    sophpi_camera_demo
    tdl_multi_vpss_demo
    tdl_media_smoke_demo
    tdl_osd_demo
    tdl_graphic_vo_demo
    tdl_vpss_format_probe_demo
    tdl_vdec_demo
    RUNTIME DESTINATION bin
  )

  if(NOT TDL_APP_MEDIA_MINIMAL)
    install(TARGETS
      tdl_detect_demo
      tdl_yolov5_demo
      tdl_yolov8_demo
      tdl_classify_demo
      tdl_feature_demo
      tdl_keypoint_demo
      tdl_face_dense_keypoint_demo
      tdl_semantic_seg_demo
      tdl_instance_seg_demo
      tdl_lane_demo
      tdl_vad_demo
      tdl_face_detect_demo
      tdl_face_attribute_demo
      tdl_face_attr_pipeline_demo
      tdl_plate_recognize_demo
      tdl_camera_detect_demo
      tdl_camera_classify_demo
      tdl_camera_feature_demo
      sophpi_ai_osd_demo
      RUNTIME DESTINATION bin
    )
  endif()
endif()

install(DIRECTORY include/
  DESTINATION include
)

install(DIRECTORY configs/
  DESTINATION configs
)
