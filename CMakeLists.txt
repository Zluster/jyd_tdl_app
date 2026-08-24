cmake_minimum_required(VERSION 3.16)
project(tdl_app_sdk LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_SKIP_RPATH ON)

option(TDL_APP_BUILD_DEMOS "Build command line demo applications" ON)
option(TDL_APP_MEDIA_MINIMAL "Build only media/audio targets and skip algorithm sources/demos" OFF)
option(TDL_APP_BUILD_MMF_MINIMAL "Build standalone C MMF API wrapper" ON)
option(TDL_APP_BUILD_PYTHON "Build the tdl_py nanobind python module" ON)
option(TDL_APP_PY_WITH_NPU "Enable NPU inference bindings in tdl_py" ON)
option(TDL_APP_BUILD_KWS "Build Sherpa keyword-spotter targets and demos" ON)
set(TDL_APP_PYTHON_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/python3.10"
    CACHE PATH "Target-platform Python 3.10 headers (Python.h)")
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
  src/media/rgb_led.cpp
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
if(TDL_APP_FFMPEG_INCLUDE_DIR AND TDL_APP_AVFORMAT_LIBRARY AND
   TDL_APP_AVCODEC_LIBRARY AND TDL_APP_AVUTIL_LIBRARY AND
   TDL_APP_SWRESAMPLE_LIBRARY)
  set(TDL_APP_HAS_FFMPEG ON)
else()
  set(TDL_APP_HAS_FFMPEG OFF)
  message(STATUS
    "FFmpeg not found under ${TDL_APP_AUDIO_FFMPEG_ROOT}; "
    "audio file decode/playback support is disabled")
endif()

if(TDL_APP_HAS_FFMPEG)
  set_property(SOURCE src/media/private/audio_file_decoder.cpp APPEND PROPERTY
    COMPILE_OPTIONS "-isystem${TDL_APP_FFMPEG_INCLUDE_DIR}")
  set_property(SOURCE src/media/private/audio_file_decoder.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS TDL_APP_HAS_FFMPEG=1)
endif()

# CAMPPlus uses the same Kaldi-compatible fbank implementation as the
# validated speaker-recognition reference. These are target ARM static libs;
# no Sherpa runtime or tdl_core.so is linked into this SDK.
set(TDL_APP_KALDI_NATIVE_FBANK_DIR
    "${CMAKE_CURRENT_LIST_DIR}/third_party/vendor/kaldi_native_fbank")
set(TDL_APP_KALDI_NATIVE_FBANK_LIBRARY
    "${TDL_APP_KALDI_NATIVE_FBANK_DIR}/lib/libkaldi-native-fbank-core.a")
set(TDL_APP_KISSFFT_LIBRARY
    "${TDL_APP_KALDI_NATIVE_FBANK_DIR}/lib/libkissfft-float.a")
if(NOT EXISTS "${TDL_APP_KALDI_NATIVE_FBANK_LIBRARY}" OR
   NOT EXISTS "${TDL_APP_KISSFFT_LIBRARY}")
  message(FATAL_ERROR "Missing vendored Kaldi fbank libraries under ${TDL_APP_KALDI_NATIVE_FBANK_DIR}")
endif()
add_library(tdl_app_kaldi_native_fbank STATIC IMPORTED GLOBAL)
set_target_properties(tdl_app_kaldi_native_fbank PROPERTIES
  IMPORTED_LOCATION "${TDL_APP_KALDI_NATIVE_FBANK_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${TDL_APP_KALDI_NATIVE_FBANK_DIR}/include"
)
add_library(tdl_app_kissfft STATIC IMPORTED GLOBAL)
set_target_properties(tdl_app_kissfft PROPERTIES
  IMPORTED_LOCATION "${TDL_APP_KISSFFT_LIBRARY}"
)

# Kept separate from tdl_app_sdk so the speaker application does not inherit
# the legacy tdl_core.so dependency of the broad visual-algorithm target.
add_library(tdl_app_speaker_core STATIC
  src/algorithm/speaker_recognizer.cpp
  src/algorithm/model_descriptor.cpp
  src/media/audio_input.cpp
  src/media/private/audio_runtime.cpp
  src/media/sys_context.cpp
)

