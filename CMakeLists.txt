cmake_minimum_required(VERSION 3.13 FATAL_ERROR)

if(NOT TARGET score_lib_base)
  include("${SCORE_SOURCE_DIR}/cmake/ScoreExternalAddon.cmake")
endif()

project(score_addon_orbbec LANGUAGES CXX)

# General initialization
score_common_setup()

# Creation of the library
add_library(score_addon_orbbec
  Orbbec/Device.cpp
  Orbbec/Device.hpp
  Orbbec/ProtocolFactory.cpp
  Orbbec/ProtocolFactory.hpp
  Orbbec/ProtocolSettingsWidget.cpp
  Orbbec/ProtocolSettingsWidget.hpp
  Orbbec/SpecificSettings.hpp
  Orbbec/SpecificSettingsSerialization.cpp

  score_addon_orbbec.hpp
  score_addon_orbbec.cpp
)

# Link
target_link_libraries(score_addon_orbbec PUBLIC score_plugin_engine)

# Target-specific options
setup_score_plugin(score_addon_orbbec)
