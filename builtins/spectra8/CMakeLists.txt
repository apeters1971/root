# Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.
# All rights reserved.
#
# For the licensing terms see $ROOTSYS/LICENSE.
# For the list of contributors see $ROOTSYS/README/CREDITS.

# Build spectra8 as part of ROOT. Upstream:
#   https://github.com/apeters1971/spectra8.git
#
# If SPECTRA8_SOURCE_DIR is set, that checkout is built via add_subdirectory
# (offline / development). Otherwise the source is fetched with ExternalProject
# (clad-style git clone; requires network).

include(ExternalProject)

# **PLEASE UPDATE ALSO THE FOLLOWING LINE WHEN UPDATING THE VERSION**
# 25 Oct 2026, https://github.com/apeters1971/spectra8.git main @ a067e3d (spectra8 2.0.0)
set(ROOT_SPECTRA8_VERSION 2.0.0)
set(ROOT_SPECTRA8_GIT_REPOSITORY "https://github.com/apeters1971/spectra8.git")
set(ROOT_SPECTRA8_GIT_TAG "a067e3d3eeeba42157aca29b089242ec7234d12f")

# Bzip3 stays off: it is an optional LGPL backend and not a ROOT dependency.
# Zstd and lzma are AUTO so a system library can satisfy Codec::Zstd / Codec::Lzma;
# ROOT already links those. CUDA and HIP stay off. Metal is on for Apple so the
# default Crystal decompress path can use the GPU.
if(APPLE)
  set(_spectra8_metal ON)
else()
  set(_spectra8_metal OFF)
endif()
set(_spectra8_build_args
  -DSPECTRA8_BUILD_CLI=OFF
  -DSPECTRA8_BUILD_TESTS=OFF
  -DSPECTRA8_METAL=${_spectra8_metal}
  -DSPECTRA8_HIP=OFF
  -DSPECTRA8_CUDA=OFF
  -DSPECTRA8_NATIVE=OFF
  -DSPECTRA8_LTO=OFF
  -DSPECTRA8_ZSTD=AUTO
  -DSPECTRA8_LZMA=AUTO
  -DSPECTRA8_BZIP3=OFF
  -DCMAKE_CXX_STANDARD=20
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
)

if(SPECTRA8_SOURCE_DIR)
  if(NOT EXISTS "${SPECTRA8_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "SPECTRA8_SOURCE_DIR=\"${SPECTRA8_SOURCE_DIR}\" does not contain a CMake project")
  endif()

  message(STATUS "Building builtin spectra8 from ${SPECTRA8_SOURCE_DIR}")

  set(SPECTRA8_BUILD_CLI OFF CACHE BOOL "" FORCE)
  set(SPECTRA8_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(SPECTRA8_METAL ${_spectra8_metal} CACHE BOOL "" FORCE)
  set(SPECTRA8_HIP OFF CACHE BOOL "" FORCE)
  set(SPECTRA8_NATIVE OFF CACHE BOOL "" FORCE)
  set(SPECTRA8_LTO OFF CACHE BOOL "" FORCE)
  set(SPECTRA8_CUDA OFF CACHE STRING "" FORCE)
  set(SPECTRA8_ZSTD AUTO CACHE STRING "" FORCE)
  set(SPECTRA8_LZMA AUTO CACHE STRING "" FORCE)
  set(SPECTRA8_BZIP3 OFF CACHE STRING "" FORCE)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)

  add_subdirectory(${SPECTRA8_SOURCE_DIR} ${CMAKE_BINARY_DIR}/builtins/spectra8-build EXCLUDE_FROM_ALL)

  set(SPECTRA8_INCLUDE_DIRS ${SPECTRA8_SOURCE_DIR}/include)
  set(SPECTRA8_LIBRARIES spectra8_static)

  if(NOT TARGET SPECTRA8::SPECTRA8)
    add_library(SPECTRA8::SPECTRA8 ALIAS spectra8_static)
  endif()
