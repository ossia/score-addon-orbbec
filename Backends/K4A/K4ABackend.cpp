/*
 * Azure Kinect DK backend: libk4a behind depthcam_abi.h.
 *
 * Unlike the other backends this one does not link its SDK. libk4a exposes a
 * flat C API, so it is resolved with dlopen at runtime, which means:
 *   - this target builds anywhere, CI included, with no SDK present;
 *   - the user installs the SDK through their own package manager;
 *   - we never redistribute libdepthengine, a closed Microsoft binary that any
 *     depth output requires and that we have no licence to ship.
 * A missing SDK is reported as "no cameras", not as a load failure.
 *
 * k4a is also a *pull* API -- k4a_device_get_capture blocks -- so unlike Orbbec
 * and libfreenect2 this backend runs its own capture thread.
 */
#include <depthcam_abi.h>

#include <k4a/k4a.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{
std::string g_last_error;
std::mutex g_error_mutex;

void set_error(const std::string& e)
{
  std::lock_guard lock{g_error_mutex};
  g_last_error = e;
}

const char* backend_last_error()
{
  std::lock_guard lock{g_error_mutex};
  return g_last_error.empty() ? nullptr : g_last_error.c_str();
}

/// The subset of libk4a this backend uses, resolved at runtime.
struct K4AApi
{
  void* lib{};

  uint32_t (*device_get_installed_count)();
  k4a_result_t (*device_open)(uint32_t, k4a_device_t*);
  void (*device_close)(k4a_device_t);
  k4a_buffer_result_t (*device_get_serialnum)(k4a_device_t, char*, size_t*);
  k4a_result_t (*device_start_cameras)(k4a_device_t, const k4a_device_configuration_t*);
  void (*device_stop_cameras)(k4a_device_t);
  k4a_wait_result_t (*device_get_capture)(k4a_device_t, k4a_capture_t*, int32_t);
  k4a_result_t (*device_get_calibration)(
      k4a_device_t, k4a_depth_mode_t, k4a_color_resolution_t, k4a_calibration_t*);

  void (*capture_release)(k4a_capture_t);
  k4a_image_t (*capture_get_color_image)(k4a_capture_t);
  k4a_image_t (*capture_get_depth_image)(k4a_capture_t);
  k4a_image_t (*capture_get_ir_image)(k4a_capture_t);

  void (*image_release)(k4a_image_t);
  void (*image_reference)(k4a_image_t);
  uint8_t* (*image_get_buffer)(k4a_image_t);
  size_t (*image_get_size)(k4a_image_t);
  int (*image_get_width_pixels)(k4a_image_t);
  int (*image_get_height_pixels)(k4a_image_t);
  int (*image_get_stride_bytes)(k4a_image_t);
  k4a_image_format_t (*image_get_format)(k4a_image_t);
  uint64_t (*image_get_device_timestamp_usec)(k4a_image_t);
  k4a_result_t (*image_create)(k4a_image_format_t, int, int, int, k4a_image_t*);

  k4a_transformation_t (*transformation_create)(const k4a_calibration_t*);
  void (*transformation_destroy)(k4a_transformation_t);
  k4a_result_t (*transformation_depth_image_to_point_cloud)(
      k4a_transformation_t, const k4a_image_t, const k4a_calibration_type_t,
      k4a_image_t);
  k4a_result_t (*transformation_color_image_to_depth_camera)(
      k4a_transformation_t, const k4a_image_t, const k4a_image_t, k4a_image_t);
  k4a_result_t (*transformation_depth_image_to_color_camera)(
      k4a_transformation_t, const k4a_image_t, k4a_image_t);

  explicit operator bool() const noexcept { return lib != nullptr; }
};

K4AApi g_k4a;

void* load_symbol(void* lib, const char* name)
{
#if defined(_WIN32)
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
  return dlsym(lib, name);
#endif
}

