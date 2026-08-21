/*
 * Stand-in for the k4a_export.h the Azure Kinect SDK generates at build time.
 *
 * The backend never links libk4a -- it resolves the API with dlopen at runtime
 * -- so these headers are used purely for their types and function signatures
 * and the visibility attributes are irrelevant.
 */
#ifndef K4A_EXPORT_H
#define K4A_EXPORT_H

#define K4A_EXPORT
#define K4A_NO_EXPORT
#define K4A_DEPRECATED_EXPORT
#define K4A_DEPRECATED_NO_EXPORT

#endif