else()
  message(STATUS "Downloading and building spectra8 ${ROOT_SPECTRA8_VERSION} from "
                  "${ROOT_SPECTRA8_GIT_REPOSITORY} (${ROOT_SPECTRA8_GIT_TAG})")

  set(ROOT_SPECTRA8_PREFIX ${CMAKE_BINARY_DIR}/builtins/spectra8-prefix)
  set(ROOT_SPECTRA8_SOURCE_DIR ${ROOT_SPECTRA8_PREFIX}/src/BUILTIN_SPECTRA8)
  set(ROOT_SPECTRA8_BUILD_DIR ${ROOT_SPECTRA8_PREFIX}/src/BUILTIN_SPECTRA8-build)
  # Staged headers live outside the git tree so the path exists at configure time
  # (CMake validates INTERFACE_INCLUDE_DIRECTORIES on imported targets).
  set(ROOT_SPECTRA8_INCLUDE_DIR ${ROOT_SPECTRA8_PREFIX}/include)
  set(ROOT_SPECTRA8_LIBRARY
      ${ROOT_SPECTRA8_BUILD_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}spectra8${CMAKE_STATIC_LIBRARY_SUFFIX})
  file(MAKE_DIRECTORY ${ROOT_SPECTRA8_INCLUDE_DIR})

  if(MSVC)
    set(_spectra8_build_type Release)
    if(winrtdebug)
      set(_spectra8_build_type Debug)
    endif()
    if(NOT CMAKE_GENERATOR MATCHES Ninja)
      set(_spectra8_build_flags --config ${_spectra8_build_type})
    endif()
    set(_spectra8_extra_cmake_args
      -DCMAKE_CXX_FLAGS_RELWITHDEBINFO=${CMAKE_CXX_FLAGS_RELWITHDEBINFO}
      -DCMAKE_CXX_FLAGS_RELEASE=${CMAKE_CXX_FLAGS_RELEASE}
      -DCMAKE_CXX_FLAGS_DEBUG=${CMAKE_CXX_FLAGS_DEBUG})
  else()
    set(_spectra8_build_type Release)
    set(_spectra8_build_flags "")
    set(_spectra8_extra_cmake_args "")
  endif()

  ExternalProject_Add(
    BUILTIN_SPECTRA8
    GIT_REPOSITORY ${ROOT_SPECTRA8_GIT_REPOSITORY}
    GIT_TAG ${ROOT_SPECTRA8_GIT_TAG}
    GIT_SUBMODULES ""
    PREFIX ${ROOT_SPECTRA8_PREFIX}
    UPDATE_COMMAND ""
    CMAKE_ARGS -G ${CMAKE_GENERATOR}
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=${_spectra8_build_type}
      ${_spectra8_build_args}
      ${_spectra8_extra_cmake_args}
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> ${_spectra8_build_flags}
                  --target spectra8_static
    INSTALL_COMMAND ""
    LOG_DOWNLOAD 1 LOG_CONFIGURE 1 LOG_BUILD 1 LOG_OUTPUT_ON_FAILURE 1
    BUILD_BYPRODUCTS ${ROOT_SPECTRA8_LIBRARY}
    TIMEOUT 600
  )

  # Copy public headers after the git clone, before the nested configure/build.
  ExternalProject_Add_Step(BUILTIN_SPECTRA8 stage-headers
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${ROOT_SPECTRA8_INCLUDE_DIR}/spectra8
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            <SOURCE_DIR>/include/spectra8
            ${ROOT_SPECTRA8_INCLUDE_DIR}/spectra8
    DEPENDEES download
    DEPENDERS configure
  )

  add_library(SPECTRA8::SPECTRA8 IMPORTED STATIC GLOBAL)
  add_dependencies(SPECTRA8::SPECTRA8 BUILTIN_SPECTRA8)
  set_target_properties(SPECTRA8::SPECTRA8 PROPERTIES
    IMPORTED_LOCATION ${ROOT_SPECTRA8_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${ROOT_SPECTRA8_INCLUDE_DIR}
  )

  set(SPECTRA8_INCLUDE_DIRS ${ROOT_SPECTRA8_INCLUDE_DIR})
  set(SPECTRA8_LIBRARIES ${ROOT_SPECTRA8_LIBRARY})
endif()

set(SPECTRA8_FOUND TRUE)
set(SPECTRA8_VERSION ${ROOT_SPECTRA8_VERSION})

set(SPECTRA8_INCLUDE_DIRS ${SPECTRA8_INCLUDE_DIRS} PARENT_SCOPE)
set(SPECTRA8_LIBRARIES ${SPECTRA8_LIBRARIES} PARENT_SCOPE)
set(SPECTRA8_FOUND ${SPECTRA8_FOUND} PARENT_SCOPE)
set(SPECTRA8_VERSION ${SPECTRA8_VERSION} PARENT_SCOPE)
