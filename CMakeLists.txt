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

add_library(tdl_app_sdk STATIC
  third_party/vendor/ini/ini.c
  src/algorithm/algorithm_engine.cpp
  src/algorithm/classifier.cpp
  src/algorithm/detector.cpp
  src/algorithm/feature_extractor.cpp
  src/algorithm/model_descriptor.cpp
  src/algorithm/nn_base.cpp
  src/algorithm/nn_classifier.cpp
  src/algorithm/nn_feature.cpp
  src/algorithm/nn_yolov5.cpp
  src/algorithm/nn_yolov8.cpp
  src/media/camera.cpp
  src/media/frame_reader.cpp
  src/media/frame_source.cpp
  src/media/frame_sink.cpp
  src/media/media_link.cpp
  src/media/media_system.cpp
  src/media/osd_region.cpp
  src/media/mmf.cpp
  src/pipeline.cpp
  src/media/private/sensor_media_impl.cpp
  src/media/private/sensor_media_vi_isp.cpp
  src/media/private/sensor_media_vpss.cpp
  src/media/region_overlay.cpp
  src/media/rtsp_streamer.cpp
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
)

if(TDL_APP_BUILD_DEMOS)
  add_executable(tdl_detect_demo apps/tdl_detect_demo.cpp apps/demo_support.cpp)
  target_sources(tdl_detect_demo PRIVATE third_party/vendor/ini/ini.c)
  target_include_directories(tdl_detect_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_detect_demo PRIVATE tdl_app_sdk)

  add_executable(tdl_yolov5_demo apps/tdl_yolov5_demo.cpp apps/demo_support.cpp)
  target_sources(tdl_yolov5_demo PRIVATE third_party/vendor/ini/ini.c)
  target_include_directories(tdl_yolov5_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_yolov5_demo PRIVATE tdl_app_sdk)

  add_executable(tdl_yolov8_demo apps/tdl_yolov8_demo.cpp apps/demo_support.cpp)
  target_sources(tdl_yolov8_demo PRIVATE third_party/vendor/ini/ini.c)
  target_include_directories(tdl_yolov8_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_yolov8_demo PRIVATE tdl_app_sdk)

  add_executable(tdl_classify_demo apps/tdl_classify_demo.cpp apps/demo_support.cpp)
  target_sources(tdl_classify_demo PRIVATE third_party/vendor/ini/ini.c)
  target_include_directories(tdl_classify_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_classify_demo PRIVATE tdl_app_sdk)

  add_executable(tdl_feature_demo apps/tdl_feature_demo.cpp apps/demo_support.cpp)
  target_sources(tdl_feature_demo PRIVATE third_party/vendor/ini/ini.c)
  target_include_directories(tdl_feature_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_feature_demo PRIVATE tdl_app_sdk)

  add_executable(tdl_camera_capture_demo
    apps/tdl_camera_capture_demo.cpp
    apps/camera_demo_support.cpp
  )
  target_sources(tdl_camera_capture_demo PRIVATE third_party/vendor/ini/ini.c)
  target_include_directories(tdl_camera_capture_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_camera_capture_demo PRIVATE tdl_app_sdk)

  add_executable(tdl_camera_detect_demo
    apps/tdl_camera_detect_demo.cpp
    apps/demo_support.cpp
    apps/camera_demo_support.cpp
  )
  target_sources(tdl_camera_detect_demo PRIVATE third_party/vendor/ini/ini.c)
  target_include_directories(tdl_camera_detect_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_camera_detect_demo PRIVATE tdl_app_sdk)

  add_executable(tdl_camera_classify_demo
    apps/tdl_camera_classify_demo.cpp
    apps/demo_support.cpp
    apps/camera_demo_support.cpp
  )
  target_sources(tdl_camera_classify_demo PRIVATE third_party/vendor/ini/ini.c)
  target_include_directories(tdl_camera_classify_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_camera_classify_demo PRIVATE tdl_app_sdk)

  add_executable(tdl_camera_feature_demo
    apps/tdl_camera_feature_demo.cpp
    apps/demo_support.cpp
    apps/camera_demo_support.cpp
  )
  target_sources(tdl_camera_feature_demo PRIVATE third_party/vendor/ini/ini.c)
  target_include_directories(tdl_camera_feature_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_camera_feature_demo PRIVATE tdl_app_sdk)

  add_executable(tdl_camera_rtsp_demo
    apps/tdl_camera_rtsp_demo.cpp
    apps/camera_demo_support.cpp
  )
  target_sources(tdl_camera_rtsp_demo PRIVATE third_party/vendor/ini/ini.c)
  target_include_directories(tdl_camera_rtsp_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_camera_rtsp_demo PRIVATE tdl_app_sdk)

  add_executable(tdl_multi_vpss_demo
    apps/tdl_multi_vpss_demo.cpp
    apps/camera_demo_support.cpp
  )
  target_sources(tdl_multi_vpss_demo PRIVATE third_party/vendor/ini/ini.c)
  target_include_directories(tdl_multi_vpss_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_multi_vpss_demo PRIVATE tdl_app_sdk)

  add_executable(tdl_media_smoke_demo apps/tdl_media_smoke_demo.cpp)
  target_sources(tdl_media_smoke_demo PRIVATE third_party/vendor/ini/ini.c)
  target_include_directories(tdl_media_smoke_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_media_smoke_demo PRIVATE tdl_app_sdk)

  add_executable(tdl_osd_demo apps/tdl_osd_demo.cpp)
  target_sources(tdl_osd_demo PRIVATE third_party/vendor/ini/ini.c)
  target_include_directories(tdl_osd_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_osd_demo PRIVATE tdl_app_sdk)

  add_executable(tdl_vdec_demo apps/tdl_vdec_demo.cpp)
  target_sources(tdl_vdec_demo PRIVATE third_party/vendor/ini/ini.c)
  target_include_directories(tdl_vdec_demo PRIVATE "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
  target_link_libraries(tdl_vdec_demo PRIVATE tdl_app_sdk)
endif()

install(TARGETS tdl_app_sdk
  ARCHIVE DESTINATION lib
)

if(TDL_APP_BUILD_DEMOS)
  install(TARGETS tdl_detect_demo
    tdl_yolov5_demo
    tdl_yolov8_demo
    tdl_classify_demo
    tdl_feature_demo
    tdl_camera_capture_demo
    tdl_camera_detect_demo
    tdl_camera_classify_demo
    tdl_camera_feature_demo
    tdl_camera_rtsp_demo
    tdl_multi_vpss_demo
    tdl_media_smoke_demo
    tdl_osd_demo
    tdl_vdec_demo
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