bool load_k4a(K4AApi& api, const char* resource_dir)
{
  // SONAME first, then the unversioned development symlink.
#if defined(_WIN32)
  const char* names[] = {"k4a.dll"};
#elif defined(__APPLE__)
  // Microsoft has never shipped an Azure Kinect SDK for macOS; kept only so the
  // backend still compiles there.
  const char* names[] = {"libk4a.dylib"};
#else
  const char* names[] = {"libk4a.so.1.4", "libk4a.so.1", "libk4a.so"};
#endif

  std::vector<std::string> candidates;

  // The backend's own package directory first: a shipped package carries libk4a
  // and libdepthengine beside the backend, and must not depend on the user
  // having installed the SDK system-wide.
  if(resource_dir && *resource_dir)
    for(const char* n : names)
      candidates.push_back(std::string{resource_dir} + "/" + n);

  // Then the ordinary loader search path, for a system-installed SDK.
  for(const char* n : names)
    candidates.push_back(n);

  for(const auto& c : candidates)
  {
#if defined(_WIN32)
    api.lib = LoadLibraryA(c.c_str());
#else
    api.lib = dlopen(c.c_str(), RTLD_LAZY | RTLD_LOCAL);
#endif
    if(api.lib)
      break;
  }

  if(!api.lib)
  {
    set_error(
        "libk4a not found. Install the Azure Kinect Sensor SDK, or place libk4a "
        "and libdepthengine next to this backend. Without it no Azure Kinect "
        "can be listed.");
    return false;
  }

  bool ok = true;
  const auto sym = [&](auto& fn, const char* name) {
    auto* p = load_symbol(api.lib, name);
    if(!p)
    {
      set_error(std::string{"libk4a is missing "} + name);
      ok = false;
      return;
    }
    fn = reinterpret_cast<std::decay_t<decltype(fn)>>(p);
  };

  sym(api.device_get_installed_count, "k4a_device_get_installed_count");
  sym(api.device_open, "k4a_device_open");
  sym(api.device_close, "k4a_device_close");
  sym(api.device_get_serialnum, "k4a_device_get_serialnum");
  sym(api.device_start_cameras, "k4a_device_start_cameras");
  sym(api.device_stop_cameras, "k4a_device_stop_cameras");
  sym(api.device_get_capture, "k4a_device_get_capture");
  sym(api.device_get_calibration, "k4a_device_get_calibration");
  sym(api.capture_release, "k4a_capture_release");
  sym(api.capture_get_color_image, "k4a_capture_get_color_image");
  sym(api.capture_get_depth_image, "k4a_capture_get_depth_image");
  sym(api.capture_get_ir_image, "k4a_capture_get_ir_image");
  sym(api.image_release, "k4a_image_release");
  sym(api.image_reference, "k4a_image_reference");
  sym(api.image_get_buffer, "k4a_image_get_buffer");
  sym(api.image_get_size, "k4a_image_get_size");
  sym(api.image_get_width_pixels, "k4a_image_get_width_pixels");
  sym(api.image_get_height_pixels, "k4a_image_get_height_pixels");
  sym(api.image_get_stride_bytes, "k4a_image_get_stride_bytes");
  sym(api.image_get_format, "k4a_image_get_format");
  sym(api.image_get_device_timestamp_usec, "k4a_image_get_device_timestamp_usec");
  sym(api.image_create, "k4a_image_create");
  sym(api.transformation_create, "k4a_transformation_create");
  sym(api.transformation_destroy, "k4a_transformation_destroy");
  sym(api.transformation_depth_image_to_point_cloud,
      "k4a_transformation_depth_image_to_point_cloud");
  sym(api.transformation_color_image_to_depth_camera,
      "k4a_transformation_color_image_to_depth_camera");
  sym(api.transformation_depth_image_to_color_camera,
      "k4a_transformation_depth_image_to_color_camera");

  if(!ok)
  {
#if !defined(_WIN32)
    dlclose(api.lib);
#endif
    api.lib = nullptr;
  }
  return ok;
}

