# Answers libfreenect2's FIND_PACKAGE(LibUSB REQUIRED) with the copy the
# OrbbecSDK vendors.
#
# libfreenect2's own module asks pkg-config first and returns whatever it says,
# so there is no variable to pre-seed: the only way in is to be found ahead of
# it. Backends/CMakeLists.txt puts this directory on CMAKE_MODULE_PATH first,
# and libfreenect2 appends rather than replaces.
#
# See the libusb section of ../CMakeLists.txt for why a system libusb is not
# good enough off Linux.

set(LibUSB_INCLUDE_DIRS "${SCORE_DEPTHCAM_LIBUSB_INCLUDE}")
set(LibUSB_LIBRARIES libusb_static)
set(LibUSB_FOUND TRUE)
