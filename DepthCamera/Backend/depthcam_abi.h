/*
 * Depth-camera backend ABI, version 1.
 *
 * One shared object per camera SDK, loaded with dlopen/RTLD_LOCAL and reached
 * only through this header. Nothing else crosses the boundary.
 *
 * The split exists for two reasons:
 *
 *  - Packaging. The score plug-in is meant to be statically linked into ossia
 *    score, so it must not have a link-time dependency on any camera SDK. The
 *    SDKs are downloaded as separate packages; a user with no depth camera pays
 *    nothing, and a user with one camera does not download the other three.
 *
 *  - Symbol isolation. libfreenect, libfreenect2, k4a and the OrbbecSDK each
 *    bundle or link their own libusb, libuvc, libjpeg and logging library. With
 *    default ELF visibility the first definition loaded wins for every caller,
 *    so two SDKs sharing a process silently bind to one another's copies. Each
 *    backend therefore hides everything it links (-fvisibility=hidden plus
 *    --exclude-libs,ALL) and exports exactly one symbol: the entry point below.
 *
 * Rules, all of which the host relies on:
 *  - Pure C. No C++ type, container or allocator crosses this boundary.
 *  - No exception may escape a callback. Backends catch everything.
 *  - Strings are UTF-8, owned by the backend, valid until the next call on the
 *    same handle (or, for enumerate(), until the callback returns).
 *  - Frame buffers are owned by the backend and handed back through the frame's
 *    own release hook, so nothing is copied on the way in.
 */
#ifndef SCORE_DEPTHCAM_ABI_H
#define SCORE_DEPTHCAM_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 1: streams, frames, alignment.
 * 2: controls (list_controls / get_control / set_control).
 * 3: DEPTHCAM_STREAM_IMU and depthcam_imu_sample.
 *
 * The host refuses a backend whose version does not match, so host and backends
 * are always built together.
 */
#define DEPTHCAM_ABI_VERSION 3

#if defined(_WIN32)
#define DEPTHCAM_EXPORT __declspec(dllexport)
#else
#define DEPTHCAM_EXPORT __attribute__((visibility("default")))
#endif

/** Which outputs a camera can produce, and which the host is asking for. */
enum depthcam_stream
{
  DEPTHCAM_STREAM_COLOR = 1u << 0,
  DEPTHCAM_STREAM_IR = 1u << 1,
  DEPTHCAM_STREAM_DEPTH = 1u << 2,
  DEPTHCAM_STREAM_POINTCLOUD = 1u << 3,
  /** Accelerometer and gyroscope samples; see depthcam_imu_sample. */
  DEPTHCAM_STREAM_IMU = 1u << 4,
};

/**
 * Pixel and buffer layouts.
 *
 * Deliberately not AVPixelFormat: libav's enum values are not ABI-stable across
 * major versions and pulling its headers into every backend would defeat the
 * isolation this file exists for. The host maps these to AVPixelFormat once.
 *
 * Compressed formats are allowed: the host already has a libav decode path, and
 * several cameras only offer MJPEG at their higher resolutions.
 */
enum depthcam_format
{
  DEPTHCAM_FMT_NONE = 0,

  /* packed colour */
  DEPTHCAM_FMT_RGB24,
  DEPTHCAM_FMT_BGR24,
  DEPTHCAM_FMT_RGBA,
  DEPTHCAM_FMT_BGRA,
  DEPTHCAM_FMT_RGB0, /* 32bpp, alpha byte undefined */
  DEPTHCAM_FMT_BGR0,

  /* single channel */
  DEPTHCAM_FMT_GRAY8,
  DEPTHCAM_FMT_GRAY16, /* little-endian */
  DEPTHCAM_FMT_GRAYF32,

  /* yuv */
  DEPTHCAM_FMT_YUYV422,
  DEPTHCAM_FMT_UYVY422,
  DEPTHCAM_FMT_NV12,
  DEPTHCAM_FMT_YUV420P,

  /* compressed; the host decodes */
  DEPTHCAM_FMT_MJPEG,
  DEPTHCAM_FMT_H264,
  DEPTHCAM_FMT_H265,

  /* Point clouds: interleaved float32, positions in METRES, right-handed,
   * +X right / +Y down / +Z forward from the camera.
   *
   * The unit has to be stated here because no two SDKs agree: Orbbec, k4a and
   * libfreenect all produce millimetres, libfreenect2 and librealsense produce
   * metres. A backend converts; the host never guesses. */
  DEPTHCAM_FMT_XYZ,    /* 3 floats per point */
  DEPTHCAM_FMT_XYZRGB, /* 6 floats per point, colour normalised to 0..1 */

  /* One depthcam_imu_sample. */
  DEPTHCAM_FMT_IMU,
};

