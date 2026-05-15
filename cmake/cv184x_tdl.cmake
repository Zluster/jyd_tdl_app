if(NOT EXISTS "${TDL_APP_THIRD_PARTY_DIR}")
  message(FATAL_ERROR
    "Missing dependency bundle: ${TDL_APP_THIRD_PARTY_DIR}\n"
    "Run scripts/collect_cv184x_deps.sh in the Sophpi SDK, or set "
    "-DTDL_APP_THIRD_PARTY_DIR=/path/to/cv184x bundle.")
endif()

set(TDL_CV184X_INCLUDE_DIRS
  "${TDL_APP_THIRD_PARTY_DIR}/include"
  "${TDL_APP_THIRD_PARTY_DIR}/include/framework"
  "${TDL_APP_THIRD_PARTY_DIR}/include/components"
  "${TDL_APP_THIRD_PARTY_DIR}/include/nn"
  "${TDL_APP_THIRD_PARTY_DIR}/cvi_mpi/include"
  "${TDL_APP_THIRD_PARTY_DIR}/cvi_mpi/include/isp"
  "${CMAKE_CURRENT_LIST_DIR}/../third_party/vendor/ini"
  "${TDL_APP_THIRD_PARTY_DIR}/cvi_rtsp/include"
  "${TDL_APP_THIRD_PARTY_DIR}/opencv/include"
)

set(TDL_CV184X_LIBRARY_DIRS
  "${TDL_APP_THIRD_PARTY_DIR}/lib"
  "${TDL_APP_THIRD_PARTY_DIR}/opencv/lib"
)

foreach(dir IN LISTS TDL_CV184X_LIBRARY_DIRS)
  if(EXISTS "${dir}")
    link_directories("${dir}")
  endif()
endforeach()

set(TDL_CV184X_LIBS
  tdl_core
  opencv_imgcodecs
  opencv_imgproc
  opencv_core
  bmrt
  bmlib
  model_combine
  cvi_rtsp
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
  z
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
