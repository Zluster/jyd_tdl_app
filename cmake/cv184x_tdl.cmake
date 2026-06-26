include(CheckIncludeFileCXX)

if(NOT EXISTS "${TDL_APP_THIRD_PARTY_DIR}")
  message(FATAL_ERROR
    "Missing dependency bundle: ${TDL_APP_THIRD_PARTY_DIR}\n"
    "Run scripts/collect_cv184x_deps.sh in the Sophpi SDK, or set "
    "-DTDL_APP_THIRD_PARTY_DIR=/path/to/cv184x bundle.")
endif()

set(TDL_SOPHPI_ROOT "${TDL_SOPHPI_ROOT}")
if(NOT TDL_SOPHPI_ROOT)
  if(EXISTS "/home/jyd/zwz/sophpi")
    set(TDL_SOPHPI_ROOT "/home/jyd/zwz/sophpi")
  endif()
endif()

function(tdl_pick_library out_var)
  set(result "")
  foreach(candidate ${ARGN})
    if(EXISTS "${candidate}")
      set(result "${candidate}")
      break()
    endif()
  endforeach()
  set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

function(tdl_append_existing_dirs out_var)
  set(result "${${out_var}}")
  foreach(candidate ${ARGN})
    if(EXISTS "${candidate}")
      list(APPEND result "${candidate}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES result)
  set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

set(TDL_FALLBACK_LIB_DIRS "")
if(TDL_SOPHPI_ROOT)
  tdl_append_existing_dirs(TDL_FALLBACK_LIB_DIRS
    "${TDL_SOPHPI_ROOT}/cvi_mpi/lib"
    "${TDL_SOPHPI_ROOT}/cvi_mpi/lib/musl_arm_dual"
    "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/lib"
    "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/dual_os/lib"
    "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/opencv/lib"
    "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/dual_os/opencv/lib"
    "${TDL_SOPHPI_ROOT}/libsophon/install/libsophon-0.4.9/lib"
    "${TDL_SOPHPI_ROOT}/install/soc_cv1843hp_jyd_common_sd_dualos_alios/rootfs/system/lib"
    "${TDL_SOPHPI_ROOT}/install/soc_cv1843hp_jyd_common_sd_dualos_alios/rootfs/system/usr/lib"
    "${TDL_SOPHPI_ROOT}/install/soc_cv1843hp_jyd_common_sd/rootfs/system/lib"
    "${TDL_SOPHPI_ROOT}/install/soc_cv1843hp_jyd_common_sd/rootfs/system/usr/lib"
  )
endif()

set(TDL_OPENCV_INCLUDE_DIR "")
if(EXISTS "${TDL_APP_THIRD_PARTY_DIR}/opencv/include")
  set(TDL_OPENCV_INCLUDE_DIR "${TDL_APP_THIRD_PARTY_DIR}/opencv/include")
elseif(EXISTS "${TDL_APP_THIRD_PARTY_DIR}/include/opencv4")
  set(TDL_OPENCV_INCLUDE_DIR "${TDL_APP_THIRD_PARTY_DIR}/include/opencv4")
elseif(TDL_SOPHPI_ROOT AND EXISTS "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/dual_os/opencv/include")
  set(TDL_OPENCV_INCLUDE_DIR "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/dual_os/opencv/include")
elseif(TDL_SOPHPI_ROOT AND EXISTS "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/opencv/include")
  set(TDL_OPENCV_INCLUDE_DIR "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/opencv/include")
endif()

set(TDL_OPENCV_LIB_DIR "")
if(EXISTS "${TDL_APP_THIRD_PARTY_DIR}/opencv/lib")
  set(TDL_OPENCV_LIB_DIR "${TDL_APP_THIRD_PARTY_DIR}/opencv/lib")
elseif(EXISTS "${TDL_APP_THIRD_PARTY_DIR}/lib")
  set(TDL_OPENCV_LIB_DIR "${TDL_APP_THIRD_PARTY_DIR}/lib")
elseif(TDL_SOPHPI_ROOT AND EXISTS "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/dual_os/opencv/lib")
  set(TDL_OPENCV_LIB_DIR "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/dual_os/opencv/lib")
elseif(TDL_SOPHPI_ROOT AND EXISTS "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/opencv/lib")
  set(TDL_OPENCV_LIB_DIR "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/opencv/lib")
endif()

if(NOT TDL_OPENCV_INCLUDE_DIR OR NOT TDL_OPENCV_LIB_DIR)
  message(FATAL_ERROR
    "Missing OpenCV headers/libs under ${TDL_APP_THIRD_PARTY_DIR}/opencv")
endif()

set(OpenCV_INCLUDE_DIRS "${TDL_OPENCV_INCLUDE_DIR}")
tdl_pick_library(TDL_OPENCV_CORE_LIBRARY
  "${TDL_OPENCV_LIB_DIR}/libopencv_core.so.4.5.0"
  "${TDL_OPENCV_LIB_DIR}/libopencv_core.so"
  "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/dual_os/opencv/lib/libopencv_core.so.4.5.0"
  "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/opencv/lib/libopencv_core.so.4.5.0"
  "${TDL_SOPHPI_ROOT}/tdl_sdk/install/CV184X/sample/3rd/opencv/lib/libopencv_core.so.4.5.0")
tdl_pick_library(TDL_OPENCV_IMGPROC_LIBRARY
  "${TDL_OPENCV_LIB_DIR}/libopencv_imgproc.so.4.5.0"
  "${TDL_OPENCV_LIB_DIR}/libopencv_imgproc.so"
  "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/dual_os/opencv/lib/libopencv_imgproc.so.4.5.0"
  "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/opencv/lib/libopencv_imgproc.so.4.5.0")
tdl_pick_library(TDL_OPENCV_IMGCODECS_LIBRARY
  "${TDL_OPENCV_LIB_DIR}/libopencv_imgcodecs.so.4.5.0"
  "${TDL_OPENCV_LIB_DIR}/libopencv_imgcodecs.so"
  "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/dual_os/opencv/lib/libopencv_imgcodecs.so.4.5.0"
  "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/opencv/lib/libopencv_imgcodecs.so.4.5.0")

if(NOT TDL_OPENCV_CORE_LIBRARY OR NOT TDL_OPENCV_IMGPROC_LIBRARY OR
   NOT TDL_OPENCV_IMGCODECS_LIBRARY)
  message(FATAL_ERROR "Missing usable OpenCV shared libraries for CV184X build")
endif()

set(OpenCV_LIBS
  "${TDL_OPENCV_CORE_LIBRARY}"
  "${TDL_OPENCV_IMGPROC_LIBRARY}"
  "${TDL_OPENCV_IMGCODECS_LIBRARY}"
)

set(TDL_ZLIB_LIBRARY "")
foreach(candidate
    "${TDL_APP_THIRD_PARTY_DIR}/lib/libz.so.1.2.11"
    "${TDL_APP_THIRD_PARTY_DIR}/lib/libz.so.1"
    "${TDL_APP_THIRD_PARTY_DIR}/lib/libz.so"
    "${TDL_APP_THIRD_PARTY_DIR}/opencv/lib/libz.so"
    "${TDL_APP_THIRD_PARTY_DIR}/opencv/lib/libz.a"
    "/home/jyd/zwz/sophpi/tdl_sdk/build/CV184X/_deps/opencv-src/lib/opencv4/3rdparty/libzlib.a"
    "/home/jyd/zwz/sophpi/host-tools/gcc/gcc-buildroot-9.3.0-aarch64-linux-gnu/lib/libz.a")
  if(EXISTS "${candidate}")
    set(TDL_ZLIB_LIBRARY "${candidate}")
    break()
  endif()
endforeach()

if(NOT TDL_ZLIB_LIBRARY)
  message(FATAL_ERROR "Missing usable zlib library for CV184X build")
endif()

set(TDL_TINYALSA_LIBRARY "")
foreach(candidate
    "${TDL_APP_THIRD_PARTY_DIR}/lib/libtinyalsa.so"
    "${TDL_APP_THIRD_PARTY_DIR}/lib/libtinyalsa.a"
    "${TDL_SOPHPI_ROOT}/cvi_mpi/lib/musl_arm_dual/libtinyalsa.so"
    "${TDL_SOPHPI_ROOT}/cvi_mpi/lib/libtinyalsa.so"
    "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/dual_os/lib/libtinyalsa.so"
    "${TDL_SOPHPI_ROOT}/install/soc_cv1843hp_jyd_common_sd_dualos_alios/rootfs/system/usr/lib/libtinyalsa.so")
  if(EXISTS "${candidate}")
    set(TDL_TINYALSA_LIBRARY "${candidate}")
    break()
  endif()
endforeach()

if(NOT TDL_TINYALSA_LIBRARY)
  message(FATAL_ERROR "Missing usable tinyalsa library for CV184X build")
endif()

set(TDL_AUDIO_FBANK_HINTS
  "${TDL_APP_THIRD_PARTY_DIR}/lib/libkaldi-native-fbank-core.a"
  "/home/jyd/zwz/sophpi/tdl_sdk/install/CV184X/lib/libkaldi-native-fbank-core.a"
  "/home/jyd/zwz/sophpi/tdl_sdk/build/CV184X/_deps/kaldi-native-fbank-build/libkaldi-native-fbank-core.a"
  "/home/jyd/zwz/sophpi/tdl_sdk/build_jyd/CV184X/deps/kaldi-native-fbank-build/libkaldi-native-fbank-core.a"
)

set(TDL_KALDI_NATIVE_FBANK_LIBRARY "")
foreach(candidate IN LISTS TDL_AUDIO_FBANK_HINTS)
  if(EXISTS "${candidate}")
    set(TDL_KALDI_NATIVE_FBANK_LIBRARY "${candidate}")
    break()
  endif()
endforeach()

set(TDL_HAS_AUDIO_RUNTIME OFF)
if(TDL_KALDI_NATIVE_FBANK_LIBRARY)
  set(TDL_HAS_AUDIO_RUNTIME ON)
endif()

check_include_file_cxx("linux/fb.h" TDL_HAS_LINUX_FB_H)

set(TDL_CV184X_INCLUDE_DIRS
  "${TDL_APP_THIRD_PARTY_DIR}/include"
  "${TDL_APP_THIRD_PARTY_DIR}/include/framework"
  "${TDL_APP_THIRD_PARTY_DIR}/include/components"
  "${TDL_APP_THIRD_PARTY_DIR}/include/nn"
  "${TDL_APP_THIRD_PARTY_DIR}/cvi_mpi/include"
  "${TDL_APP_THIRD_PARTY_DIR}/cvi_mpi/include/isp"
  "${TDL_OPENCV_INCLUDE_DIR}"
  "${CMAKE_CURRENT_LIST_DIR}/../third_party/vendor/ini"
)

set(TDL_CV184X_LIBRARY_DIRS
  "${TDL_APP_THIRD_PARTY_DIR}/lib"
  "${TDL_OPENCV_LIB_DIR}"
)
tdl_append_existing_dirs(TDL_CV184X_LIBRARY_DIRS ${TDL_FALLBACK_LIB_DIRS})

foreach(dir IN LISTS TDL_CV184X_LIBRARY_DIRS)
  if(EXISTS "${dir}")
    link_directories("${dir}")
  endif()
endforeach()

tdl_pick_library(TDL_CORE_LIBRARY
  "${TDL_APP_THIRD_PARTY_DIR}/lib/libtdl_core-static.a"
  "${TDL_APP_THIRD_PARTY_DIR}/lib/libtdl_core.so"
  "${TDL_SOPHPI_ROOT}/tdl_sdk/install/CV184X/lib/libtdl_core-static.a")

if(NOT TDL_CORE_LIBRARY)
  message(FATAL_ERROR "Missing usable tdl_core library for CV184X build")
endif()

tdl_pick_library(TDL_BMRT_LIBRARY
  "${TDL_APP_THIRD_PARTY_DIR}/lib/libbmrt.so.1.0"
  "${TDL_APP_THIRD_PARTY_DIR}/lib/libbmrt.so"
  "${TDL_APP_THIRD_PARTY_DIR}/lib/libbmrt.a"
  "${TDL_SOPHPI_ROOT}/install/soc_cv1843hp_jyd_common_sd_dualos_alios/rootfs/system/lib/libbmrt.so.1.0"
  "${TDL_SOPHPI_ROOT}/install/soc_cv1843hp_jyd_common_sd_dualos_alios/rootfs/system/lib/libbmrt.a"
  "${TDL_SOPHPI_ROOT}/libsophon/install/libsophon-0.4.9/lib/libbmrt.so.1.0"
  "${TDL_SOPHPI_ROOT}/libsophon/install/libsophon-0.4.9/lib/libbmrt.a")

tdl_pick_library(TDL_BMLIB_LIBRARY
  "${TDL_APP_THIRD_PARTY_DIR}/lib/libbmlib.so.0"
  "${TDL_APP_THIRD_PARTY_DIR}/lib/libbmlib.so"
  "${TDL_APP_THIRD_PARTY_DIR}/lib/libbmlib.a"
  "${TDL_SOPHPI_ROOT}/install/soc_cv1843hp_jyd_common_sd_dualos_alios/rootfs/system/lib/libbmlib.so.0"
  "${TDL_SOPHPI_ROOT}/install/soc_cv1843hp_jyd_common_sd_dualos_alios/rootfs/system/lib/libbmlib.a"
  "${TDL_SOPHPI_ROOT}/libsophon/install/libsophon-0.4.9/lib/libbmlib.so.0"
  "${TDL_SOPHPI_ROOT}/libsophon/install/libsophon-0.4.9/lib/libbmlib.a")

tdl_pick_library(TDL_BMODEL_LIBRARY
  "${TDL_APP_THIRD_PARTY_DIR}/lib/libbmodel.a"
  "${TDL_SOPHPI_ROOT}/install/soc_cv1843hp_jyd_common_sd_dualos_alios/rootfs/system/lib/libbmodel.a"
  "${TDL_SOPHPI_ROOT}/libsophon/install/libsophon-0.4.9/lib/libbmodel.a")

tdl_pick_library(TDL_MODEL_COMBINE_LIBRARY
  "${TDL_APP_THIRD_PARTY_DIR}/lib/libmodel_combine.so"
  "${TDL_APP_THIRD_PARTY_DIR}/lib/libmodel_combine.a"
  "${TDL_SOPHPI_ROOT}/install/soc_cv1843hp_jyd_common_sd_dualos_alios/rootfs/system/lib/libmodel_combine.so"
  "${TDL_SOPHPI_ROOT}/libsophon/install/libsophon-0.4.9/lib/libmodel_combine.so")

tdl_pick_library(TDL_SENSOR_CV2003_LIBRARY
  "${TDL_APP_THIRD_PARTY_DIR}/lib/libsns_cv2003.so"
  "${TDL_APP_THIRD_PARTY_DIR}/lib/libsns_cv2003.a"
  "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/dual_os/lib/libsns_cv2003.so"
  "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/lib/libsns_cv2003.so"
  "${TDL_SOPHPI_ROOT}/install/soc_cv1843hp_jyd_common_sd/rootfs/system/usr/lib/libsns_cv2003.so")

tdl_pick_library(TDL_SENSOR_GC2053_LIBRARY
  "${TDL_APP_THIRD_PARTY_DIR}/lib/libsns_gc2053.so"
  "${TDL_APP_THIRD_PARTY_DIR}/lib/libsns_gc2053.a"
  "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/dual_os/lib/libsns_gc2053.so"
  "${TDL_SOPHPI_ROOT}/tdl_app_sdk/third_party/cv184x/lib/libsns_gc2053.so"
  "${TDL_SOPHPI_ROOT}/install/soc_cv1843hp_jyd_common_sd/rootfs/system/usr/lib/libsns_gc2053.so")

if(NOT TDL_BMRT_LIBRARY OR NOT TDL_BMLIB_LIBRARY OR NOT TDL_BMODEL_LIBRARY OR
   NOT TDL_MODEL_COMBINE_LIBRARY)
  message(FATAL_ERROR
    "Missing one or more TPU runtime libraries for CV184X build")
endif()

set(TDL_CV184X_LIBS
  "${TDL_CORE_LIBRARY}"
  ${OpenCV_LIBS}
  "${TDL_BMRT_LIBRARY}"
  "${TDL_BMLIB_LIBRARY}"
  "${TDL_BMODEL_LIBRARY}"
  "${TDL_MODEL_COMBINE_LIBRARY}"
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
  mipi
  vi
  vpss
  vdec
  venc
  vo
  rgn
  isp
  isp_algo
  ae
  awb
  af
  sensor
  sensor_cfg
  sensor_i2c
  sns_full
  ${TDL_TINYALSA_LIBRARY}
  "${TDL_ZLIB_LIBRARY}"
  dl
  rt
  pthread
)

if(TDL_SENSOR_CV2003_LIBRARY)
  list(APPEND TDL_CV184X_LIBS "${TDL_SENSOR_CV2003_LIBRARY}")
endif()

if(TDL_SENSOR_GC2053_LIBRARY)
  list(APPEND TDL_CV184X_LIBS "${TDL_SENSOR_GC2053_LIBRARY}")
else()
  message(WARNING
    "GC2053 sensor runtime library was not found in TDL_APP_THIRD_PARTY_DIR or "
    "TDL_SOPHPI_ROOT. CV2003 builds remain available, but GC2053 runtime bring-up "
    "will require building or installing libsns_gc2053.so.")
endif()

if(TDL_HAS_AUDIO_RUNTIME)
  list(APPEND TDL_CV184X_LIBS "${TDL_KALDI_NATIVE_FBANK_LIBRARY}")
endif()

set(TDL_CV184X_RUNTIME_DIR "${TDL_APP_THIRD_PARTY_DIR}/runtime")
set(TDL_CV184X_MODEL_DIR "${TDL_APP_THIRD_PARTY_DIR}/models")
set(TDL_CV184X_CONFIG_DIR "${TDL_APP_THIRD_PARTY_DIR}/configs")

add_compile_definitions(
  TDL_APP_DEFAULT_MODEL_DIR="${TDL_CV184X_MODEL_DIR}"
  TDL_APP_DEFAULT_CONFIG_DIR="${TDL_CV184X_CONFIG_DIR}"
)