/** Which fields of a depthcam_imu_sample carry a reading. */
enum depthcam_imu_field
{
  DEPTHCAM_IMU_ACCEL = 1u << 0,
  DEPTHCAM_IMU_GYRO = 1u << 1,
  DEPTHCAM_IMU_TEMPERATURE = 1u << 2,
};

/**
 * One inertial reading, the payload of a DEPTHCAM_STREAM_IMU frame.
 *
 * Units are SI, for the same reason point clouds are in metres: the host must
 * not have to know which camera it is talking to. m/s^2 and rad/s is also what
 * the OrbbecSDK (since 2.9), librealsense and libk4a all report natively, so
 * only libfreenect converts.
 *
 * Accelerometer and gyroscope are sampled independently by every one of these
 * cameras, and rarely at the same rate. A backend delivers whichever it just
 * received and says so in `fields`; it does not wait to pair them up, because
 * pairing costs latency on the faster of the two and the host has nothing to do
 * with a stale sample. `fields` is never zero.
 */
typedef struct depthcam_imu_sample
{
  uint32_t fields; /* bitmask of depthcam_imu_field */

  float accel[3];      /* metres per second squared */
  float gyro[3];       /* radians per second */
  float temperature_c; /* degrees Celsius */
} depthcam_imu_sample;

/** How depth and colour are brought into a common frame of reference. */
enum depthcam_align
{
  DEPTHCAM_ALIGN_NONE = 0,
  DEPTHCAM_ALIGN_DEPTH_TO_COLOR, /* cloud at colour resolution */
  DEPTHCAM_ALIGN_COLOR_TO_DEPTH, /* cloud at depth resolution */
};

/** What enumerate() reports. None of this requires opening the camera. */
typedef struct depthcam_device_info
{
  const char* backend;   /* "orbbec", "freenect2", ... */
  const char* uri;       /* opaque to the host; pass back to open() verbatim */
  const char* name;      /* "Femto Mega" */
  const char* serial;    /* may be empty */
  const char* transport; /* "usb3.2", "ethernet", ... ; may be empty */
  uint32_t streams;      /* bitmask of depthcam_stream */
} depthcam_device_info;

/**
 * One delivered frame.
 *
 * `data` stays valid until release(owner) is called. The host may hold it for
 * several render passes, so a backend must not recycle the buffer early.
 * A backend with no natural refcounting may allocate per frame and free in
 * release; a backend whose SDK already refcounts (Orbbec) should pin the SDK
 * frame instead and avoid the copy entirely.
 */
typedef struct depthcam_frame
{
  uint32_t stream; /* exactly one depthcam_stream bit */
  int32_t format;  /* depthcam_format */
  int32_t width;
  int32_t height;
  int32_t stride; /* bytes per row; 0 means tightly packed */
  uint64_t timestamp_ns;

  const void* data;
  size_t bytes;

  /* DEPTH only: multiply a sample by this to get millimetres. 0 if unknown. */
  float depth_unit_mm;

  /* POINTCLOUD only: number of points. 0 means derive it from bytes/format. */
  int32_t point_count;

  void* owner;
  void (*release)(void* owner);
} depthcam_frame;

/** What the host asks for when opening a camera. Zero means "backend decides". */
typedef struct depthcam_open_config
{
  uint32_t streams; /* bitmask of depthcam_stream */

  int32_t color_width, color_height, color_fps;
  int32_t depth_width, depth_height, depth_fps;

  /**
   * Infrared, which is not always the depth sensor's own resolution: an Orbbec
   * enumerates IR profiles separately, and a RealSense can stream 1280x720
   * infrared next to 848x480 depth. Zero means "backend decides", and a backend
   * whose IR is inseparable from depth is free to ignore this.
   */
  int32_t ir_width, ir_height, ir_fps;

  int32_t align;            /* depthcam_align */
  int32_t color_pointcloud; /* non-zero to tint the cloud from the colour sensor */
} depthcam_open_config;

/**
 * A camera setting: exposure, laser power, a tilt motor, a temperature readout.
 *
 * Modelled on what the SDKs can actually tell us. Orbbec
 * (getSupportedProperty + getIntPropertyRange) and librealsense
 * (get_supported_options + get_option_range) both enumerate their settings with
 * ranges at runtime, so nothing here is hard-coded for those two; k4a has a
 * fixed list but reports capabilities per entry; libfreenect and libfreenect2
 * have a handful each, described by hand.
 */
enum depthcam_control_kind
{
  DEPTHCAM_CONTROL_BOOL = 0,
  DEPTHCAM_CONTROL_INT,
  DEPTHCAM_CONTROL_FLOAT,
  /** An int whose values are named; see enum_labels. */
  DEPTHCAM_CONTROL_ENUM,
  /**
   * Write-only trigger with no value -- rebooting the device, forcing a
   * capture. Setting it to anything fires it. Kept distinct so the host can put
   * these somewhere a performer will not hit them by accident.
   */
  DEPTHCAM_CONTROL_ACTION,
};

