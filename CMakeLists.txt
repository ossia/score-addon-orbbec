cmake_minimum_required(VERSION 3.13 FATAL_ERROR)

return()
if(NOT (WIN32 OR APPLE OR LINUX))
  return()
endif()

if(NOT TARGET score_lib_base)
  include("${SCORE_SOURCE_DIR}/cmake/ScoreExternalAddon.cmake")
endif()

project(score_addon_orbbec LANGUAGES CXX)

set(OB_BUILD_EXAMPLES 0)
set(OB_BUILD_TESTS 0)
set(OB_BUILD_DOCS 0)
set(OB_BUILD_TOOLS 0)
set(OB_BUILD_EXAMPLES 0 CACHE "" INTERNAL)
set(OB_BUILD_TESTS 0 CACHE "" INTERNAL)
set(OB_BUILD_DOCS 0 CACHE "" INTERNAL)
set(OB_BUILD_TOOLS 0 CACHE "" INTERNAL)
block()
  set(BUILD_SHARED_LIBS 0)
  set(CMAKE_UNITY_BUILD 0)
  add_compile_options(-Ofast -march=native)
  add_subdirectory(3rdparty/OrbbecSDK_v2 SYSTEM)
endblock()
# General initialization
score_common_setup()

# Creation of the library
add_library(score_addon_orbbec
  Orbbec/ApplicationPlugin.hpp
  Orbbec/ApplicationPlugin.cpp
  Orbbec/OrbbecInputStream.hpp
  Orbbec/OrbbecInputStream.cpp
  Orbbec/OrbbecInputDevice.hpp
  Orbbec/OrbbecInputDevice.cpp
  Orbbec/OrbbecProtocol.hpp
  Orbbec/OrbbecProtocol.cpp

  score_addon_orbbec.hpp
  score_addon_orbbec.cpp
)

# Link
target_link_libraries(score_addon_orbbec PUBLIC
    score_plugin_engine
    score_plugin_media
    score_plugin_gfx
    ob::OrbbecSDK
)

set_target_properties(score_addon_orbbec PROPERTIES SCORE_CUSTOM_PCH 1)

# Target-specific options
setup_score_plugin(score_addon_orbbec)
