cmake_minimum_required(VERSION 4.2)

if(NOT DEFINED VCPKG_ROOT AND DEFINED ENV{VCPKG_ROOT})
  set(VCPKG_ROOT "$ENV{VCPKG_ROOT}")
endif()
if(NOT DEFINED VCPKG_ROOT OR VCPKG_ROOT STREQUAL "")
  message(FATAL_ERROR "VCPKG_ROOT is required")
endif()
set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")

project(RigVR VERSION 0.0.1 LANGUAGES CXX)

# Shares CommonLibF4VR and the FRIK/OpenVR headers with WeaponVise (repo root).
set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(COMMON_LIB_F4VR_PATH "${REPO_ROOT}/extern/CommonLibF4VR" CACHE PATH "CommonLibF4VR checkout")

set(F4SE_SUPPORT_XBYAK ON CACHE BOOL "" FORCE)
set(ENABLE_FALLOUT_NG OFF CACHE BOOL "" FORCE)
set(ENABLE_FALLOUT_F4 OFF CACHE BOOL "" FORCE)
set(ENABLE_FALLOUT_VR ON CACHE BOOL "" FORCE)
add_subdirectory("${COMMON_LIB_F4VR_PATH}" "CommonLibF4" EXCLUDE_FROM_ALL)

find_package(spdlog CONFIG REQUIRED)

if(DEFINED ENV{GITHUB_SHA})
  string(SUBSTRING "$ENV{GITHUB_SHA}" 0 7 RIGVR_BUILD)
else()
  set(RIGVR_BUILD "local")
endif()

add_library(RigVR SHARED
  src/Runtime.cpp
  src/Runtime.h
  src/RigVR.cpp
)
target_compile_features(RigVR PRIVATE cxx_std_23)
target_compile_definitions(RigVR PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  NOMMNOSOUND
  RIGVR_BUILD="${RIGVR_BUILD}"
)
target_include_directories(RigVR PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${REPO_ROOT}/src"
)
target_link_libraries(RigVR PRIVATE
  CommonLibF4::CommonLibF4
  spdlog::spdlog
)
target_compile_options(RigVR PRIVATE
  /sdl
  /utf-8
  /permissive-
  /Zc:__cplusplus
  /Zc:preprocessor
  /W4
  "$<$<CONFIG:RELEASE>:/Zc:inline;/JMC->"
)
target_link_options(RigVR PRIVATE
  "$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
)
set_target_properties(RigVR PROPERTIES PREFIX "")
