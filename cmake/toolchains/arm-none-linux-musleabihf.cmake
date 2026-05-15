set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT DEFINED TOOLCHAIN_ROOT)
  set(TOOLCHAIN_ROOT "$ENV{TOOLCHAIN_ROOT}")
endif()

if(NOT TOOLCHAIN_ROOT)
  message(FATAL_ERROR "Set TOOLCHAIN_ROOT to arm-none-linux-musleabihf toolchain root")
endif()

set(CMAKE_C_COMPILER "${TOOLCHAIN_ROOT}/bin/arm-none-linux-musleabihf-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_ROOT}/bin/arm-none-linux-musleabihf-g++")

set(CMAKE_FIND_ROOT_PATH "${TOOLCHAIN_ROOT}/arm-none-linux-musleabihf")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mcpu=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcpu=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard")