enum depthcam_control_access
{
  DEPTHCAM_ACCESS_READ = 1 << 0,
  DEPTHCAM_ACCESS_WRITE = 1 << 1,
};

typedef struct depthcam_control
{
  /**
   * Stable identifier, and also the node path the host builds:
   * "color/exposure", "depth/laser_power", "sensors/temperature",
   * "advanced/reboot_device". The leading segment groups the control; the host
   * does not interpret it further.
   */
  const char* id;
  const char* name;        /**< human-readable */
  const char* description; /**< may be NULL */

  int32_t kind;   /**< depthcam_control_kind */
  int32_t access; /**< bitmask of depthcam_control_access */

  double min, max, step, def;

  /** ENUM only: enum_count labels, indexed from min in steps of step. */
  const char* const* enum_labels;
  int32_t enum_count;
} depthcam_control;

typedef struct depthcam_device depthcam_device;

typedef void (*depthcam_enumerate_cb)(const depthcam_device_info*, void* user);
typedef void (*depthcam_frame_cb)(const depthcam_frame*, void* user);
typedef void (*depthcam_changed_cb)(void* user);
typedef void (*depthcam_control_cb)(const depthcam_control*, void* user);

typedef struct depthcam_backend_v1
{
  uint32_t abi_version; /* must equal DEPTHCAM_ABI_VERSION */
  const char* name;         /* stable id, matches depthcam_device_info::backend */
  const char* display_name; /* shown in the device browser */

  /** Human-readable reason for the last failure, or NULL. */
  const char* (*last_error)(void);

  /**
   * @param resource_dir the backend's own package directory, for SDKs that load
   *        auxiliary blobs at runtime (the OrbbecSDK's extensions/ tree, k4a's
   *        depth engine). May be NULL.
   * @return non-zero on success.
   */
  int (*init)(const char* resource_dir);
  void (*shutdown)(void);

  /** Metadata only; must not open any camera. */
  int (*enumerate)(depthcam_enumerate_cb cb, void* user);

  /**
   * Hot-plug notification. Called from an arbitrary backend thread; the host
   * only re-runs enumerate() from its own thread in response. Pass NULL to
   * clear. Optional: a backend that cannot detect hot-plug leaves this NULL.
   */
  void (*set_changed_callback)(depthcam_changed_cb cb, void* user);

  depthcam_device* (*open)(const char* uri, const depthcam_open_config* config);
  void (*close)(depthcam_device*);

  /** on_frame is called from a backend thread, once per frame per stream. */
  int (*start)(depthcam_device*, depthcam_frame_cb on_frame, void* user);
  void (*stop)(depthcam_device*);

  /* --- controls, ABI 2 ------------------------------------------------- */

  /**
   * Enumerate the camera's settings. Called once after open; the descriptors
   * and their strings stay valid until close.
   *
   * All four may be NULL on a backend with nothing to offer.
   */
  int (*list_controls)(depthcam_device*, depthcam_control_cb, void* user);

  /**
   * Read one control. Values are always doubles, whatever the kind: bools are
   * 0/1 and enums are their integer value.
   *
   * No SDK here offers change notification for settings, so a host that wants
   * to track a read-only sensor has to call this periodically. It should only
   * do so for controls something is actually listening to.
   */
  int (*get_control)(depthcam_device*, const char* id, double* out);

  int (*set_control)(depthcam_device*, const char* id, double value);

  /* --- ABI 3 ------------------------------------------------------------ */

  /**
   * Which streams the camera actually gave us, as a bitmask of depthcam_stream.
   *
   * open() takes a wish, not an order: a Kinect v2 has no IMU, a D435 has one
   * where a D435i does not, and several Orbbec models have neither. Without
   * this the host would publish an imu/accel node on a camera that will never
   * fill it in, which is worse than not offering it.
   *
   * May be NULL, in which case the host assumes it got what it asked for.
   */
  uint32_t (*active_streams)(depthcam_device*);
} depthcam_backend_v1;

/**
 * The one and only exported symbol of a backend library.
 *
 * The name is versioned rather than the struct: a host and a backend built
 * against different ABI versions simply fail to find each other, instead of
 * agreeing on a layout that has changed underneath them.
 */
DEPTHCAM_EXPORT const depthcam_backend_v1* score_depthcam_backend_v1(void);

typedef const depthcam_backend_v1* (*score_depthcam_backend_v1_fn)(void);

#ifdef __cplusplus
}
#endif

#endif