# Direct BM Runtime streaming ASR is intentionally isolated from tdl_app_sdk:
# it uses the validated 39/320/5537 Zipformer model set and does not link
# the legacy tdl_core.so speech implementation.
add_library(tdl_app_npu_asr_core STATIC
  src/algorithm/npu_asr_recognizer.cpp
  src/algorithm/model_descriptor.cpp
  src/media/audio_input.cpp
  src/media/private/audio_runtime.cpp
  src/media/sys_context.cpp
)
if(TDL_APP_BUILD_KWS)
add_library(tdl_app_npu_kws_core STATIC
  src/algorithm/npu_keyword_spotter.cpp
  src/algorithm/model_descriptor.cpp
)
add_library(tdl_app_npu_kws_audio STATIC
  src/media/audio_input.cpp
  src/media/private/audio_runtime.cpp
  src/media/sys_context.cpp
)
set_property(TARGET tdl_app_npu_kws_core PROPERTY CXX_STANDARD 17)
target_compile_definitions(tdl_app_npu_kws_core PRIVATE
  LIBESPEAK_NG_EXPORT=1
  SHERPA_ONNX_ENABLE_DIRECTML=0
  SHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION=0
  SHERPA_ONNX_ENABLE_TTS=1
  kiss_fft_scalar=float)
target_compile_options(tdl_app_npu_kws_core PRIVATE -fPIE)
# ORT carries a different FlatBuffers build; keep archive symbols local so the
# dual-os BM Runtime always uses its matching bmodel parser.
target_link_options(tdl_app_npu_kws_core INTERFACE
  -static-libstdc++
  -static-libgcc
  "-Wl,--exclude-libs,ALL"
  "-Wl,-rpath-link,${TDL_APP_THIRD_PARTY_DIR}/lib")

# KWS uses Sherpa's production ContextGraph decoder.  The encoder is the
# CV184X bmodel, while the decoder and joiner are the original small ONNX
# models.  This is deliberately separate from the legacy tdl_core runtime.
set(TDL_APP_SHERPA_KWS_DIR
    "${CMAKE_CURRENT_LIST_DIR}/third_party/vendor/sherpa_onnx")
set(TDL_APP_SHERPA_KWS_LIB_DIR "${TDL_APP_SHERPA_KWS_DIR}/lib")
set(TDL_APP_SHERPA_KWS_SOURCE_DIR
    "/home/jyd/zwz/sophpi/sherpa-onnx_cv184x/third_party/sherpa-onnx")
set(TDL_APP_SHERPA_KWS_ORT_INCLUDE_DIR
    "/home/jyd/zwz/sophpi/sherpa-onnx_cv184x/build/arm-musl/_deps/onnxruntime-src/include")
set(TDL_APP_SHERPA_KWS_DEPS_DIR
    "/home/jyd/zwz/sophpi/sherpa-onnx_cv184x/build/arm-musl/_deps")
set(TDL_APP_SHERPA_KWS_REQUIRED_FILES
  "${TDL_APP_SHERPA_KWS_LIB_DIR}/libsherpa-onnx-core.a"
  "${TDL_APP_SHERPA_KWS_LIB_DIR}/libsherpa-onnx-cv184x-bmrt.so"
  "${TDL_APP_SHERPA_KWS_LIB_DIR}/libonnxruntime.a"
  "${TDL_APP_SHERPA_KWS_DIR}/include/sherpa-onnx/csrc/keyword-spotter.h"
  "${TDL_APP_SHERPA_KWS_ORT_INCLUDE_DIR}/onnxruntime_cxx_api.h")
foreach(file IN LISTS TDL_APP_SHERPA_KWS_REQUIRED_FILES)
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "Missing vendored Sherpa KWS dependency: ${file}")
  endif()
