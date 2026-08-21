### Orbbec / depth camera support for ossia score.
###
### The OrbbecSDK is vendored as a submodule and built here rather than consumed
### prebuilt on purpose: building it ourselves is what lets us keep its bundled
### libusb / libuvc / libjpeg / spdlog copies hidden (the SDK's own
### cmake/linux_config.cmake sets -fvisibility=hidden and -Wl,-Bsymbolic), so
### they cannot be interposed by the other depth-camera SDKs we intend to load
### into the same process later on.
cmake_minimum_required(VERSION 3.25 FATAL_ERROR)

if(NOT (WIN32 OR APPLE OR LINUX))
  return()
endif()

# The SDK is a submodule; a checkout without --recursive should skip the addon
# rather than fail the whole score configure step.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/OrbbecSDK_v2/CMakeLists.txt")
  message(STATUS "score-addon-orbbec: 3rdparty/OrbbecSDK_v2 submodule not checked out, skipping")
  return()
endif()

if(NOT TARGET score_lib_base)
  include("${SCORE_SOURCE_DIR}/cmake/ScoreExternalAddon.cmake")
endif()

project(score_addon_orbbec LANGUAGES C CXX)

# Where the SDK's extensions/ tree lands in this build. The addon needs to hand
# this to ob::Context::setExtensionsDirectory() before any context is created,
# because the SDK resolves it against the process working directory otherwise
# (see EnvConfig::getExtensionsDirectory, which caches on first use).
set(SCORE_ORBBEC_RUNTIME_DIR "${CMAKE_BINARY_DIR}/orbbec" CACHE INTERNAL "")

### --- Vendored OrbbecSDK -----------------------------------------------------
if(NOT TARGET ob::OrbbecSDK)
  # These are options() in the SDK, so they must be cache entries to take effect.
  set(OB_BUILD_EXAMPLES OFF CACHE INTERNAL "")
  set(OB_BUILD_TESTS    OFF CACHE INTERNAL "")
  set(OB_BUILD_DOCS     OFF CACHE INTERNAL "")
  set(OB_BUILD_TOOLS    OFF CACHE INTERNAL "")
  set(OB_INSTALL_EXAMPLES_SOURCE OFF CACHE INTERNAL "")
  set(OB_BUILD_MAIN_PROJECT OFF CACHE INTERNAL "")

  block()
    # The SDK's own CMakeLists forces C++11 and a pile of -Wno-* at its directory
    # scope; SYSTEM keeps its headers from spamming our warning output.
    set(CMAKE_UNITY_BUILD 0)

    # NOTE: no -march=native here, ever. This tree is shipped as a binary package
    # and a -march=native build faults on any machine other than the builder's.

    # The SDK copies its runtime blobs (filters, frameprocessor, depthengine,
    # firmwareupdater) to OB_RESOURCES_OUTPUT_DIRECTORY, which it derives from
    # CMAKE_LIBRARY_OUTPUT_DIRECTORY. score does not necessarily set that, and an
    # empty value would make it copy to "/extensions", so pin it explicitly.
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${SCORE_ORBBEC_RUNTIME_DIR}")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${SCORE_ORBBEC_RUNTIME_DIR}")

    add_subdirectory(3rdparty/OrbbecSDK_v2 SYSTEM)
  endblock()

  # The SDK sets -fvisibility=hidden, but that does not cover dependencies whose
  # own headers annotate their API with an explicit default visibility. libusb
  # (DEFAULT_VISIBILITY) and lz4 (LZ4LIB_API) both do, so without this they end
  # up as 140 globally-visible symbols on libOrbbecSDK.so -- and the moment
  # libfreenect/libfreenect2 (which link the *system* libusb-1.0) are loaded in
  # the same process, ELF interposition silently binds both SDKs to whichever
  # libusb was loaded first. Force everything coming from a static archive to be
  # local to the library.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(CheckLinkerFlag)
    check_linker_flag(CXX "LINKER:--exclude-libs,ALL" OB_HAS_EXCLUDE_LIBS)
    if(OB_HAS_EXCLUDE_LIBS)
      target_link_options(OrbbecSDK PRIVATE "LINKER:--exclude-libs,ALL")
    endif()

    # libOrbbecSDK otherwise comes out with an RWE PT_GNU_STACK, inherited from
    # the surrounding build rather than from anything in the SDK (no assembly is
    # compiled here -- only jsimd_none.c). A hardened loader refuses to dlopen a
    # module that pulls in such a library, which breaks the backend.
    check_linker_flag(CXX "LINKER:-z,noexecstack" OB_HAS_NOEXECSTACK)
    if(OB_HAS_NOEXECSTACK)
      target_link_options(OrbbecSDK PRIVATE "LINKER:-z,noexecstack")
    endif()
  endif()
endif()

# Where backend libraries land in this build. At runtime the plug-in looks here
# via $SCORE_DEPTHCAM_BACKEND_PATH, in its own package directory, and next to
# its binary -- see BackendRegistry.
set(SCORE_DEPTHCAM_BACKEND_DIR "${CMAKE_BINARY_DIR}/depthcam-backends" CACHE INTERNAL "")

add_subdirectory(Backends)

# General initialization
score_common_setup()

# Creation of the library.
#
# Note what is *not* here: any camera SDK. This target is meant to be statically
# linked into ossia score, so the protocol, settings widget and device browser
# are always present, while the SDKs arrive as downloadable backend packages
# that are dlopen'd at runtime. See DepthCamera/Backend/depthcam_abi.h.
add_library(score_addon_orbbec
  DepthCamera/Backend/depthcam_abi.h
  DepthCamera/Backend/BackendRegistry.hpp
  DepthCamera/Backend/BackendRegistry.cpp
  DepthCamera/ApplicationPlugin.hpp
  DepthCamera/ApplicationPlugin.cpp
  DepthCamera/DepthCameraSettings.hpp
  DepthCamera/DepthCameraSettings.cpp
  DepthCamera/DepthCameraStream.hpp
  DepthCamera/DepthCameraStream.cpp
  DepthCamera/DepthCameraDevice.hpp
  DepthCamera/DepthCameraDevice.cpp
  DepthCamera/DepthCameraProtocol.hpp
  DepthCamera/DepthCameraProtocol.cpp

  score_addon_orbbec.hpp
  score_addon_orbbec.cpp
)

target_include_directories(score_addon_orbbec
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/DepthCamera/Backend")

target_compile_definitions(score_addon_orbbec
  PRIVATE
    SCORE_DEPTHCAM_BUILD_BACKEND_DIR="${SCORE_DEPTHCAM_BACKEND_DIR}"
)

# Link
target_link_libraries(score_addon_orbbec PUBLIC
    score_plugin_engine
    score_plugin_media
    score_plugin_gfx
    score_plugin_avnd
    score_plugin_threedim
)

set_target_properties(score_addon_orbbec PROPERTIES SCORE_CUSTOM_PCH 1)

# Target-specific options
setup_score_plugin(score_addon_orbbec)
