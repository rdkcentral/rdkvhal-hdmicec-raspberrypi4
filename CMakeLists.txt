# Minimum CMake version required
cmake_minimum_required(VERSION 3.10)

# Version is matched with the version of hdmicecheader - 1.3.7.
project(RCECHal VERSION 1.4.0 LANGUAGES C)

set(LIBNAME RCECHal)
set(DEFAULT_BUILD_TYPE "Release")

set_target_properties(${LIBNAME} PROPERTIES
	VERSION ${PROJECT_VERSION}
	SOVERSION ${PROJECT_VERSION}
)

message(STATUS "Building with USERLAND-based CEC implementation (experimental)")
set(SRCS hdmi_cec_driver_userland.c)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	message(STATUS "Setting build type to '${DEFAULT_BUILD_TYPE}' as none was specified.")
	set(CMAKE_BUILD_TYPE "${DEFAULT_BUILD_TYPE}" CACHE STRING "Choose the type of build." FORCE)
	set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

include(GNUInstallDirs)

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
target_compile_definitions(${LIBNAME} PRIVATE
    HAL_VERSION="${PROJECT_VERSION}"
    GIT_COMMIT_SHA="${GIT_COMMIT_SHA}"
)

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
target_include_directories(${LIBNAME} PRIVATE ${HDMI_CEC_INCLUDE_DIR})
target_include_directories(${LIBNAME} PRIVATE /usr/include/interface/vmcs_host/linux)
target_link_libraries(${LIBNAME} PRIVATE vchostif vchiq_arm vcos pthread)

add_custom_target(clean-all
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/clean.cmake
)

install(TARGETS ${LIBNAME}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# Define package configuration variables
set(PACKAGE_NAME ${LIBNAME})
set(PACKAGE_VERSION ${PROJECT_VERSION})
set(PACKAGE_DESCRIPTION "RPI-4 HDMI CEC HAL Library")
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
