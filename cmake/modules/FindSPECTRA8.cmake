# Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.
# All rights reserved.
#
# For the licensing terms see $ROOTSYS/LICENSE.
# For the list of contributors see $ROOTSYS/README/CREDITS.

#.rst:
# FindSPECTRA8
# -------------
#
# Find the spectra8 library header and define variables.
#
# Imported Targets
# ^^^^^^^^^^^^^^^^
#
# This module defines :prop_tgt:`IMPORTED` target ``SPECTRA8::SPECTRA8``,
# if spectra8 has been found.
#
# Result Variables
# ^^^^^^^^^^^^^^^^
#
# This module defines the following variables:
#
# ::
#
#   SPECTRA8_FOUND          - True if spectra8 is found.
#   SPECTRA8_INCLUDE_DIRS   - Where to find spectra8/spectra8_c.h
#   SPECTRA8_LIBRARIES      - The spectra8 library path
#   SPECTRA8_VERSION        - The spectra8 version string, if available
#   SPECTRA8_INSTALL_HELP   - How to install a missing package (always set)

# Prebuilt packages: https://gitlab.cern.ch/apeters/spectra8-bin
set(SPECTRA8_INSTALL_HELP [=[
spectra8 was not found (need spectra8/spectra8_c.h and libspectra8).

Install a prebuilt package from https://gitlab.cern.ch/apeters/spectra8-bin :

  # AlmaLinux 9 / 10 (x86_64)
  sudo curl -fsSL -o /etc/yum.repos.d/spectra8.repo \
    https://gitlab.cern.ch/apeters/spectra8-bin/-/raw/master/spectra8-el9.repo   # or -el10
  sudo dnf install spectra8 spectra8-devel

  # macOS, Apple silicon (macOS 13+)
  brew tap apeters/spectra8 https://gitlab.cern.ch/apeters/spectra8-bin.git
  brew install apeters/spectra8/spectra8

Or point CMake at an existing prefix:  -DSPECTRA8_ROOT=/path/to/prefix
Or let ROOT clone and build it:        -Dbuiltin_spectra8=ON
  (fetches https://github.com/apeters1971/spectra8.git)
]=])

set(_spectra8_hints)
if(SPECTRA8_ROOT)
  list(APPEND _spectra8_hints "${SPECTRA8_ROOT}")
endif()
if(DEFINED ENV{SPECTRA8_ROOT})
  list(APPEND _spectra8_hints "$ENV{SPECTRA8_ROOT}")
endif()

if(APPLE)
  # Homebrew: arm64 default is /opt/homebrew, Intel is /usr/local.
  list(APPEND _spectra8_hints /opt/homebrew /usr/local)
  find_program(_spectra8_brew brew)
  if(_spectra8_brew)
    execute_process(COMMAND "${_spectra8_brew}" --prefix spectra8
                    OUTPUT_VARIABLE _spectra8_brew_formula
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(_spectra8_brew_formula)
      list(APPEND _spectra8_hints "${_spectra8_brew_formula}")
    endif()
    execute_process(COMMAND "${_spectra8_brew}" --prefix
                    OUTPUT_VARIABLE _spectra8_brew_prefix
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(_spectra8_brew_prefix)
      list(APPEND _spectra8_hints "${_spectra8_brew_prefix}")
    endif()
  endif()
  unset(_spectra8_brew CACHE)
endif()

find_path(SPECTRA8_INCLUDE_DIR spectra8/spectra8_c.h
  HINTS ${_spectra8_hints}
  PATH_SUFFIXES include
)

find_library(SPECTRA8_LIBRARY
  NAMES spectra8 spectra8_static
  HINTS ${_spectra8_hints}
  PATH_SUFFIXES lib lib64
)

if(SPECTRA8_INCLUDE_DIR AND EXISTS "${SPECTRA8_INCLUDE_DIR}/spectra8/spectra8_c.h")
  file(READ "${SPECTRA8_INCLUDE_DIR}/spectra8/spectra8_c.h" _spectra8_header)
  string(REGEX MATCH "#define SPECTRA8_VERSION \"([^\"]+)\"" _spectra8_version_match "${_spectra8_header}")
  if(CMAKE_MATCH_1)
    set(SPECTRA8_VERSION "${CMAKE_MATCH_1}")
  endif()
  unset(_spectra8_header)
  unset(_spectra8_version_match)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SPECTRA8
  REQUIRED_VARS SPECTRA8_LIBRARY SPECTRA8_INCLUDE_DIR
  VERSION_VAR SPECTRA8_VERSION
)

if(SPECTRA8_FOUND)
  set(SPECTRA8_INCLUDE_DIRS "${SPECTRA8_INCLUDE_DIR}")
  set(SPECTRA8_LIBRARIES "${SPECTRA8_LIBRARY}")

  if(NOT TARGET SPECTRA8::SPECTRA8)
    add_library(SPECTRA8::SPECTRA8 UNKNOWN IMPORTED)
    set_target_properties(SPECTRA8::SPECTRA8 PROPERTIES
      IMPORTED_LOCATION "${SPECTRA8_LIBRARIES}"
      INTERFACE_INCLUDE_DIRECTORIES "${SPECTRA8_INCLUDE_DIRS}")
  endif()
endif()

unset(_spectra8_hints)
unset(_spectra8_brew_formula)
unset(_spectra8_brew_prefix)
