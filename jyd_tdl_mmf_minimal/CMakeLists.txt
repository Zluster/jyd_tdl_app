cmake_minimum_required(VERSION 3.16)

project(jyd_tdl_mmf_minimal C CXX)

option(MMF_MINIMAL_BUILD_EXAMPLES "Build minimal MMF examples" ON)

set(MMF_MINIMAL_SOURCES
  src/audio/mmf_cvi_audio.cpp
  src/camera/mmf_cvi_camera_display.cpp
  src/display/mmf_cvi_osd.cpp
  src/jpeg/mmf_cvi_vdec.cpp
  src/jpeg/mmf_cvi_venc.cpp
  src/audio/mmf_audio_3a_cv184x.cpp
  src/audio/mmf_audio_codec_cv184x.cpp
  src/audio/mmf_audio_cv184x.cpp
  src/camera/mmf_camera_cv184x.cpp
  src/display/mmf_display_cv184x.cpp
  src/http/mmf_jpg_http_cv184x.cpp
  src/jpeg/mmf_jpg_cv184x.cpp
  src/system/mmf_system_cv184x.cpp
  src/core/mmf_cv184x_common.cpp
  src/core/mmf_cv184x_resources.cpp
  src/touch/mmf_touch.c
)

add_library(mmf_minimal STATIC
  ${MMF_MINIMAL_SOURCES}
)

add_library(mmf_minimal_ini_compat STATIC
  ../third_party/vendor/ini/ini.c
)

target_include_directories(mmf_minimal
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(mmf_minimal PUBLIC c_std_99)

target_compile_features(mmf_minimal PUBLIC cxx_std_14)
target_include_directories(mmf_minimal
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${TDL_CV184X_INCLUDE_DIRS}
)
target_link_libraries(mmf_minimal PUBLIC ${TDL_CV184X_LIBS} mmf_minimal_ini_compat)

if(MMF_MINIMAL_BUILD_EXAMPLES)
  set(MMF_MINIMAL_EXAMPLE_TARGETS)
  foreach(example
      camera_snapshot
      display_show
      jpg_http_server
      av_monitor_app
      pipeline_soak_app
      camera_album_app
      audio_full_duplex_app
      smoke_test)
    add_executable(mmf_${example} examples/${example}.c)
    target_link_libraries(mmf_${example} PRIVATE mmf_minimal)
    list(APPEND MMF_MINIMAL_EXAMPLE_TARGETS mmf_${example})
  endforeach()
endif()

install(TARGETS mmf_minimal ARCHIVE DESTINATION lib)
if(MMF_MINIMAL_EXAMPLE_TARGETS)
  install(TARGETS ${MMF_MINIMAL_EXAMPLE_TARGETS} RUNTIME DESTINATION bin)
endif()
install(PROGRAMS scripts/board_smoke_test.sh
        DESTINATION bin
        RENAME mmf_board_smoke_test)
install(DIRECTORY include/mmf DESTINATION include)