int to_depthcam_format(k4a_image_format_t f)
{
  switch(f)
  {
    case K4A_IMAGE_FORMAT_COLOR_MJPG:
      return DEPTHCAM_FMT_MJPEG;
    case K4A_IMAGE_FORMAT_COLOR_NV12:
      return DEPTHCAM_FMT_NV12;
    case K4A_IMAGE_FORMAT_COLOR_YUY2:
      return DEPTHCAM_FMT_YUYV422;
    case K4A_IMAGE_FORMAT_COLOR_BGRA32:
      return DEPTHCAM_FMT_BGRA;
    case K4A_IMAGE_FORMAT_DEPTH16:
    case K4A_IMAGE_FORMAT_IR16:
      return DEPTHCAM_FMT_GRAY16;
    default:
      return DEPTHCAM_FMT_NONE;
  }
}

struct Address
{
  enum Kind
  {
    Any,
    Serial,
    Index
  } kind{Any};
  std::string serial;
  uint32_t index{0};
};

Address parse_uri(const char* uri)
{
  Address a;
  if(!uri)
    return a;
  std::string s{uri};
  if(s.rfind("k4a:", 0) == 0)
    s = s.substr(4);
  if(s.empty())
    return a;

  if(s.rfind("sn:", 0) == 0)
  {
    a.kind = Address::Serial;
    a.serial = s.substr(3);
  }
  else if(s.rfind("index:", 0) == 0)
  {
    try
    {
      a.index = uint32_t(std::stoul(s.substr(6)));
      a.kind = Address::Index;
    }
    catch(...)
    {
    }
  }
  else
  {
    a.kind = Address::Serial;
    a.serial = s;
  }
  if(a.kind == Address::Serial && a.serial.empty())
    a.kind = Address::Any;
  return a;
}

std::string serial_of(k4a_device_t dev)
{
  size_t len = 0;
  if(g_k4a.device_get_serialnum(dev, nullptr, &len) != K4A_BUFFER_RESULT_TOO_SMALL)
    return {};
  std::string s(len, '\0');
  if(g_k4a.device_get_serialnum(dev, s.data(), &len) != K4A_BUFFER_RESULT_SUCCEEDED)
    return {};
  if(!s.empty() && s.back() == '\0')
    s.pop_back();
  return s;
}

struct EnumEntry
{
  std::string uri, serial;
};

} // namespace

// ---------------------------------------------------------------------------

struct depthcam_device
{
  k4a_device_t dev{};
  k4a_calibration_t calibration{};
  k4a_transformation_t transformation{};

  depthcam_open_config cfg{};
  bool color_pointcloud{};
  bool cloud_at_color{}; ///< DEPTH_TO_COLOR: cloud at the colour resolution

  std::atomic_bool running{};
  std::thread thread;

  depthcam_frame_cb on_frame{};
  void* user{};

  // Reused across frames; k4a images are refcounted so these are cheap to keep.
  k4a_image_t xyz_image{};
  k4a_image_t transformed_color{};
  k4a_image_t transformed_depth{};
  std::vector<float> cloud_buf;

  ~depthcam_device();

  void run();
  void handleCapture(k4a_capture_t capture);
  void emitImage(uint32_t stream, k4a_image_t img, float depth_unit);
  void emitPointCloud(k4a_image_t depth, k4a_image_t color);
};

namespace
{
/// Holds a reference on a k4a image for as long as the host holds its buffer.
struct ImageHolder
{
  k4a_image_t image{};
};

void release_image_holder(void* owner)
{
  auto* h = static_cast<ImageHolder*>(owner);
  if(h->image)
    g_k4a.image_release(h->image);
  delete h;
}

struct VectorHolder
{
  std::vector<float> data;
};

void release_vector_holder(void* owner)
{
  delete static_cast<VectorHolder*>(owner);
}
} // namespace

depthcam_device::~depthcam_device()
{
  if(transformation)
    g_k4a.transformation_destroy(transformation);
  for(auto* img : {&xyz_image, &transformed_color, &transformed_depth})
    if(*img)
      g_k4a.image_release(*img);
  if(dev)
    g_k4a.device_close(dev);
}

