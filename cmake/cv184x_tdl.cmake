include(CheckIncludeFileCXX)

if(NOT EXISTS "${TDL_APP_THIRD_PARTY_DIR}")
  message(FATAL_ERROR
    "Missing dependency bundle: ${TDL_APP_THIRD_PARTY_DIR}\n"
    "Run scripts/collect_cv184x_deps.sh in the Sophpi SDK, or set "
    "-DTDL_APP_THIRD_PARTY_DIR=/path/to/cv184x bundle.")
endif()

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
find_package(ZLIB REQUIRED)
find_library(TDL_TINYALSA_LIBRARY NAMES tinyalsa REQUIRED)

check_include_file_cxx("linux/fb.h" TDL_HAS_LINUX_FB_H)

set(TDL_CV184X_INCLUDE_DIRS
  "${TDL_APP_THIRD_PARTY_DIR}/include"
  "${TDL_APP_THIRD_PARTY_DIR}/include/framework"
  "${TDL_APP_THIRD_PARTY_DIR}/include/components"
  "${TDL_APP_THIRD_PARTY_DIR}/include/nn"
  "${TDL_APP_THIRD_PARTY_DIR}/cvi_mpi/include"
  "${TDL_APP_THIRD_PARTY_DIR}/cvi_mpi/include/isp"
  "${CMAKE_CURRENT_LIST_DIR}/../third_party/vendor/ini"
  "${TDL_APP_THIRD_PARTY_DIR}/cvi_rtsp/include"
  ${OpenCV_INCLUDE_DIRS}
)

set(TDL_CV184X_LIBRARY_DIRS
  "${TDL_APP_THIRD_PARTY_DIR}/lib"
)

foreach(dir IN LISTS TDL_CV184X_LIBRARY_DIRS)
  if(EXISTS "${dir}")
    link_directories("${dir}")
  endif()
endforeach()

set(TDL_CV184X_LIBS
  tdl_core
  ${OpenCV_LIBS}
  bmrt
  bmlib
  model_combine
  cvi_rtsp
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
  sns_cv2003
  teaisp
  ${TDL_TINYALSA_LIBRARY}
  ZLIB::ZLIB
  dl
  rt
  pthread
)

set(TDL_CV184X_RUNTIME_DIR "${TDL_APP_THIRD_PARTY_DIR}/runtime")
set(TDL_CV184X_MODEL_DIR "${TDL_APP_THIRD_PARTY_DIR}/models")
set(TDL_CV184X_CONFIG_DIR "${TDL_APP_THIRD_PARTY_DIR}/configs")

add_compile_definitions(
  TDL_APP_DEFAULT_MODEL_DIR="${TDL_CV184X_MODEL_DIR}"
  TDL_APP_DEFAULT_CONFIG_DIR="${TDL_CV184X_CONFIG_DIR}"
)
