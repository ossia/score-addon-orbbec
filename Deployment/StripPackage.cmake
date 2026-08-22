### Strips the shared objects of a staged depth-camera package, and drops the
### files a user must supply themselves.
###
### Run as a script: cmake -DDIR=... -DSTRIP=... -P StripPackage.cmake

if(NOT DIR OR NOT IS_DIRECTORY "${DIR}")
  message(FATAL_ERROR "StripPackage: DIR is not a directory: ${DIR}")
endif()

### Microsoft's Azure Kinect runtime is not ours to redistribute. The backend
### looks for it in its own package directory first, which is where a user who
### installed it by hand is told to put it, so a copy left over from a developer
### machine would silently ship in the release.
file(GLOB _k4a "${DIR}/libk4a.so*" "${DIR}/k4a.dll" "${DIR}/libk4a*.dylib")
foreach(f ${_k4a})
  message(STATUS "StripPackage: removing ${f} (not redistributable)")
  file(REMOVE "${f}")
endforeach()
file(REMOVE_RECURSE "${DIR}/libk4a1.4")

if(NOT STRIP OR NOT EXISTS "${STRIP}")
  message(STATUS "StripPackage: no strip tool, shipping as built")
  return()
endif()

file(GLOB_RECURSE _libs "${DIR}/*.so" "${DIR}/*.so.*" "${DIR}/*.dylib" "${DIR}/*.dll")
foreach(lib ${_libs})
  if(IS_SYMLINK "${lib}")
    continue()
  endif()
  file(SIZE "${lib}" _before)
  execute_process(COMMAND "${STRIP}" --strip-unneeded "${lib}" RESULT_VARIABLE _res
                  OUTPUT_QUIET ERROR_QUIET)
  if(_res EQUAL 0)
    file(SIZE "${lib}" _after)
    math(EXPR _before_mb "${_before} / 1048576")
    math(EXPR _after_mb "${_after} / 1048576")
    message(STATUS "StripPackage: ${lib}: ${_before_mb}M -> ${_after_mb}M")
  endif()
endforeach()