endforeach()
target_include_directories(tdl_app_npu_kws_core
  PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include"
  PUBLIC "${CMAKE_CURRENT_LIST_DIR}/src"
          "${TDL_APP_SHERPA_KWS_DIR}/include"
          "${TDL_APP_SHERPA_KWS_SOURCE_DIR}"
          "${TDL_APP_SHERPA_KWS_ORT_INCLUDE_DIR}"
          "${TDL_APP_SHERPA_KWS_DEPS_DIR}/kaldi_decoder-src"
          "${TDL_APP_SHERPA_KWS_DEPS_DIR}/kaldi_native_fbank-src"
          "${TDL_APP_SHERPA_KWS_DEPS_DIR}/kaldifst-src"
          "${TDL_APP_SHERPA_KWS_DEPS_DIR}/openfst-src/src/include"
          "${TDL_APP_SHERPA_KWS_DEPS_DIR}/eigen-src"
          "${TDL_APP_SHERPA_KWS_DEPS_DIR}/json-src/include"
          "${TDL_APP_SHERPA_KWS_DEPS_DIR}/kissfft-src"
          "${TDL_APP_SHERPA_KWS_DEPS_DIR}/simple-sentencepiece-src"
          "${TDL_APP_SHERPA_KWS_DEPS_DIR}/piper_phonemize-src/src"
          "${TDL_APP_SHERPA_KWS_DEPS_DIR}/piper_phonemize-src/src/include"
          "${TDL_APP_SHERPA_KWS_DEPS_DIR}/espeak_ng-src/include"
          "${TDL_APP_SHERPA_KWS_DEPS_DIR}/espeak_ng-src/src/include"
          "${TDL_APP_SHERPA_KWS_DEPS_DIR}/espeak_ng-src/src/ucd-tools/src/include"
          "${TDL_CV184X_INCLUDE_DIRS}"
)
target_link_libraries(tdl_app_npu_kws_core
  PUBLIC
         "${TDL_APP_SHERPA_KWS_LIB_DIR}/libsherpa-onnx-core.a"
         "${TDL_APP_SHERPA_KWS_LIB_DIR}/libkaldi-native-fbank-core.a"
         "${TDL_APP_SHERPA_KWS_LIB_DIR}/libkissfft-float.a"
         "${TDL_APP_SHERPA_KWS_LIB_DIR}/libkaldi-decoder-core.a"
         "${TDL_APP_SHERPA_KWS_LIB_DIR}/libsherpa-onnx-kaldifst-core.a"
         "${TDL_APP_SHERPA_KWS_LIB_DIR}/libssentencepiece_core.a"
         pthread
         "${TDL_APP_SHERPA_KWS_LIB_DIR}/libsherpa-onnx-cv184x-bmrt.so"
         "${TDL_BMRT_LIBRARY}"
         "${TDL_BMLIB_LIBRARY}"
         "${TDL_APP_SHERPA_KWS_LIB_DIR}/libonnxruntime.a"
         m
         "${TDL_APP_SHERPA_KWS_LIB_DIR}/libsherpa-onnx-fstfar.a"
         "${TDL_APP_SHERPA_KWS_LIB_DIR}/libsherpa-onnx-fst.a"
         dl
         "${TDL_APP_SHERPA_KWS_LIB_DIR}/libpiper_phonemize.a"
         "${TDL_APP_SHERPA_KWS_LIB_DIR}/libespeak-ng.a"
         "${TDL_APP_SHERPA_KWS_LIB_DIR}/libucd.a"
         dl pthread
         "${TDL_APP_SHERPA_KWS_LIB_DIR}/libc_compat.a"
         m dl rt pthread
)
target_include_directories(tdl_app_npu_kws_audio
  PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src" "${TDL_CV184X_INCLUDE_DIRS}"
)
target_link_libraries(tdl_app_npu_kws_audio
  PUBLIC
         cvi_audio cvi_RES1 cvi_vqe cvi_dnvqe cvi_VoiceEngine cvi_ssp cvi_ssp2
         aaccomm2 aacenc2 aacdec2 aacsbrenc2 aacsbrdec2 sys
         m dl rt pthread
)
endif()
target_include_directories(tdl_app_npu_asr_core
  PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/include"
  PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${TDL_APP_KALDI_NATIVE_FBANK_DIR}/include"
    "${TDL_CV184X_INCLUDE_DIRS}"
)
target_link_libraries(tdl_app_npu_asr_core
  PUBLIC
    "${TDL_BMRT_LIBRARY}"
    "${TDL_BMLIB_LIBRARY}"
    cvi_audio
    cvi_RES1
    cvi_vqe
    cvi_dnvqe
    cvi_VoiceEngine
    cvi_ssp
    cvi_ssp2
    aaccomm2
    aacenc2
    aacdec2
    aacsbrenc2
    aacsbrdec2
    sys
    tdl_app_kaldi_native_fbank
    tdl_app_kissfft
    m
    dl
    rt
    pthread
)
target_include_directories(tdl_app_speaker_core
  PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/include"
  PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${TDL_APP_KALDI_NATIVE_FBANK_DIR}/include"
    "${TDL_CV184X_INCLUDE_DIRS}"
)
target_link_libraries(tdl_app_speaker_core
  PUBLIC
    "${TDL_BMRT_LIBRARY}"
    "${TDL_BMLIB_LIBRARY}"
    cvi_audio
    cvi_RES1
    cvi_vqe
    cvi_dnvqe
    cvi_VoiceEngine
    cvi_ssp
    cvi_ssp2
    aaccomm2
    aacenc2
    aacdec2
    aacsbrenc2
    aacsbrdec2
    sys
    tdl_app_kaldi_native_fbank
    tdl_app_kissfft
    m
    dl
    rt
    pthread
)

target_include_directories(tdl_app_sdk
  PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/include"
  PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${TDL_APP_KALDI_NATIVE_FBANK_DIR}/include"
    "${TDL_CV184X_INCLUDE_DIRS}"
)

