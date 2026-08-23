### Depth-camera backends.
###
### Each backend is a standalone shared object exporting exactly one symbol,
### score_depthcam_backend_v1, loaded with dlopen/RTLD_LOCAL by the score
### plug-in. None of this is linked into score: that is what lets the plug-in be
### statically built into the application while the SDKs ship as downloadable
### packages.
###
### The SDKs are git submodules under ../3rdparty, pinned to a tag, so a build is
### reproducible and needs no network at configure time.

set(SCORE_DEPTHCAM_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../3rdparty")

### Everything under here ends up inside a shared module, static archives
### included, so all of it has to be position-independent. score sets this for
### its whole build and the requirement went unnoticed; a standalone
### backends-only configuration fails at link time instead ("relocation
### R_X86_64_PC32 against symbol `_ZSt4cerr' can not be used when making a
### shared object").
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

### libfreenect and librealsense each declare a global `uninstall` target, and
### score already has one (3rdparty/sh4lt, 3rdparty/shmdata). CMake makes a
### duplicate custom target a hard error, and these are pinned upstream
### submodules we would rather not patch. Overriding the command here makes the
### duplicate a no-op; the override is directory-scoped, so it applies to the
### SDK subtrees added below and to nothing else in score.
function(add_custom_target)
  if("${ARGV0}" STREQUAL "uninstall" AND TARGET uninstall)
    return()
  endif()

  # librealsense's Windows 7 UVC driver target lists its DEPENDS as bare
  # filenames relative to the *source* drivers directory. ninja resolves them
  # against the build directory, finds nothing, and refuses to generate anything
  # at all: "'IntelRealSense_D400_series_win7.inf', needed by
  # 'realsense-driver', missing and no known rule to make it". The files
  # themselves are already copied by configure_file just above; only the DEPENDS
  # is wrong, and we ship no drivers, so drop it and keep the target so
  # librealsense's add_dependencies still resolves.
  if("${ARGV0}" STREQUAL "realsense-driver")
    _add_custom_target(realsense-driver)
    return()
  endif()

  _add_custom_target(${ARGV})
endfunction()

### Nothing in this tree is ever installed. The SDKs below all carry install()
### and install(EXPORT) rules for their own packaging, and those rules are a
### steady source of configure-time failures that have nothing to do with us:
### librealsense exports realsense2, realsense2 links the `usb` target we hand
### it, and CMake then refuses the export set because the vendored libusb behind
### it is in no export set either -- a chain with no end that only exists to
### produce files no one asked for. Same override as above, same directory
### scope, so it reaches the SDK subtrees added below and nothing else.
function(install)
endfunction()

### Settings every backend needs, whatever SDK it wraps.
###
### The visibility flags are not cosmetic. libfreenect, libfreenect2, k4a and the
### OrbbecSDK each bundle or link libusb / libuvc / libjpeg / a logging library,
### and those dependencies annotate their own API with explicit default
### visibility, which overrides -fvisibility=hidden: measured on libOrbbecSDK.so,
### 99 libusb_* and 41 LZ4_* symbols stayed globally visible until
### --exclude-libs was added. Two backends in one process would then bind to
### whichever copy the loader saw first.
function(score_depthcam_setup_backend Target)
  # The authoritative list of what this configuration produces. `depthcam-package`
  # depends on it, and CI compares it against what the platform is meant to
  # ship: a backend silently skipped because a dependency was missing would
  # otherwise just come out as a smaller zip.
  set_property(GLOBAL APPEND PROPERTY SCORE_DEPTHCAM_BACKENDS ${Target})

  set_target_properties(${Target} PROPERTIES
    # Stated, not inherited. Inside score every target is C++20 already, so the
    # backends compiled as C++20 by accident; standalone they get whatever the
    # compiler defaults to. Apple clang's default is old enough that
    # `std::lock_guard lock{m}` and std::size are both errors, and every backend
    # fills in its ABI vtable with designated initializers, which MSVC accepts
    # only at C++20 -- gcc and clang take them as an extension at any standard,
    # which is exactly why nobody noticed.
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${SCORE_DEPTHCAM_BACKEND_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${SCORE_DEPTHCAM_BACKEND_DIR}"
  )

  target_include_directories(${Target} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../DepthCamera/Backend")

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(CheckLinkerFlag)

    check_linker_flag(CXX "LINKER:--exclude-libs,ALL" _has_exclude_libs)
    if(_has_exclude_libs)
      target_link_options(${Target} PRIVATE "LINKER:--exclude-libs,ALL")
    endif()

    # And state the one-symbol guarantee outright. --exclude-libs only covers
    # static archives; a version script also covers objects compiled straight
    # into the module, which matters because score dlopens everything in its
    # support/ directory with RTLD_GLOBAL.
    target_link_options(${Target} PRIVATE
      "LINKER:--version-script=${CMAKE_CURRENT_LIST_DIR}/backend.version")
    set_property(TARGET ${Target} APPEND PROPERTY
      LINK_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/backend.version")

    # Bind internal calls inside the library so a sibling backend can never
    # interpose them.
    check_linker_flag(CXX "LINKER:-Bsymbolic-functions" _has_bsymbolic)
    if(_has_bsymbolic)
      target_link_options(${Target} PRIVATE "LINKER:-Bsymbolic-functions")
    endif()

    # score links with -Wl,-zexecstack for its JIT plug-in, and everything built
    # in this tree inherits it. A hardened loader then refuses to dlopen the
    # module outright ("cannot enable executable stack as shared object
    # requires"). Nothing in a camera SDK needs an executable stack.
    check_linker_flag(CXX "LINKER:-z,noexecstack" _has_noexecstack)
    if(_has_noexecstack)
      target_link_options(${Target} PRIVATE "LINKER:-z,noexecstack")
    endif()

    # An explicit link flag rather than the RPATH target properties: score sets
    # CMAKE_SKIP_BUILD_RPATH, which silently drops BUILD_RPATH, so a backend in
    # the build tree ended up with no RPATH at all and dlopen could not find the
    # SDK sitting beside it ("libOrbbecSDK.so.2: cannot open shared object
    # file"). $ORIGIN is also exactly what a shipped package needs, so the build
    # tree and the package now resolve libraries the same way.
    target_link_options(${Target} PRIVATE "LINKER:-rpath,$ORIGIN")
    set_target_properties(${Target} PROPERTIES INSTALL_RPATH "$ORIGIN")
  elseif(APPLE)
    target_link_options(${Target} PRIVATE "LINKER:-rpath,@loader_path")
    set_target_properties(${Target} PROPERTIES INSTALL_RPATH "@loader_path")

    # CMake gives a MODULE library the .so suffix on macOS. BackendRegistry
    # globs score_depthcam_*.dylib there, and a plug-in loader that finds
    # nothing is indistinguishable from a package that failed to download.
    set_target_properties(${Target} PROPERTIES SUFFIX ".dylib")

    # The Mach-O counterpart of the version script below. Two-level namespaces
    # make macOS far less prone to the interposition this guards against, but
    # the guarantee is the same one the ABI documents, so state it on both.
    target_link_options(${Target} PRIVATE
      "LINKER:-exported_symbols_list,${CMAKE_CURRENT_LIST_DIR}/backend.symbols")
    set_property(TARGET ${Target} APPEND PROPERTY
      LINK_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/backend.symbols")
  endif()
endfunction()

### Copies a backend's udev rules next to it so the shipped package carries them.
### Every one of these SDKs talks to the camera over raw USB; without the rules
### the device nodes stay root-owned and the SDK reports no cameras, which is
### indistinguishable from "nothing is plugged in".
###
### configure_file rather than a POST_BUILD command: POST_BUILD only runs when
### the target relinks, so editing a .rules file otherwise leaves a stale copy in
### the output tree.
function(score_depthcam_install_udev_rules Dir)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
  endif()
  file(GLOB _rules "${Dir}/*.rules")
  foreach(r ${_rules})
    get_filename_component(_name "${r}" NAME)
    configure_file("${r}" "${SCORE_DEPTHCAM_BACKEND_DIR}/udev/${_name}" COPYONLY)
  endforeach()
endfunction()

### Several of these SDKs predate CMake 3.5 and declare a minimum that CMake 4
### refuses outright. This is the documented escape hatch.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE INTERNAL "")

### Never unity-build the vendored SDKs.
###
### They are pinned third-party trees that were not written for it, and a
### score build configured with -DCMAKE_UNITY_BUILD=1 otherwise drags them in:
### librealsense fails with ~28 redefinitions and ambiguous references once its
### translation units are concatenated (`redefinition of depth_sensor_snapshot`,
### `unexpected namespace name 'time'`), and libfreenect's C sources collide on
### a static/non-static `get_reply`.
###
### Our own backends are one source file each, so they lose nothing here; the
### score plug-in itself is defined in the parent directory and still honours
### whatever the build was configured with.
set(CMAKE_UNITY_BUILD OFF)

### --- libusb ----------------------------------------------------------------
###
### On Linux libusb is a system library. Every distribution ships it, it is what
### the udev rules in this tree are written against, and linking it is ordinary.
###
### Nowhere else. macOS has no libusb at all: linking Homebrew's leaves the
### backends referencing /opt/homebrew/opt/libusb/lib/libusb-1.0.0.dylib, an
### absolute path that exists only on the machine that built the package.
### Windows has none either, and shipping the DLL beside the backend does not
### help -- Windows resolves a module's dependencies through the standard search
### order, which does not include the directory the module itself came from, so
### a libusb-1.0.dll sitting right next to it is simply not found.
###
### So off Linux, link the copy the OrbbecSDK already vendors, statically. It is
### 1.0.26, it builds against winusb and darwin_usb, and it is already here.
### Each of the three consumers has to be told a different way.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET libusb_static)
  set(SCORE_DEPTHCAM_LIBUSB_INCLUDE
      "${SCORE_DEPTHCAM_SDK_DIR}/OrbbecSDK_v2/3rdparty/libusb/src/libusb"
      CACHE INTERNAL "")

  # libfreenect: its Findlibusb-1.0.cmake short-circuits when both of these are
  # already set ("in cache already").
  set(LIBUSB_1_INCLUDE_DIRS "${SCORE_DEPTHCAM_LIBUSB_INCLUDE}" CACHE PATH "" FORCE)
  set(LIBUSB_1_LIBRARIES libusb_static CACHE STRING "" FORCE)

  # libfreenect2: only reachable by shadowing its find module. See cmake/FindLibUSB.cmake.
  list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

  # And the same for libjpeg-turbo, which the SDK also vendors. libfreenect2
  # requires it on Windows -- only macOS has VideoToolbox to fall back on -- so
  # without this there is no Kinect v2 backend there at all.
  if(TARGET turbojpeg_static)
    set(SCORE_DEPTHCAM_TURBOJPEG_INCLUDE
        "${SCORE_DEPTHCAM_SDK_DIR}/OrbbecSDK_v2/3rdparty/libjpeg/src"
        CACHE INTERNAL "")
    set(SCORE_DEPTHCAM_VENDORED_TURBOJPEG ON)
  endif()

  # librealsense is handled in its own section below: the target it looks for
  # has to join an export set that only exists once it has been added.

  set(SCORE_DEPTHCAM_VENDORED_LIBUSB ON)
  message(STATUS "score-addon-orbbec: using the vendored static libusb")
endif()

### --- Orbbec (Femto, Gemini, Astra) ----------------------------------------
option(SCORE_DEPTHCAM_BUILD_ORBBEC "Build the Orbbec backend" ON)

if(SCORE_DEPTHCAM_BUILD_ORBBEC AND TARGET ob::OrbbecSDK)
  add_library(score_depthcam_orbbec MODULE Orbbec/OrbbecBackend.cpp)

  # One file per backend where possible: the SDK's CMake only ever builds
  # libOrbbecSDK as a shared library ("only support shared library for now"),
  # but that library is just its public C API compiled on top of seven static
  # component libs. Building those sources into the backend instead removes the
  # separate .so, so the package is one object plus the vendor blobs the SDK
  # dlopens at runtime, which cannot be merged by anyone.
  option(SCORE_DEPTHCAM_ORBBEC_STATIC
         "Build the OrbbecSDK into the backend instead of alongside it" ON)

  if(SCORE_DEPTHCAM_ORBBEC_STATIC)
    # An object library rather than more sources on the backend, because the two
    # halves cannot agree on a C++ standard. Ours uses designated initializers
    # to fill in the ABI vtable, which MSVC accepts only at C++20 (gcc and clang
    # take them as an extension, which is why this went unnoticed). The SDK's
    # were written for C++11, and at C++20 spdlog's format strings turn consteval
    # and its logging calls stop compiling against Apple's libc++. Compile each
    # at what it needs and link the objects together.
    file(GLOB _ob_api
      "${SCORE_DEPTHCAM_SDK_DIR}/OrbbecSDK_v2/src/impl/*.cpp"
      "${SCORE_DEPTHCAM_SDK_DIR}/OrbbecSDK_v2/src/context/*.cpp")

    add_library(score_depthcam_orbbec_sdk OBJECT ${_ob_api})
    set_target_properties(score_depthcam_orbbec_sdk PROPERTIES
      CXX_STANDARD 17
      CXX_STANDARD_REQUIRED ON
      CXX_VISIBILITY_PRESET hidden
      C_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN ON
      POSITION_INDEPENDENT_CODE ON)

    target_include_directories(score_depthcam_orbbec_sdk SYSTEM PRIVATE
      "${SCORE_DEPTHCAM_SDK_DIR}/OrbbecSDK_v2/src"
      "${SCORE_DEPTHCAM_SDK_DIR}/OrbbecSDK_v2/include")

    # These come from the SDK's add_definitions(), which is directory-scoped and
    # so does not reach a target defined here. They have to match what the SDK's
    # own cmake/<platform>_config.cmake would have set, because these are the
    # same sources the SDK compiles: getting OS_LINUX onto a macOS build is not
    # a warning, it selects a different ObPal.
    #
    # OB_STATIC_DEFINE is ours. The SDK's generated Export.h marks its public C
    # API __declspec(dllimport) for anyone who is not building the DLL, and
    # these translation units *define* those functions -- MSVC rejects that
    # outright ("definition of dllimport function not allowed"). We are not
    # consuming a DLL on any platform here, so say so everywhere.
    target_compile_definitions(score_depthcam_orbbec_sdk PRIVATE
      OB_STATIC_DEFINE BUILD_USB_PAL BUILD_NET_PAL
      $<$<CONFIG:Debug>:OB_DEBUG> $<$<NOT:$<CONFIG:Debug>>:OB_RELEASE>)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      # GMSL is the coaxial/serialiser transport on Jetson-class boards. The
      # SDK only ever compiles its sources under OB_BUILD_LINUX.
      target_compile_definitions(score_depthcam_orbbec_sdk PRIVATE OS_LINUX BUILD_GMSL_PAL)
    elseif(APPLE)
      target_compile_definitions(score_depthcam_orbbec_sdk PRIVATE OS_MACOS)
      if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
        target_compile_definitions(score_depthcam_orbbec_sdk PRIVATE OS_MACOS_ARM64 __NEON__)
      else()
        target_compile_definitions(score_depthcam_orbbec_sdk PRIVATE OS_MACOS_X64)
      endif()
    elseif(WIN32)
      target_compile_definitions(score_depthcam_orbbec_sdk PRIVATE
        _UNICODE UNICODE _CRT_SECURE_NO_WARNINGS)
    endif()

    # Linked for their headers; the archives themselves go on the backend below.
    target_link_libraries(score_depthcam_orbbec_sdk PRIVATE
      ob::shared ob::core ob::filter ob::platform ob::device ob::pipeline ob::media)

    target_sources(score_depthcam_orbbec
      PRIVATE $<TARGET_OBJECTS:score_depthcam_orbbec_sdk>)
    target_compile_definitions(score_depthcam_orbbec PRIVATE OB_STATIC_DEFINE)
    target_include_directories(score_depthcam_orbbec SYSTEM PRIVATE
      "${SCORE_DEPTHCAM_SDK_DIR}/OrbbecSDK_v2/src"
      "${SCORE_DEPTHCAM_SDK_DIR}/OrbbecSDK_v2/include")
    target_link_libraries(score_depthcam_orbbec PRIVATE
      ob::shared ob::core ob::filter ob::platform ob::device ob::pipeline ob::media)
  else()
    target_link_libraries(score_depthcam_orbbec PRIVATE ob::OrbbecSDK)
  endif()
  score_depthcam_setup_backend(score_depthcam_orbbec)

  # The OrbbecSDK's own rules are well-formed, unlike Microsoft's; ship those.
  score_depthcam_install_udev_rules(
    "${SCORE_DEPTHCAM_SDK_DIR}/OrbbecSDK_v2/scripts/env_setup")

  # The blobs under extensions/ -- filters, frameprocessor, firmwareupdater --
  # are dlopen'd by path at runtime and cannot be linked in by anyone, so they
  # have to sit beside the backend, in the build tree exactly as in a shipped
  # package. That is what makes the backend directory *be* the package layout:
  # anything that works here works installed.
  add_custom_command(TARGET score_depthcam_orbbec POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${SCORE_ORBBEC_RUNTIME_DIR}/extensions" "${SCORE_DEPTHCAM_BACKEND_DIR}/extensions"
    VERBATIM)

  # libOrbbecSDK itself only when it was not built into the backend. 72MB of
  # shared library that nothing loads is not something to ship.
  if(NOT SCORE_DEPTHCAM_ORBBEC_STATIC)
    add_custom_command(TARGET score_depthcam_orbbec POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_FILE:OrbbecSDK> "${SCORE_DEPTHCAM_BACKEND_DIR}/"
      COMMAND ${CMAKE_COMMAND} -E create_symlink
              $<TARGET_FILE_NAME:OrbbecSDK> "${SCORE_DEPTHCAM_BACKEND_DIR}/$<TARGET_SONAME_FILE_NAME:OrbbecSDK>"
      VERBATIM)
  endif()

  if(TARGET extensions)
    add_dependencies(score_depthcam_orbbec extensions)
  endif()
endif()

### --- Kinect v2 (libfreenect2) ---------------------------------------------
option(SCORE_DEPTHCAM_BUILD_FREENECT2 "Build the Kinect v2 backend" ON)

if(SCORE_DEPTHCAM_BUILD_FREENECT2
   AND EXISTS "${SCORE_DEPTHCAM_SDK_DIR}/libfreenect2/CMakeLists.txt")
  # OpenGL stays off: OpenGLPacketPipeline creates its own GLFW context and
  # window, which has no business inside a Qt RHI process. OpenCL and CUDA are
  # worth having -- the CPU depth pipeline is by far the slowest part of a
  # Kinect v2 -- and the backend picks the best available at runtime.
  set(BUILD_EXAMPLES OFF CACHE INTERNAL "")
  set(BUILD_OPENNI2_DRIVER OFF CACHE INTERNAL "")
  set(ENABLE_OPENGL OFF CACHE INTERNAL "")
  set(ENABLE_VAAPI OFF CACHE INTERNAL "")
  set(ENABLE_TEGRAJPEG OFF CACHE INTERNAL "")
  set(ENABLE_CXX11 ON CACHE INTERNAL "")

  option(SCORE_DEPTHCAM_FREENECT2_OPENCL "Kinect v2 OpenCL depth pipeline" ON)
  option(SCORE_DEPTHCAM_FREENECT2_CUDA "Kinect v2 CUDA depth pipeline" OFF)

  set(ENABLE_OPENCL ${SCORE_DEPTHCAM_FREENECT2_OPENCL} CACHE INTERNAL "")

  # libfreenect2's CUDA sources include helper_math.h, which ships with the CUDA
  # *samples* rather than the toolkit. Off unless someone points at a copy.
  set(ENABLE_CUDA ${SCORE_DEPTHCAM_FREENECT2_CUDA} CACHE INTERNAL "")
  if(SCORE_DEPTHCAM_FREENECT2_CUDA AND SCORE_DEPTHCAM_CUDA_SAMPLES_DIR)
    include_directories("${SCORE_DEPTHCAM_CUDA_SAMPLES_DIR}")
  endif()

  add_subdirectory("${SCORE_DEPTHCAM_SDK_DIR}/libfreenect2" libfreenect2 EXCLUDE_FROM_ALL)

  # See compat/freenect2_cl_compat.h: modern CL headers turned CL_ICDL_VERSION
  # into a macro, which breaks a local declaration of the same name in
  # libfreenect2's OpenCL processors.
  if(TARGET freenect2 AND SCORE_DEPTHCAM_FREENECT2_OPENCL)
    if(MSVC)
      # cl.exe spells -include as /FI, and it needs the shim just as much:
      # without it both OpenCL processors fail with "error C2059: syntax error:
      # 'constant'" on the same line.
      target_compile_options(freenect2 PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:SHELL:/FI${CMAKE_CURRENT_LIST_DIR}/Freenect2/compat/freenect2_cl_compat.h>")
    else()
      target_compile_options(freenect2 PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/Freenect2/compat/freenect2_cl_compat.h>")
    endif()
  endif()

  if(TARGET freenect2)
    add_library(score_depthcam_freenect2 MODULE Freenect2/Freenect2Backend.cpp)
    target_link_libraries(score_depthcam_freenect2 PRIVATE freenect2)

    # libfreenect2 predates usage requirements: its target carries no INTERFACE
    # include directories. The build dir is needed too -- config.h and export.h
    # are generated there.
    target_include_directories(score_depthcam_freenect2 SYSTEM PRIVATE
      "${SCORE_DEPTHCAM_SDK_DIR}/libfreenect2/include"
      "${CMAKE_CURRENT_BINARY_DIR}/libfreenect2"
    )
    score_depthcam_setup_backend(score_depthcam_freenect2)
    score_depthcam_install_udev_rules("${CMAKE_CURRENT_LIST_DIR}/Freenect2/udev")
  endif()
endif()

### --- Kinect v1 (libfreenect) ----------------------------------------------
option(SCORE_DEPTHCAM_BUILD_FREENECT "Build the Kinect v1 backend" ON)

if(SCORE_DEPTHCAM_BUILD_FREENECT
   AND EXISTS "${SCORE_DEPTHCAM_SDK_DIR}/libfreenect/CMakeLists.txt")
  # Only the core library: the examples want glut/OpenGL and the wrappers pull
  # in Python and OpenCV. BUILD_REDIST_PACKAGE avoids the audio-firmware
  # download, which needs Python at build time.
  set(BUILD_EXAMPLES OFF CACHE INTERNAL "")
  set(BUILD_FAKENECT OFF CACHE INTERNAL "")
  set(BUILD_C_SYNC OFF CACHE INTERNAL "")
  set(BUILD_CPP OFF CACHE INTERNAL "")
  set(BUILD_CV OFF CACHE INTERNAL "")
  set(BUILD_PYTHON OFF CACHE INTERNAL "")
  set(BUILD_OPENNI2_DRIVER OFF CACHE INTERNAL "")
  set(BUILD_AUDIO OFF CACHE INTERNAL "")
  set(BUILD_REDIST_PACKAGE ON CACHE INTERNAL "")

  add_subdirectory("${SCORE_DEPTHCAM_SDK_DIR}/libfreenect" libfreenect EXCLUDE_FROM_ALL)

  # libfreenect always defines both a shared `freenect` and a static
  # `freenectstatic`, and on Windows they collide: the import library of the DLL
  # and the static library are both freenect.lib, which ninja rejects outright
  # ("multiple rules generate lib/freenect.lib"). We link the static one.
  if(WIN32 AND TARGET freenectstatic)
    set_target_properties(freenectstatic PROPERTIES OUTPUT_NAME freenect_static)
  endif()

  set(_freenect_target "")
  if(TARGET freenectstatic)
    set(_freenect_target freenectstatic)
  elseif(TARGET freenect)
    set(_freenect_target freenect)
  endif()

  if(_freenect_target)
    add_library(score_depthcam_freenect MODULE Freenect/FreenectBackend.cpp)
    target_link_libraries(score_depthcam_freenect PRIVATE ${_freenect_target})
    target_include_directories(score_depthcam_freenect SYSTEM PRIVATE
      "${SCORE_DEPTHCAM_SDK_DIR}/libfreenect/include"
      "${CMAKE_CURRENT_BINARY_DIR}/libfreenect"
    )
    score_depthcam_setup_backend(score_depthcam_freenect)
    score_depthcam_install_udev_rules("${CMAKE_CURRENT_LIST_DIR}/Freenect/udev")
  endif()
endif()

### --- Azure Kinect DK (libk4a) ---------------------------------------------
###
### No link dependency on the SDK: the backend resolves libk4a with dlopen at
### runtime against vendored headers (see K4A/README.md). That keeps this target
### buildable in CI without the SDK, lets the user install it through their own
### package manager, and avoids redistributing libdepthengine, a closed
### Microsoft binary that any depth output requires.
### Off by default on macOS: Microsoft never released an Azure Kinect SDK for
### it, so the backend would build (it only needs the vendored headers) and then
### never find anything to dlopen.
if(APPLE)
  set(SCORE_DEPTHCAM_K4A_DEFAULT OFF)
else()
  set(SCORE_DEPTHCAM_K4A_DEFAULT ON)
endif()
option(SCORE_DEPTHCAM_BUILD_K4A "Build the Azure Kinect backend"
       ${SCORE_DEPTHCAM_K4A_DEFAULT})

if(SCORE_DEPTHCAM_BUILD_K4A)
  add_library(score_depthcam_k4a MODULE K4A/K4ABackend.cpp)
  target_include_directories(score_depthcam_k4a SYSTEM PRIVATE "${CMAKE_CURRENT_LIST_DIR}/K4A")
  score_depthcam_setup_backend(score_depthcam_k4a)
  score_depthcam_install_udev_rules("${CMAKE_CURRENT_LIST_DIR}/K4A/udev")
  if(UNIX AND NOT APPLE)
    target_link_libraries(score_depthcam_k4a PRIVATE ${CMAKE_DL_LIBS})
  endif()
endif()

### --- Intel RealSense (librealsense) ---------------------------------------
option(SCORE_DEPTHCAM_BUILD_REALSENSE "Build the RealSense backend" ON)

if(SCORE_DEPTHCAM_BUILD_REALSENSE
   AND EXISTS "${SCORE_DEPTHCAM_SDK_DIR}/librealsense/CMakeLists.txt")
  set(BUILD_EXAMPLES OFF CACHE INTERNAL "")
  set(BUILD_GRAPHICAL_EXAMPLES OFF CACHE INTERNAL "")
  set(BUILD_UNIT_TESTS OFF CACHE INTERNAL "")
  set(BUILD_TOOLS OFF CACHE INTERNAL "")
  set(BUILD_PYTHON_BINDINGS OFF CACHE INTERNAL "")
  set(BUILD_WITH_TM2 OFF CACHE INTERNAL "")
  set(BUILD_NETWORK_DEVICE OFF CACHE INTERNAL "")
  set(IMPORT_DEPTH_CAM_FW OFF CACHE INTERNAL "")
  set(CHECK_FOR_UPDATES OFF CACHE INTERNAL "")
  # The libusb backend rather than V4L2: V4L2 needs patched kernel modules for
  # full functionality, and the udev rules we ship grant raw USB access anyway.
  set(FORCE_RSUSB_BACKEND ON CACHE INTERNAL "")

  # librealsense's install_config.cmake runs configure_package_config_file,
  # which rejects a relative CMAKE_INSTALL_PREFIX outright ("INSTALL_PREFIX
  # must be an absolute path"). CMake accepts a relative one, and score's
  # sanitizer build tree uses "install", so a perfectly ordinary configuration
  # otherwise fails here. Nothing in this tree is ever installed; the value only
  # has to be well-formed.
  # librealsense: CMake/libusb_config.cmake does nothing at all if a target
  # called `usb` already exists, which is the cleanest of the three ways in.
  if(SCORE_DEPTHCAM_VENDORED_LIBUSB AND NOT TARGET usb)
    add_library(usb INTERFACE)
    target_link_libraries(usb INTERFACE libusb_static)
  endif()

  block()
    if(NOT IS_ABSOLUTE "${CMAKE_INSTALL_PREFIX}")
      set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_PREFIX}")
    endif()
    add_subdirectory("${SCORE_DEPTHCAM_SDK_DIR}/librealsense" librealsense EXCLUDE_FROM_ALL)
  endblock()

  # librealsense bundles an old copy of rosbag2 whose headers use uint8_t/
  # uint64_t without including <cstdint>. That compiled while libstdc++ still
  # pulled it in transitively; it does not any more. Force it in rather than
  # patching a pinned submodule.
  # librealsense's vendored zstd calls qsort_r, which glibc only declares under
  # _GNU_SOURCE. score defines that for its whole build, so this only surfaces
  # in a standalone backends-only configuration -- which is exactly what CI
  # packages.
  if(TARGET realsense-file AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(realsense-file PRIVATE _GNU_SOURCE)
  endif()

  foreach(_rs_target realsense-file realsense2)
    if(TARGET ${_rs_target} AND NOT MSVC)
      # C++ only: realsense-file mixes C and C++ sources, and <cstdint> does
      # not exist for the C ones.
      target_compile_options(${_rs_target} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstdint>")
    endif()
  endforeach()

  if(TARGET realsense2)
    add_library(score_depthcam_realsense MODULE RealSense/RealSenseBackend.cpp)
    target_link_libraries(score_depthcam_realsense PRIVATE realsense2)
    target_include_directories(score_depthcam_realsense SYSTEM PRIVATE
      "${SCORE_DEPTHCAM_SDK_DIR}/librealsense/include")
    score_depthcam_setup_backend(score_depthcam_realsense)
    score_depthcam_install_udev_rules("${CMAKE_CURRENT_LIST_DIR}/RealSense/udev")
  endif()
endif()

unset(CMAKE_POLICY_VERSION_MINIMUM CACHE)

### One target that builds whatever backends this configuration enabled, so
### neither CI nor a developer has to keep a list of target names in step with
### this file.
get_property(_backends GLOBAL PROPERTY SCORE_DEPTHCAM_BACKENDS)
add_custom_target(depthcam-backends)
if(_backends)
  add_dependencies(depthcam-backends ${_backends})
  message(STATUS "score-addon-orbbec: backends -> ${_backends}")
else()
  message(WARNING "score-addon-orbbec: no camera backend will be built")
endif()
