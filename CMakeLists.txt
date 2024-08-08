# Minimum CMake version required
cmake_minimum_required(VERSION 3.10)

project(RCECHal VERSION 1.0 LANGUAGES C)

set(LIBNAME RCECHal)
set(SRCS hdmi_cec_driver.c)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror -Wall -Wextra -fPIC")

include(GNUInstallDirs)

add_library(${LIBNAME} SHARED ${SRCS})

add_custom_target(clean-all
    COMMAND ${CMAKE_COMMAND} -P clean.cmake
)

install(TARGETS ${LIBNAME}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# Create a symbolic link for the .so file
install(CODE "execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink lib${LIBNAME}.so.${PROJECT_VERSION} lib${LIBNAME}.so)")

# Install the symbolic link in the -dev package
install(FILES ${CMAKE_BINARY_DIR}/lib${LIBNAME}.so
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
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
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/pkgconfig
)