target_link_libraries(tdl_app_sdk
  PUBLIC
    ${TDL_CV184X_LIBS}
    tdl_app_ini_compat
)

if(TDL_SOPHPI_ROOT AND EXISTS "${TDL_SOPHPI_ROOT}/cvi_mpi/lib/libsys.a")
  target_link_libraries(tdl_app_sdk PUBLIC
    "${TDL_SOPHPI_ROOT}/cvi_mpi/lib/libsys.a")
endif()

if(TDL_APP_HAS_FFMPEG)
  target_link_libraries(tdl_app_sdk
    PUBLIC
      "${TDL_APP_AVFORMAT_LIBRARY}"
      "${TDL_APP_AVCODEC_LIBRARY}"
      "${TDL_APP_SWRESAMPLE_LIBRARY}"
      "${TDL_APP_AVUTIL_LIBRARY}"
  )
  target_link_options(tdl_app_sdk PUBLIC
    "-Wl,-rpath-link,${TDL_APP_AUDIO_FFMPEG_ROOT}/lib"
    "-Wl,-rpath-link,${TDL_APP_AUDIO_FFMPEG_ROOT}/../lib")
endif()

if(TDL_APP_BUILD_MMF_MINIMAL)
  add_subdirectory(jyd_tdl_mmf_minimal)
endif()

