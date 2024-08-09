# Minimum CMake version required
cmake_minimum_required(VERSION 3.10)

# Version is matched with the version of hdmicecheader - 1.3.7.
project(RCECHal VERSION 1.3.7 LANGUAGES C)

set(LIBNAME RCECHal)
set(SRCS hdmi_cec_driver.c)
set(DEFAULT_BUILD_TYPE "Release")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	message(STATUS "Setting build type to '${DEFAULT_BUILD_TYPE}' as none was specified.")
	set(CMAKE_BUILD_TYPE "${DEFAULT_BUILD_TYPE}" CACHE STRING "Choose the type of build." FORCE)
	set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

include(GNUInstallDirs)

add_library(${LIBNAME} SHARED ${SRCS})

set_target_properties(${LIBNAME} PROPERTIES
	VERSION ${PROJECT_VERSION}
	SOVERSION ${PROJECT_VERSION}
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
