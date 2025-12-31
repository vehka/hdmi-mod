cmake_minimum_required(VERSION 3.2)

project(hdmi-mod)

# packaging for the mod

set(MOD_OUTPUT_PATH "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}")
set(MOD_INSTALL_PATH "/home/we/dust/code/${CMAKE_PROJECT_NAME}")

set(CMAKE_BUILD_RPATH "${MOD_INSTALL_PATH}/lib")
set(CMAKE_INSTALL_RPATH "${MOD_INSTALL_PATH}/lib")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_SKIP_BUILD_RPATH FALSE)

# Build the hdmi_mod C++ extension
include(FindPkgConfig)
pkg_check_modules(LIBLUA REQUIRED lua-5.3)

set(NORNS_SOURCE_PATH "${CMAKE_SOURCE_DIR}/dep/norns")

add_library(lua-extension MODULE src/hdmi_mod.cpp)
set_target_properties(lua-extension PROPERTIES
  PREFIX ""
  OUTPUT_NAME "hdmi_mod"
  CXX_STANDARD 11
)

target_include_directories(lua-extension PUBLIC
  "${NORNS_SOURCE_PATH}/matron/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "/usr/include/cairo"
  ${LIBLUA_INCLUDE_DIRS}
)

target_compile_options(lua-extension PRIVATE -O3)

add_custom_target(copy-mod ALL COMMAND
    ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/mod-hdmi
        ${MOD_OUTPUT_PATH}
)

add_custom_target(copy-extension ALL COMMAND
    ${CMAKE_COMMAND} -E copy $<TARGET_FILE:lua-extension> "${MOD_OUTPUT_PATH}/lib/"
)
add_dependencies(copy-extension copy-mod lua-extension)

add_custom_target(install-zip COMMAND
    ${CMAKE_COMMAND} -E tar "cfv" "${MOD_OUTPUT_PATH}.zip" --format=zip
        "${MOD_OUTPUT_PATH}"
)
add_dependencies(install-zip copy-extension copy-mod)

add_custom_target(install-dust COMMAND
    ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}
        ${MOD_INSTALL_PATH}
)
add_dependencies(install-dust copy-extension copy-mod)