if(TDL_APP_BUILD_PYTHON)
  if(TDL_APP_PY_WITH_NPU AND TDL_APP_MEDIA_MINIMAL)
    message(FATAL_ERROR
      "TDL_APP_PY_WITH_NPU requires the algorithm sources; "
      "disable TDL_APP_MEDIA_MINIMAL or set TDL_APP_PY_WITH_NPU=OFF.")
  endif()
  if(NOT EXISTS "${TDL_APP_PYTHON_INCLUDE_DIR}/Python.h")
    message(FATAL_ERROR
      "Missing target Python headers: ${TDL_APP_PYTHON_INCLUDE_DIR}/Python.h\n"
      "Set TDL_APP_PYTHON_INCLUDE_DIR or disable TDL_APP_BUILD_PYTHON.")
  endif()

  # tdl_app_sdk is linked into the shared python module, so it (and the ini
  # helper it drags in) must be compiled as position-independent code.
  set_target_properties(tdl_app_sdk tdl_app_ini_compat PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

  # nanobind is compiled statically into the module (nb_combined.cpp), which
  # avoids find_package(Python) and works with the cross toolchain. Flags
  # follow the manual-build recipe documented in nb_combined.cpp.
  add_library(tdl_py MODULE
    python/tdl_py_module.cpp
    third_party/nanobind/src/nb_combined.cpp
  )
  set_target_properties(tdl_py PROPERTIES
    PREFIX ""
    OUTPUT_NAME "tdl_py"
    SUFFIX ".so"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_VISIBILITY_PRESET hidden
  )
  target_compile_definitions(tdl_py PRIVATE NB_COMPACT_ASSERTIONS)
  if(TDL_APP_PY_WITH_NPU)
    target_compile_definitions(tdl_py PRIVATE TDL_PY_WITH_NPU)
    target_include_directories(tdl_py PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src")
  endif()
  target_compile_options(tdl_py PRIVATE
    -fno-strict-aliasing
    -ffunction-sections
    -fdata-sections
  )
  target_include_directories(tdl_py PRIVATE
    "${TDL_APP_PYTHON_INCLUDE_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/nanobind/include"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/nanobind/ext/robin_map/include"
    "${TDL_CV184X_INCLUDE_DIRS}"
  )
  target_link_libraries(tdl_py PRIVATE tdl_app_sdk)
  target_link_options(tdl_py PRIVATE -Wl,--gc-sections -Wl,--as-needed)

  install(TARGETS tdl_py LIBRARY DESTINATION lib/python)
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
    # FreeType is only needed by the two PP-OCR demos (UTF-8 overlay text).
    # When the cross-compiled rootfs is not available, skip those demos
    # instead of failing the whole configure.
    if(TDL_APP_FREETYPE_INCLUDE_DIR AND TDL_APP_FREETYPE_LIBRARY AND
       TDL_APP_BZ2_LIBRARY AND TDL_APP_PNG_LIBRARY AND
       TDL_APP_BROTLI_DEC_LIBRARY AND TDL_APP_BROTLI_COMMON_LIBRARY AND
       TDL_APP_ROOTFS_Z_LIBRARY)
      set(TDL_APP_HAS_FREETYPE ON)
      set(TDL_APP_FREETYPE_LIBRARIES
        "${TDL_APP_FREETYPE_LIBRARY}"
        "${TDL_APP_BZ2_LIBRARY}"
        "${TDL_APP_PNG_LIBRARY}"
        "${TDL_APP_BROTLI_DEC_LIBRARY}"
        "${TDL_APP_BROTLI_COMMON_LIBRARY}"
        "${TDL_APP_ROOTFS_Z_LIBRARY}"
      )
    else()
      set(TDL_APP_HAS_FREETYPE OFF)
      message(WARNING
        "FreeType rootfs not found (TDL_APP_FREETYPE_ROOT=${TDL_APP_FREETYPE_ROOT}); "
        "skipping tdl_pp_ocr_demo and tdl_pp_ocr_camera_demo.")
    endif()

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

    add_executable(tdl_speaker_recognition_demo
      apps/tdl_speaker_recognition_demo.cpp
    )
    target_include_directories(tdl_speaker_recognition_demo PRIVATE
      "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_speaker_recognition_demo PRIVATE tdl_app_speaker_core)

    add_executable(tdl_npu_asr_demo apps/tdl_npu_asr_demo.cpp)
    target_include_directories(tdl_npu_asr_demo PRIVATE
      "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
    target_link_libraries(tdl_npu_asr_demo PRIVATE tdl_app_npu_asr_core)

    if(TDL_APP_BUILD_KWS)
      add_executable(tdl_npu_keyword_spotter_demo apps/tdl_npu_keyword_spotter_demo.cpp)
      set_property(TARGET tdl_npu_keyword_spotter_demo PROPERTY CXX_STANDARD 17)
      target_include_directories(tdl_npu_keyword_spotter_demo PRIVATE
        "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
      target_link_libraries(tdl_npu_keyword_spotter_demo PRIVATE
        tdl_app_npu_kws_core tdl_app_npu_kws_audio)

      # Keeps the validated KWS runtime independent from MMF/AI libraries.
      add_executable(tdl_npu_keyword_spotter_pcm_smoke
        apps/tdl_npu_keyword_spotter_pcm_smoke.cpp)
      target_include_directories(tdl_npu_keyword_spotter_pcm_smoke PRIVATE
        "${TDL_CV184X_INCLUDE_DIRS}" "${CMAKE_CURRENT_LIST_DIR}/apps")
      target_link_libraries(tdl_npu_keyword_spotter_pcm_smoke PRIVATE tdl_app_npu_kws_core)
    endif()

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

    if(TDL_APP_HAS_FREETYPE)
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
    endif()

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

    add_executable(dual_benchmark_led_test
      apps/dual_benchmark_led_test.cpp
      src/media/rgb_led.cpp
    )
    target_include_directories(dual_benchmark_led_test PRIVATE
      "${CMAKE_CURRENT_LIST_DIR}/include"
      "${CMAKE_CURRENT_LIST_DIR}/apps")
    if(TDL_SOPHPI_ROOT AND EXISTS "${TDL_SOPHPI_ROOT}/cvi_mpi/lib/libsys.a")
      target_link_libraries(dual_benchmark_led_test PRIVATE
        "${TDL_SOPHPI_ROOT}/cvi_mpi/lib/libsys.a"
        dl
        rt
        pthread
      )
    else()
      message(FATAL_ERROR "dual_benchmark_led_test requires cvi_mpi/lib/libsys.a")
    endif()
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
      tdl_npu_asr_demo
      tdl_vad_demo
      tdl_speaker_recognition_demo
      tdl_face_detect_demo
      tdl_face_attribute_demo
      tdl_face_attr_pipeline_demo
      tdl_face_recognition_demo
      tdl_plate_recognize_demo
      tdl_camera_detect_demo
      tdl_camera_scrfd_benchmark_demo
      tdl_camera_tdlsdk_face_benchmark_demo
      tdl_camera_yolov8_benchmark_demo
      tdl_camera_classify_demo
      tdl_camera_feature_demo
      sophpi_ai_osd_demo
      tdl_benchmark_demo
      dual_benchmark_led_test
      RUNTIME DESTINATION bin
    )

    if(TDL_APP_BUILD_KWS)
      install(TARGETS tdl_npu_keyword_spotter_demo RUNTIME DESTINATION bin)
    endif()

    if(TDL_APP_HAS_FREETYPE)
      install(TARGETS
        tdl_pp_ocr_demo
        tdl_pp_ocr_camera_demo
        RUNTIME DESTINATION bin
      )
    endif()
  endif()
endif()

install(DIRECTORY include/
  DESTINATION include
)

install(DIRECTORY configs/
  DESTINATION configs
)