void depthcam_device::emitImage(uint32_t stream, k4a_image_t img, float depth_unit)
{
  if(!img || !on_frame)
    return;

  const int format = to_depthcam_format(g_k4a.image_get_format(img));
  if(format == DEPTHCAM_FMT_NONE)
    return;

  // Take our own reference; the capture is released as soon as we return.
  g_k4a.image_reference(img);
  auto* holder = new ImageHolder{img};

  depthcam_frame out{};
  out.stream = stream;
  out.format = format;
  out.width = g_k4a.image_get_width_pixels(img);
  out.height = g_k4a.image_get_height_pixels(img);
  out.stride = g_k4a.image_get_stride_bytes(img);
  out.timestamp_ns = g_k4a.image_get_device_timestamp_usec(img) * 1000ull;
  out.data = g_k4a.image_get_buffer(img);
  out.bytes = g_k4a.image_get_size(img);
  out.depth_unit_mm = depth_unit;
  out.owner = holder;
  out.release = &release_image_holder;

  on_frame(&out, user);
}

void depthcam_device::emitPointCloud(k4a_image_t depth, k4a_image_t color)
{
  if(!transformation || !on_frame || !depth)
    return;
  if(color_pointcloud && !color)
    return;

  // Which camera the cloud is expressed in decides its resolution, and that
  // dominates the cost -- exactly as with the other backends.
  const k4a_calibration_type_t target
      = cloud_at_color ? K4A_CALIBRATION_TYPE_COLOR : K4A_CALIBRATION_TYPE_DEPTH;

  k4a_image_t depth_for_cloud = depth;

  if(cloud_at_color)
  {
    const int w = calibration.color_camera_calibration.resolution_width;
    const int h = calibration.color_camera_calibration.resolution_height;
    if(!transformed_depth
       && g_k4a.image_create(
              K4A_IMAGE_FORMAT_DEPTH16, w, h, w * int(sizeof(uint16_t)),
              &transformed_depth)
              != K4A_RESULT_SUCCEEDED)
      return;
    if(g_k4a.transformation_depth_image_to_color_camera(
           transformation, depth, transformed_depth)
       != K4A_RESULT_SUCCEEDED)
      return;
    depth_for_cloud = transformed_depth;
  }

  const int w = g_k4a.image_get_width_pixels(depth_for_cloud);
  const int h = g_k4a.image_get_height_pixels(depth_for_cloud);

  if(!xyz_image
     && g_k4a.image_create(
            K4A_IMAGE_FORMAT_CUSTOM, w, h, w * 3 * int(sizeof(int16_t)), &xyz_image)
            != K4A_RESULT_SUCCEEDED)
    return;

  if(g_k4a.transformation_depth_image_to_point_cloud(
         transformation, depth_for_cloud, target, xyz_image)
     != K4A_RESULT_SUCCEEDED)
    return;

  const auto* xyz = reinterpret_cast<const int16_t*>(g_k4a.image_get_buffer(xyz_image));
  if(!xyz)
    return;

  // Colour, resampled into the same camera as the cloud.
  const uint8_t* rgba = nullptr;
  if(color_pointcloud)
  {
    if(cloud_at_color)
    {
      rgba = g_k4a.image_get_buffer(color);
    }
    else
    {
      if(!transformed_color
         && g_k4a.image_create(
                K4A_IMAGE_FORMAT_COLOR_BGRA32, w, h, w * 4, &transformed_color)
                != K4A_RESULT_SUCCEEDED)
        return;
      if(g_k4a.transformation_color_image_to_depth_camera(
             transformation, depth, color, transformed_color)
         != K4A_RESULT_SUCCEEDED)
        return;
      rgba = g_k4a.image_get_buffer(transformed_color);
    }
    if(!rgba)
      return;
  }

  // k4a delivers XYZ as int16 millimetres, so a conversion pass is unavoidable
  // here; the ABI promises float.
  const int stride = color_pointcloud ? 6 : 3;
  cloud_buf.clear();
  cloud_buf.reserve(size_t(w) * h * stride);

  for(int i = 0; i < w * h; i++)
  {
    const int16_t z = xyz[i * 3 + 2];
    if(z == 0) // no depth at this pixel
      continue;

    // k4a gives int16 millimetres; the ABI wants float metres.
    constexpr float mm_to_m = 0.001f;
    cloud_buf.push_back(float(xyz[i * 3 + 0]) * mm_to_m);
    cloud_buf.push_back(float(xyz[i * 3 + 1]) * mm_to_m);
    cloud_buf.push_back(float(z) * mm_to_m);

    if(color_pointcloud)
    {
      // BGRA32.
      cloud_buf.push_back(rgba[i * 4 + 2] / 255.f);
      cloud_buf.push_back(rgba[i * 4 + 1] / 255.f);
      cloud_buf.push_back(rgba[i * 4 + 0] / 255.f);
    }
  }

  if(cloud_buf.empty())
    return;

  auto* holder = new VectorHolder{cloud_buf};

  depthcam_frame out{};
  out.stream = DEPTHCAM_STREAM_POINTCLOUD;
  out.format = color_pointcloud ? DEPTHCAM_FMT_XYZRGB : DEPTHCAM_FMT_XYZ;
  out.point_count = int32_t(holder->data.size() / stride);
  out.timestamp_ns = g_k4a.image_get_device_timestamp_usec(depth) * 1000ull;
  out.data = holder->data.data();
  out.bytes = holder->data.size() * sizeof(float);
  out.owner = holder;
  out.release = &release_vector_holder;

  on_frame(&out, user);
}

