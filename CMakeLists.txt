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
  src/algorithm/byte_tracker.cpp
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
  src/algorithm/pose_classifier.cpp
  src/algorithm/semantic_segmenter.cpp
  src/algorithm/self_learning_classifier.cpp
  src/algorithm/single_object_tracker.cpp
  src/algorithm/speech_recognizer.cpp
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
  src/media/private/audio_file_decoder.cpp
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

set(TDL_APP_AUDIO_FFMPEG_ROOT
    "/home/jyd/zwz/sophpi/install/soc_cv1843hp_jyd_common_sd_dualos_alios/images/rootfs_new/usr"
    CACHE PATH "CV184X rootfs usr directory containing FFmpeg headers and libraries")
find_path(TDL_APP_FFMPEG_INCLUDE_DIR
  NAMES libavformat/avformat.h
  PATHS "${TDL_APP_AUDIO_FFMPEG_ROOT}/include"
  NO_DEFAULT_PATH)
find_file(TDL_APP_AVFORMAT_LIBRARY
  NAMES libavformat.so.59 libavformat.so.59.16.100
  PATHS "${TDL_APP_AUDIO_FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
find_file(TDL_APP_AVCODEC_LIBRARY
  NAMES libavcodec.so.59 libavcodec.so.59.18.100
  PATHS "${TDL_APP_AUDIO_FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
find_file(TDL_APP_AVUTIL_LIBRARY
  NAMES libavutil.so.57 libavutil.so.57.17.100
  PATHS "${TDL_APP_AUDIO_FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
find_file(TDL_APP_SWRESAMPLE_LIBRARY
  NAMES libswresample.so.4 libswresample.so.4.3.100
  PATHS "${TDL_APP_AUDIO_FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
if(NOT TDL_APP_FFMPEG_INCLUDE_DIR OR NOT TDL_APP_AVFORMAT_LIBRARY OR
   NOT TDL_APP_AVCODEC_LIBRARY OR NOT TDL_APP_AVUTIL_LIBRARY OR
   NOT TDL_APP_SWRESAMPLE_LIBRARY)
  message(FATAL_ERROR
    "FFmpeg audio decode/resample dependencies were not found under ${TDL_APP_AUDIO_FFMPEG_ROOT}")
endif()
set_property(SOURCE src/media/private/audio_file_decoder.cpp APPEND PROPERTY
  COMPILE_OPTIONS "-isystem${TDL_APP_FFMPEG_INCLUDE_DIR}")

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
    "${TDL_APP_AVFORMAT_LIBRARY}"
    "${TDL_APP_AVCODEC_LIBRARY}"
    "${TDL_APP_SWRESAMPLE_LIBRARY}"
    "${TDL_APP_AVUTIL_LIBRARY}"
)

target_link_options(tdl_app_sdk PUBLIC
  "-Wl,-rpath-link,${TDL_APP_AUDIO_FFMPEG_ROOT}/lib"
  "-Wl,-rpath-link,${TDL_APP_AUDIO_FFMPEG_ROOT}/../lib")

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
    if(DEFINED ENV{TDL_APP_FREETYPE_ROOT})
      set(_TDL_APP_FREETYPE_ROOT_DEFAULT "$ENV{TDL_APP_FREETYPE_ROOT}")
    else()
      set(_TDL_APP_FREETYPE_ROOT_DEFAULT
          "/home/jyd/zwz/sophpi/install/rootfs/usr")
    endif()
    set(TDL_APP_FREETYPE_ROOT "${_TDL_APP_FREETYPE_ROOT_DEFAULT}"
        CACHE PATH "CV184X rootfs containing FreeType headers and library")
    find_path(TDL_APP_FREETYPE_INCLUDE_DIR
      NAMES ft2build.h
      PATHS "${TDL_APP_FREETYPE_ROOT}/include/freetype2"
      NO_DEFAULT_PATH
    )
    find_library(TDL_APP_FREETYPE_LIBRARY
      NAMES freetype
      PATHS "${TDL_APP_FREETYPE_ROOT}/lib"
      NO_DEFAULT_PATH
    )
    find_library(TDL_APP_BZ2_LIBRARY NAMES bz2
      PATHS "${TDL_APP_FREETYPE_ROOT}/lib" NO_DEFAULT_PATH)
    find_library(TDL_APP_PNG_LIBRARY NAMES png16
      PATHS "${TDL_APP_FREETYPE_ROOT}/lib" NO_DEFAULT_PATH)
    find_library(TDL_APP_BROTLI_DEC_LIBRARY NAMES brotlidec
      PATHS "${TDL_APP_FREETYPE_ROOT}/lib" NO_DEFAULT_PATH)
    find_library(TDL_APP_BROTLI_COMMON_LIBRARY NAMES brotlicommon
      PATHS "${TDL_APP_FREETYPE_ROOT}/lib" NO_DEFAULT_PATH)
    find_library(TDL_APP_ROOTFS_Z_LIBRARY NAMES z
      PATHS "${TDL_APP_FREETYPE_ROOT}/../lib" NO_DEFAULT_PATH)
    if(NOT TDL_APP_FREETYPE_INCLUDE_DIR OR NOT TDL_APP_FREETYPE_LIBRARY OR
       NOT TDL_APP_BZ2_LIBRARY OR NOT TDL_APP_PNG_LIBRARY OR
       NOT TDL_APP_BROTLI_DEC_LIBRARY OR NOT TDL_APP_BROTLI_COMMON_LIBRARY OR
       NOT TDL_APP_ROOTFS_Z_LIBRARY)
      message(FATAL_ERROR
        "FreeType and its rootfs dependencies are required for UTF-8 OCR overlays. Set TDL_APP_FREETYPE_ROOT.")
    endif()
    set(TDL_APP_FREETYPE_LIBRARIES
      "${TDL_APP_FREETYPE_LIBRARY}"
      "${TDL_APP_BZ2_LIBRARY}"
      "${TDL_APP_PNG_LIBRARY}"
      "${TDL_APP_BROTLI_DEC_LIBRARY}"
      "${TDL_APP_BROTLI_COMMON_LIBRARY}"
      "${TDL_APP_ROOTFS_Z_LIBRARY}"
    )

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

    add_executable(tdl_keypoint_demo
      apps/tdl_keypoint_demo.cpp
      apps/camera_demo_support.cpp
    )
    target_include_directories(tdl_keypoint_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_keypoint_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_pose_classifier_demo
      apps/tdl_pose_classifier_demo.cpp
      apps/camera_demo_support.cpp
    )
    target_include_directories(tdl_pose_classifier_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_pose_classifier_demo PRIVATE tdl_app_sdk)
    target_link_options(tdl_pose_classifier_demo PRIVATE -Wl,--as-needed)

    add_executable(tdl_single_object_tracker_demo
      apps/tdl_single_object_tracker_demo.cpp
      apps/camera_demo_support.cpp
    )
    target_include_directories(tdl_single_object_tracker_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_single_object_tracker_demo PRIVATE tdl_app_sdk)
    target_link_options(tdl_single_object_tracker_demo PRIVATE -Wl,--as-needed)

    add_executable(tdl_self_learn_classify_demo
      apps/tdl_self_learn_classify_demo.cpp
    )
    target_include_directories(tdl_self_learn_classify_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_self_learn_classify_demo PRIVATE tdl_app_sdk)
    target_link_options(tdl_self_learn_classify_demo PRIVATE -Wl,--as-needed)

    add_executable(tdl_byte_tracker_camera_demo
      apps/tdl_byte_tracker_camera_demo.cpp
      apps/camera_demo_support.cpp
    )
    target_include_directories(tdl_byte_tracker_camera_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_byte_tracker_camera_demo PRIVATE tdl_app_sdk)
    target_link_options(tdl_byte_tracker_camera_demo PRIVATE -Wl,--as-needed)

    add_executable(tdl_byte_tracker_sequence_demo
      apps/tdl_byte_tracker_sequence_demo.cpp
    )
    target_include_directories(tdl_byte_tracker_sequence_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_byte_tracker_sequence_demo PRIVATE tdl_app_sdk)
    target_link_options(tdl_byte_tracker_sequence_demo PRIVATE -Wl,--as-needed)

    add_executable(tdl_face_dense_keypoint_demo
      apps/tdl_face_dense_keypoint_demo.cpp
      apps/demo_support.cpp
      apps/camera_demo_support.cpp
    )
    target_include_directories(tdl_face_dense_keypoint_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps" "${CMAKE_CURRENT_LIST_DIR}/src")
    target_link_libraries(tdl_face_dense_keypoint_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_semantic_seg_demo
      apps/tdl_semantic_seg_demo.cpp
      apps/camera_demo_support.cpp
    )
    target_include_directories(tdl_semantic_seg_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_semantic_seg_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_instance_seg_demo
      apps/tdl_instance_seg_demo.cpp
      apps/camera_demo_support.cpp
    )
    target_include_directories(tdl_instance_seg_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_instance_seg_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_lane_demo
      apps/tdl_lane_demo.cpp
      apps/camera_demo_support.cpp
    )
    target_include_directories(tdl_lane_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_lane_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_asr_demo apps/tdl_asr_demo.cpp)
    target_include_directories(tdl_asr_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_asr_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_dualos_asr_demo apps/tdl_dualos_asr_demo.cpp)
    target_include_directories(tdl_dualos_asr_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_dualos_asr_demo PRIVATE tdl_app_sdk)

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
      apps/camera_demo_support.cpp
    )
    target_include_directories(tdl_face_attr_pipeline_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_face_attr_pipeline_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_face_recognition_demo apps/tdl_face_recognition_demo.cpp)
    target_include_directories(tdl_face_recognition_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_face_recognition_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_plate_recognize_demo
      apps/tdl_plate_recognize_demo.cpp
      apps/demo_support.cpp
      apps/camera_demo_support.cpp
    )
    target_include_directories(tdl_plate_recognize_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_plate_recognize_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_pp_ocr_demo
      apps/tdl_pp_ocr_demo.cpp
      apps/demo_support.cpp
      apps/ocr_overlay_support.cpp
    )
    target_include_directories(tdl_pp_ocr_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps" "${TDL_APP_FREETYPE_INCLUDE_DIR}")
    target_link_libraries(tdl_pp_ocr_demo PRIVATE tdl_app_sdk ${TDL_APP_FREETYPE_LIBRARIES})
    target_link_options(tdl_pp_ocr_demo PRIVATE
      "-Wl,-rpath-link,${TDL_APP_FREETYPE_ROOT}/lib"
      "-Wl,-rpath-link,${TDL_APP_FREETYPE_ROOT}/../lib")

    add_executable(tdl_pp_ocr_camera_demo
      apps/tdl_pp_ocr_camera_demo.cpp
      apps/camera_demo_support.cpp
      apps/ocr_overlay_support.cpp
    )
    target_include_directories(tdl_pp_ocr_camera_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps" "${TDL_APP_FREETYPE_INCLUDE_DIR}")
    target_link_libraries(tdl_pp_ocr_camera_demo PRIVATE tdl_app_sdk ${TDL_APP_FREETYPE_LIBRARIES})
    target_link_options(tdl_pp_ocr_camera_demo PRIVATE
      -Wl,--as-needed
      "-Wl,-rpath-link,${TDL_APP_FREETYPE_ROOT}/lib"
      "-Wl,-rpath-link,${TDL_APP_FREETYPE_ROOT}/../lib")

    add_executable(tdl_camera_detect_demo
      apps/tdl_camera_detect_demo.cpp
      apps/demo_support.cpp
      apps/camera_demo_support.cpp
    )
    target_include_directories(tdl_camera_detect_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_camera_detect_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_camera_scrfd_benchmark_demo
      apps/tdl_camera_scrfd_benchmark_demo.cpp
      apps/camera_demo_support.cpp
    )
    target_include_directories(tdl_camera_scrfd_benchmark_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_camera_scrfd_benchmark_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_camera_tdlsdk_face_benchmark_demo
      apps/tdl_camera_tdlsdk_face_benchmark_demo.cpp
      apps/camera_demo_support.cpp
    )
    target_include_directories(tdl_camera_tdlsdk_face_benchmark_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_camera_tdlsdk_face_benchmark_demo PRIVATE tdl_app_sdk)

    add_executable(tdl_camera_yolov8_benchmark_demo
      apps/tdl_camera_yolov8_benchmark_demo.cpp
      apps/camera_demo_support.cpp
    )
    target_include_directories(tdl_camera_yolov8_benchmark_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_camera_yolov8_benchmark_demo PRIVATE tdl_app_sdk)

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

    add_executable(tdl_benchmark_demo
      apps/tdl_benchmark_demo.cpp
      apps/benchmark/framework.cpp
      apps/benchmark/benchmark_cv.cpp
      apps/benchmark/benchmark_display.cpp
      apps/benchmark/benchmark_npu.cpp
      apps/benchmark/screen.cpp
      apps/camera_demo_support.cpp
    )
    target_include_directories(tdl_benchmark_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_benchmark_demo PRIVATE tdl_app_sdk)
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
      tdl_pose_classifier_demo
      tdl_single_object_tracker_demo
      tdl_self_learn_classify_demo
      tdl_byte_tracker_camera_demo
      tdl_byte_tracker_sequence_demo
      tdl_face_dense_keypoint_demo
      tdl_semantic_seg_demo
      tdl_instance_seg_demo
      tdl_lane_demo
      tdl_asr_demo
      tdl_dualos_asr_demo
      tdl_vad_demo
      tdl_face_detect_demo
      tdl_face_attribute_demo
      tdl_face_attr_pipeline_demo
      tdl_face_recognition_demo
      tdl_plate_recognize_demo
      tdl_pp_ocr_demo
      tdl_pp_ocr_camera_demo
      tdl_camera_detect_demo
      tdl_camera_scrfd_benchmark_demo
      tdl_camera_tdlsdk_face_benchmark_demo
      tdl_camera_yolov8_benchmark_demo
      tdl_camera_classify_demo
      tdl_camera_feature_demo
      sophpi_ai_osd_demo
      tdl_benchmark_demo
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
