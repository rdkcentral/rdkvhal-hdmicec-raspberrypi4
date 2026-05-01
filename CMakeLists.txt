# Minimum CMake version required
cmake_minimum_required(VERSION 3.10)

# RCECHal version
project(RCECHal VERSION 1.4.0 LANGUAGES C)

set(LIBNAME RCECHal)
set(DEFAULT_BUILD_TYPE "Release")

message(STATUS "Building with Linux CEC subsystem-based implementation")
set(SRCS hdmi_cec_driver_linuxcec.c)

# Full CEC device node path: /dev/cec0 = HDMI0 (default), /dev/cec1 = HDMI1
set(CEC_DEV_NODE "/dev/cec0" CACHE STRING "CEC device node path (e.g. /dev/cec0 for HDMI0, /dev/cec1 for HDMI1)")
message(STATUS "CEC device node: ${CEC_DEV_NODE}")

# CEC identity/profile defaults; override for other platforms/devices.
# NOTE: 0x00BC44 is the Raspberry Pi/Broadcom-specific default OUI used by this repo.
# Override CEC_VENDOR_ID for non-RPi/non-Broadcom products.
set(CEC_VENDOR_ID "0x00BC44" CACHE STRING "CEC vendor ID (24-bit hex, e.g. 0x0019FB)")
set(CEC_PRIMARY_DEVICE_TYPE "CEC_OP_PRIM_DEVTYPE_TUNER" CACHE STRING "Primary CEC device type macro")
set(CEC_LOG_ADDR_TYPE "CEC_LOG_ADDR_TYPE_TUNER" CACHE STRING "CEC logical address type macro")
set(CEC_ALL_DEVICE_TYPE "CEC_OP_ALL_DEVTYPE_TUNER" CACHE STRING "CEC all-device type macro")
message(STATUS "CEC vendor id: ${CEC_VENDOR_ID}")
message(STATUS "CEC profile: primary=${CEC_PRIMARY_DEVICE_TYPE}, log_addr=${CEC_LOG_ADDR_TYPE}, all=${CEC_ALL_DEVICE_TYPE}")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to '${DEFAULT_BUILD_TYPE}' as none was specified.")
    set(CMAKE_BUILD_TYPE "${DEFAULT_BUILD_TYPE}" CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

include(GNUInstallDirs)
find_package(Threads REQUIRED)

# Get git commit SHA
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_COMMIT_SHA)
    set(GIT_COMMIT_SHA "unknown")
endif()
message(STATUS "Git commit SHA: ${GIT_COMMIT_SHA}")

find_path(HDMI_CEC_INCLUDE_DIR hdmi_cec_driver.h
    PATHS
        ${CMAKE_CURRENT_SOURCE_DIR}/../rdk-halif-hdmi_cec/include
        /usr/include
        /usr/local/include
    PATH_SUFFIXES ccec/drivers
)

if(NOT HDMI_CEC_INCLUDE_DIR)
    message(STATUS "HDMI CEC header not found in system paths, using local path")
    set(HDMI_CEC_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../rdk-halif-hdmi_cec/include)
endif()

add_library(${LIBNAME} SHARED ${SRCS})

set_target_properties(${LIBNAME} PROPERTIES
	VERSION ${PROJECT_VERSION}
	SOVERSION ${PROJECT_VERSION}
)

target_include_directories(${LIBNAME} PRIVATE ${HDMI_CEC_INCLUDE_DIR})
target_link_libraries(${LIBNAME} PRIVATE Threads::Threads)

target_compile_definitions(${LIBNAME} PRIVATE
    HAL_VERSION="${PROJECT_VERSION}"
    GIT_COMMIT_SHA="${GIT_COMMIT_SHA}"
    CEC_DEV_NODE="${CEC_DEV_NODE}"
    CEC_VENDOR_ID=${CEC_VENDOR_ID}
    CEC_PRIMARY_DEVICE_TYPE=${CEC_PRIMARY_DEVICE_TYPE}
    CEC_LOG_ADDR_TYPE=${CEC_LOG_ADDR_TYPE}
    CEC_ALL_DEVICE_TYPE=${CEC_ALL_DEVICE_TYPE}
)

add_custom_target(clean-all
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/clean.cmake
)

install(TARGETS ${LIBNAME}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# Define package configuration variables
set(PACKAGE_NAME ${LIBNAME})
set(PACKAGE_VERSION ${PROJECT_VERSION})
set(PACKAGE_DESCRIPTION "RPI-4 HDMI CEC HAL Library (Linux CEC subsystem)")
set(PACKAGE_LIBS "-L${CMAKE_INSTALL_PREFIX}/lib -l${LIBNAME}")
set(PACKAGE_CFLAGS "-I${CMAKE_INSTALL_PREFIX}/include")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/RCECHal.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/RCECHal.pc
    @ONLY
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/RCECHal.pc
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/pkgconfig
)