void depthcam_device::handleCapture(k4a_capture_t capture)
{
  k4a_image_t color = g_k4a.capture_get_color_image(capture);
  k4a_image_t depth = g_k4a.capture_get_depth_image(capture);
  k4a_image_t ir = g_k4a.capture_get_ir_image(capture);

  // Each stream independently: a capture missing colour must still yield depth,
  // IR and the point cloud.
  if(cfg.streams & DEPTHCAM_STREAM_COLOR)
    emitImage(DEPTHCAM_STREAM_COLOR, color, 0.f);
  if(cfg.streams & DEPTHCAM_STREAM_IR)
    emitImage(DEPTHCAM_STREAM_IR, ir, 0.f);
  if(cfg.streams & DEPTHCAM_STREAM_DEPTH)
    emitImage(DEPTHCAM_STREAM_DEPTH, depth, 1.f); // DEPTH16 is millimetres
  if(cfg.streams & DEPTHCAM_STREAM_POINTCLOUD)
    emitPointCloud(depth, color);

  for(auto img : {color, depth, ir})
    if(img)
      g_k4a.image_release(img);
}

void depthcam_device::run()
{
  while(running.load(std::memory_order_acquire))
  {
    k4a_capture_t capture{};
    const auto r = g_k4a.device_get_capture(dev, &capture, 1000);

    if(r == K4A_WAIT_RESULT_TIMEOUT)
      continue;
    if(r != K4A_WAIT_RESULT_SUCCEEDED)
    {
      set_error("capture failed");
      break;
    }

    try
    {
      handleCapture(capture);
    }
    catch(...)
    {
      // Never let anything escape into libk4a's caller.
    }
    g_k4a.capture_release(capture);
  }
}

// ---------------------------------------------------------------------------

