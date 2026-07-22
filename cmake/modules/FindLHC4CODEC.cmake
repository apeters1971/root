# Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.
# All rights reserved.
#
# For the licensing terms see $ROOTSYS/LICENSE.
# For the list of contributors see $ROOTSYS/README/CREDITS.

#.rst:
# FindLHC4CODEC
# -------------
#
# Find the lhc4codec library header and define variables.
#
# Imported Targets
# ^^^^^^^^^^^^^^^^
#
# This module defines :prop_tgt:`IMPORTED` target ``LHC4CODEC::LHC4CODEC``,
# if lhc4codec has been found.
#
# Result Variables
# ^^^^^^^^^^^^^^^^
#
# This module defines the following variables:
#
# ::
#
#   LHC4CODEC_FOUND          - True if lhc4codec is found.
#   LHC4CODEC_INCLUDE_DIRS   - Where to find lhc4codec/lhc4codec_c.h
#   LHC4CODEC_LIBRARIES      - The lhc4codec library path
#   LHC4CODEC_VERSION        - The lhc4codec version string, if available

find_path(LHC4CODEC_INCLUDE_DIR lhc4codec/lhc4codec_c.h)

find_library(LHC4CODEC_LIBRARY
  NAMES lhc4codec lhc4codec_static
  PATH_SUFFIXES lib lib64
)

if(LHC4CODEC_INCLUDE_DIR AND EXISTS "${LHC4CODEC_INCLUDE_DIR}/lhc4codec/lhc4codec_c.h")
  file(READ "${LHC4CODEC_INCLUDE_DIR}/lhc4codec/lhc4codec_c.h" _lhc4codec_header)
  string(REGEX MATCH "#define LHC4CODEC_VERSION \"([^\"]+)\"" _lhc4codec_version_match "${_lhc4codec_header}")
  if(CMAKE_MATCH_1)
    set(LHC4CODEC_VERSION "${CMAKE_MATCH_1}")
  endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LHC4CODEC
  REQUIRED_VARS LHC4CODEC_LIBRARY LHC4CODEC_INCLUDE_DIR
  VERSION_VAR LHC4CODEC_VERSION
)

if(LHC4CODEC_FOUND)
  set(LHC4CODEC_INCLUDE_DIRS "${LHC4CODEC_INCLUDE_DIR}")
  set(LHC4CODEC_LIBRARIES "${LHC4CODEC_LIBRARY}")

  if(NOT TARGET LHC4CODEC::LHC4CODEC)
    add_library(LHC4CODEC::LHC4CODEC UNKNOWN IMPORTED)
    set_target_properties(LHC4CODEC::LHC4CODEC PROPERTIES
      IMPORTED_LOCATION "${LHC4CODEC_LIBRARIES}"
      INTERFACE_INCLUDE_DIRECTORIES "${LHC4CODEC_INCLUDE_DIRS}")
  endif()
endif()
