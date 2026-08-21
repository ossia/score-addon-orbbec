/*
 * Compatibility shim for building libfreenect2's OpenCL processors against
 * modern OpenCL headers.
 *
 * CL/cl_ext.h has come to define CL_ICDL_VERSION as an object-like macro
 * expanding to 2. libfreenect2 (2016) declares a local `const int
 * CL_ICDL_VERSION = 2;` of its own, which the macro turns into
 * `const int 2 = 2;` -- a hard parse error.
 *
 * Pulling the header in here and dropping the macro fixes it without patching a
 * pinned submodule: the include guard stops the definition coming back when the
 * source includes CL headers itself, and the only other use in that file passes
 * the value straight to clGetICDLoaderInfoOCLICD, where the local constant
 * carries exactly the 2 the macro would have supplied.
 *
 * The deprecation macros are the ones libfreenect2 sets itself; they have to be
 * in place before the first CL include, which is now this one.
 */
#ifndef SCORE_FREENECT2_CL_COMPAT_H
#define SCORE_FREENECT2_CL_COMPAT_H

#ifdef __cplusplus

#define CL_USE_DEPRECATED_OPENCL_1_2_APIS
#define CL_USE_DEPRECATED_OPENCL_2_0_APIS
#define CL_TARGET_OPENCL_VERSION 120

#if __has_include(<CL/cl_ext.h>)
#include <CL/cl.h>
#include <CL/cl_ext.h>
#undef CL_ICDL_VERSION
#endif

#endif
#endif