namespace
{

int backend_init(const char* resource_dir)
{
  // A missing SDK is not a load failure: the backend stays registered and simply
  // reports no cameras, so the device browser can say so.
  if(!g_k4a)
    load_k4a(g_k4a, resource_dir);
  return 1;
}

void backend_shutdown() { }

int backend_enumerate(depthcam_enumerate_cb cb, void* user)
{
  if(!g_k4a || !cb)
    return 0;

  const uint32_t n = g_k4a.device_get_installed_count();
  for(uint32_t i = 0; i < n; i++)
  {
    // The serial is only readable from an open handle, so each camera is opened
    // briefly and closed again. Unlike the Orbbec SDK there is no metadata-only
    // path, but this happens once per browser refresh and the handle is not
    // retained.
    k4a_device_t dev{};
    if(g_k4a.device_open(i, &dev) != K4A_RESULT_SUCCEEDED)
      continue;

    EnumEntry e;
    e.serial = serial_of(dev);
    g_k4a.device_close(dev);

    e.uri = e.serial.empty() ? ("k4a:index:" + std::to_string(i))
                             : ("k4a:sn:" + e.serial);

    depthcam_device_info info{};
    info.backend = "k4a";
    info.uri = e.uri.c_str();
    info.name = "Azure Kinect DK";
    info.serial = e.serial.c_str();
    info.transport = "usb3";
    info.streams = DEPTHCAM_STREAM_COLOR | DEPTHCAM_STREAM_IR
                   | DEPTHCAM_STREAM_DEPTH | DEPTHCAM_STREAM_POINTCLOUD;
    cb(&info, user);
  }
  return 1;
}

void backend_set_changed_callback(depthcam_changed_cb, void*)
{
  // libk4a has no hot-plug notification.
}

/// Nearest supported colour mode for a requested height.
k4a_color_resolution_t color_resolution_for(int height)
{
  if(height <= 0)
    return K4A_COLOR_RESOLUTION_1080P;
  if(height <= 720)
    return K4A_COLOR_RESOLUTION_720P;
  if(height <= 1080)
    return K4A_COLOR_RESOLUTION_1080P;
  if(height <= 1440)
    return K4A_COLOR_RESOLUTION_1440P;
  if(height <= 1536)
    return K4A_COLOR_RESOLUTION_1536P;
  if(height <= 2160)
    return K4A_COLOR_RESOLUTION_2160P;
  return K4A_COLOR_RESOLUTION_3072P;
}

/// NFOV unbinned (640x576) unless a smaller depth was asked for; WFOV binned is
/// the wide-angle equivalent at the same cost.
k4a_depth_mode_t depth_mode_for(int width, int height)
{
  if(width <= 0 && height <= 0)
    return K4A_DEPTH_MODE_NFOV_UNBINNED;
  if(width <= 320 || height <= 288)
    return K4A_DEPTH_MODE_NFOV_2X2BINNED;
  if(width >= 1024 || height >= 1024)
    return K4A_DEPTH_MODE_WFOV_UNBINNED;
  return K4A_DEPTH_MODE_NFOV_UNBINNED;
}

k4a_fps_t fps_for(int fps)
{
  if(fps <= 0)
    return K4A_FRAMES_PER_SECOND_30;
  if(fps <= 5)
    return K4A_FRAMES_PER_SECOND_5;
  if(fps <= 15)
    return K4A_FRAMES_PER_SECOND_15;
  return K4A_FRAMES_PER_SECOND_30;
}

depthcam_device* backend_open(const char* uri, const depthcam_open_config* config)
{
  if(!g_k4a || !config)
  {
    set_error("libk4a is not available");
    return nullptr;
  }

  const auto addr = parse_uri(uri);
  const uint32_t n = g_k4a.device_get_installed_count();
  if(n == 0)
  {
    set_error("no Azure Kinect connected");
    return nullptr;
  }

  auto dev = std::make_unique<depthcam_device>();
  dev->cfg = *config;

  uint32_t index = 0;
  switch(addr.kind)
  {
    case Address::Index:
      if(addr.index >= n)
      {
        set_error("no Azure Kinect at index " + std::to_string(addr.index));
        return nullptr;
      }
      index = addr.index;
      break;
    case Address::Serial: {
      bool found = false;
      for(uint32_t i = 0; i < n && !found; i++)
      {
        k4a_device_t probe{};
        if(g_k4a.device_open(i, &probe) != K4A_RESULT_SUCCEEDED)
          continue;
        if(serial_of(probe) == addr.serial)
        {
          index = i;
          found = true;
        }
        g_k4a.device_close(probe);
      }
      if(!found)
      {
        // No silent fallback: streaming from a different camera than the
        // document names is worse than failing to connect.
        set_error("Azure Kinect '" + addr.serial + "' is not connected");
        return nullptr;
      }
      break;
    }
    case Address::Any:
      index = 0;
      break;
  }

  if(g_k4a.device_open(index, &dev->dev) != K4A_RESULT_SUCCEEDED)
  {
    set_error("could not open the Azure Kinect (is it on a USB3 port?)");
    return nullptr;
  }

  const bool want_cloud = (config->streams & DEPTHCAM_STREAM_POINTCLOUD) != 0;
  dev->color_pointcloud = want_cloud && config->color_pointcloud != 0
                          && config->align != DEPTHCAM_ALIGN_NONE;
  dev->cloud_at_color = config->align == DEPTHCAM_ALIGN_DEPTH_TO_COLOR;

  k4a_device_configuration_t k4acfg = K4A_DEVICE_CONFIG_INIT_DISABLE_ALL;

  const bool need_color
      = (config->streams & DEPTHCAM_STREAM_COLOR) || dev->color_pointcloud;
  const bool need_depth = (config->streams & DEPTHCAM_STREAM_DEPTH)
                          || (config->streams & DEPTHCAM_STREAM_IR) || want_cloud;

  if(need_color)
  {
    // BGRA32 rather than the default MJPG: the transformation functions need
    // uncompressed colour, and the host would otherwise decode it only to have
    // the cloud need it again. The SDK converts on the host either way.
    k4acfg.color_format = dev->color_pointcloud ? K4A_IMAGE_FORMAT_COLOR_BGRA32
                                                : K4A_IMAGE_FORMAT_COLOR_MJPG;
    k4acfg.color_resolution = color_resolution_for(config->color_height);
  }

  if(need_depth)
    k4acfg.depth_mode = depth_mode_for(config->depth_width, config->depth_height);

  k4acfg.camera_fps = fps_for(config->color_fps > 0 ? config->color_fps
                                                    : config->depth_fps);
  // Without this the colour and depth images of one capture can be milliseconds
  // apart, which shows up as fringing on a coloured point cloud.
  k4acfg.synchronized_images_only = need_color && need_depth;

  if(g_k4a.device_get_calibration(
         dev->dev, k4acfg.depth_mode, k4acfg.color_resolution, &dev->calibration)
     != K4A_RESULT_SUCCEEDED)
  {
    set_error("could not read the Azure Kinect calibration");
    return nullptr;
  }

  if(want_cloud)
  {
    dev->transformation = g_k4a.transformation_create(&dev->calibration);
    if(!dev->transformation)
    {
      set_error("could not create the Azure Kinect transformation");
      return nullptr;
    }
  }

  if(g_k4a.device_start_cameras(dev->dev, &k4acfg) != K4A_RESULT_SUCCEEDED)
  {
    // Almost always either a USB2 port or a missing libdepthengine.
    set_error("could not start the Azure Kinect cameras; check the USB3 "
              "connection and that libdepthengine is installed");
    return nullptr;
  }

  return dev.release();
}

void backend_stop(depthcam_device* dev);

void backend_close(depthcam_device* dev)
{
  if(!dev)
    return;
  backend_stop(dev);
  if(dev->dev)
    g_k4a.device_stop_cameras(dev->dev);
  delete dev;
}

int backend_start(depthcam_device* dev, depthcam_frame_cb on_frame, void* user)
{
  if(!dev || !dev->dev)
    return 0;
  if(dev->running.load(std::memory_order_acquire))
    return 1;

  dev->on_frame = on_frame;
  dev->user = user;
  dev->running.store(true, std::memory_order_release);
  // k4a is a pull API, so unlike the other backends this one owns a thread.
  dev->thread = std::thread{[dev] { dev->run(); }};
  return 1;
}

void backend_stop(depthcam_device* dev)
{
  if(!dev)
    return;
  if(!dev->running.exchange(false, std::memory_order_acq_rel))
    return;
  if(dev->thread.joinable())
    dev->thread.join();
}

const depthcam_backend_v1 g_backend{
    .abi_version = DEPTHCAM_ABI_VERSION,
    .name = "k4a",
    .display_name = "Azure Kinect",
    .last_error = &backend_last_error,
    .init = &backend_init,
    .shutdown = &backend_shutdown,
    .enumerate = &backend_enumerate,
    .set_changed_callback = &backend_set_changed_callback,
    .open = &backend_open,
    .close = &backend_close,
    .start = &backend_start,
    .stop = &backend_stop,
};

} // namespace

extern "C" DEPTHCAM_EXPORT const depthcam_backend_v1* score_depthcam_backend_v1(void)
{
  return &g_backend;
}
