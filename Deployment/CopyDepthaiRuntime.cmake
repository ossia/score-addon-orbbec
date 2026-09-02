### Copies depthai-core's installed shared libraries next to the backend.
###
### Globbed rather than named: what depthai installs beside libdepthai-core
### depends on how it was configured, and naming them would silently ship a
### package missing one. Run from POST_BUILD, since the install tree does not
### exist at configure time.
###
###   cmake -DIN=<depthai install lib dirs> -DOUT=<package dir> -P CopyDepthaiRuntime.cmake

if(NOT IN OR NOT OUT)
  message(FATAL_ERROR "CopyDepthaiRuntime: IN and OUT are both required")
endif()

### IN is a list: on Windows depthai installs RUNTIME to bin/ and LIBRARY to
### lib/, so both are swept and a directory absent on this platform is skipped.
set(_libs "")
foreach(dir ${IN})
  if(NOT IS_DIRECTORY "${dir}")
    continue()
  endif()
  file(GLOB _found
    "${dir}/*.so" "${dir}/*.so.*"
    "${dir}/*.dylib"
    "${dir}/*.dll")
  list(APPEND _libs ${_found})
endforeach()

if(NOT _libs)
  message(FATAL_ERROR
    "CopyDepthaiRuntime: no shared library found in ${IN}. The depthai build "
    "produced nothing to ship, which would make the backend unloadable.")
endif()

file(MAKE_DIRECTORY "${OUT}")
foreach(lib ${_libs})
  get_filename_component(_name "${lib}" NAME)
  # Symlinks are copied as symlinks so a versioned soname chain survives.
  file(COPY "${lib}" DESTINATION "${OUT}")
  message(STATUS "depthai runtime: ${_name}")
endforeach()
