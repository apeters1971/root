############################################################################
# CMakeLists.txt file for building ROOT core/spectra8 package
############################################################################

add_library(ROOTSpectra8Zip OBJECT src/ZipSpectra8.cxx)
set_target_properties(ROOTSpectra8Zip PROPERTIES
  CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON)
target_link_libraries(ROOTSpectra8Zip PRIVATE SPECTRA8::SPECTRA8)
target_include_directories(ROOTSpectra8Zip
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
)
# Compile with -DR__Spectra8_FAILURE_DUMP (or uncomment below) to write failed
# zip/unzip frames under /tmp/root-spectra8-failure-<pid>/.
# target_compile_definitions(ROOTSpectra8Zip PRIVATE R__Spectra8_FAILURE_DUMP)

target_sources(Core PRIVATE $<TARGET_OBJECTS:ROOTSpectra8Zip>)

target_link_libraries(Core PRIVATE SPECTRA8::SPECTRA8)

# Bundled bzip3 is a separate static lib in the spectra8 build; propagate it when
# built via add_subdirectory (ExternalProject uses INTERFACE_LINK_LIBRARIES).
if(TARGET spectra8_bzip3_bundled)
  target_link_libraries(Core PRIVATE spectra8_bzip3_bundled)
endif()

target_include_directories(Core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>)

ROOT_INSTALL_HEADERS()
