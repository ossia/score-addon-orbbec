/*
 * Kinect v2 backend: libfreenect2 behind depthcam_abi.h.
 */
#include <depthcam_abi.h>

#include <libfreenect2/config.h>
#include <libfreenect2/frame_listener.hpp>
#include <libfreenect2/libfreenect2.hpp>
#include <libfreenect2/logger.h>
#include <libfreenect2/packet_pipeline.h>
#include <libfreenect2/registration.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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

/// libfreenect2's Freenect2 object owns the USB enumeration state; one is
/// enough and it must outlive every device.
std::unique_ptr<libfreenect2::Freenect2> g_freenect2;

struct EnumEntry
{
  std::string uri, name, serial;
};

/// "freenect2:sn:012345678901" or "freenect2:index:0"; anything else is taken
/// as a bare serial.
struct Address
{
  enum Kind
  {
    Any,
    Serial,
    Index
  } kind{Any};
  std::string serial;
  int index{0};
};

Address parse_uri(const char* uri)
{
  Address a;
  if(!uri)
    return a;
  std::string s{uri};
  if(s.rfind("freenect2:", 0) == 0)
    s = s.substr(10);
  if(s.empty())
    return a;

  if(s.rfind("sn:", 0) == 0)
  {
    a.kind = Address::Serial;
    a.serial = s.substr(3);
  }
  else if(s.rfind("index:", 0) == 0)
  {
    a.kind = Address::Index;
    try
    {
      a.index = std::stoi(s.substr(6));
    }
    catch(...)
    {
      a.kind = Address::Any;
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
// --- controls ---------------------------------------------------------------

/**
 * The Kinect v2's colour camera has three exposure modes and no getters at all:
 * libfreenect2 offers setColorAutoExposure / setColorSemiAutoExposure /
 * setColorManualExposure and nothing that reads back. So the values published
 * here are what was last written, seeded with libfreenect2's own defaults --
 * which is the honest thing to show, and the only thing available.
 *
 * The three modes take different parameters, and writing any parameter also
 * re-applies the mode it belongs to, so setting `integration_time` puts the
 * camera in manual rather than being silently ignored.
 */
enum class ExposureMode
{
  Auto = 0,
  SemiAuto,
  Manual
};

const char* const exposure_mode_labels[] = {"auto", "semi-auto", "manual"};

struct ControlDef
{
  const char* id;
  const char* name;
  const char* description;
  int kind;
  double min, max, step, def;
  const char* const* labels;
  int label_count;
};

const ControlDef freenect2_controls[] = {
    {"color/exposure_mode", "exposure mode",
     "Which of the three exposure controls the camera obeys",
     DEPTHCAM_CONTROL_ENUM, 0, 2, 1, 0, exposure_mode_labels, 3},
    {"color/exposure_compensation", "exposure compensation",
     "Automatic mode only, in stops", DEPTHCAM_CONTROL_FLOAT, -2, 2, 0, 0,
     nullptr, 0},
    {"color/pseudo_exposure_time", "pseudo exposure time",
     "Semi-automatic mode only, in milliseconds; the camera trades integration "
     "time against gain to reach it",
     DEPTHCAM_CONTROL_FLOAT, 0, 640, 0, 33, nullptr, 0},
    {"color/integration_time", "integration time",
     "Manual mode only, in milliseconds", DEPTHCAM_CONTROL_FLOAT, 0.5, 66, 0,
     33, nullptr, 0},
    {"color/analog_gain", "analog gain", "Manual mode only",
     DEPTHCAM_CONTROL_FLOAT, 1.0, 1.5, 0, 1.0, nullptr, 0},
};

} // namespace

// ---------------------------------------------------------------------------

struct depthcam_device
{
  libfreenect2::Freenect2Device* dev{};
  libfreenect2::PacketPipeline* pipeline{};
  const char* pipeline_name{"cpu"};
  std::unique_ptr<libfreenect2::FrameListener> listener;
  std::unique_ptr<libfreenect2::Registration> registration;

  depthcam_open_config cfg{};
  bool color_pointcloud{};
  std::atomic_bool running{};

  depthcam_frame_cb on_frame{};
  void* user{};

  /// The point cloud needs colour and depth together, but the listener delivers
  /// them separately, so the most recent of each is kept until they pair up.
  std::mutex pair_mutex;
  std::shared_ptr<std::vector<uint8_t>> last_color;
  std::shared_ptr<std::vector<uint8_t>> last_depth;
  int last_color_w{}, last_color_h{};

  // Reused across frames: at 512x424 these are ~3.5MB together and
  // reallocating them per frame would dominate the cost.
  std::vector<unsigned char> undistorted_buf;
  std::vector<unsigned char> registered_buf;
  std::vector<float> cloud_buf;

  /// Mirrors of what was last written; libfreenect2 has no getters.
  std::mutex control_lock;
  ExposureMode exposure_mode{ExposureMode::Auto};
  double exposure_compensation{0.};
  double pseudo_exposure_ms{33.};
  double integration_ms{33.};
  double analog_gain{1.};

  void applyExposure();

  ~depthcam_device();

  void onFrame(libfreenect2::Frame::Type type, libfreenect2::Frame* frame);
  void emitPointCloud();
};

namespace
{

/**
 * @brief Owns a copy of a libfreenect2 frame for as long as the host holds it.
 *
 * A copy, not a reference, and that is forced. libfreenect2's packet processors
 * allocate frames from a small fixed pool and *block* waiting for one to come
 * back, so holding a frame until the host has finished rendering starves the
 * pipeline: measured, depth and the point cloud delivered exactly one frame and
 * then stopped, while colour stuttered with second-long gaps.
 *
 * Every other backend here refcounts (Orbbec, k4a, librealsense) or lets us
 * borrow safely, so this is the one place a copy is unavoidable.
 */
struct BufferHolder
{
  std::shared_ptr<std::vector<uint8_t>> data;
};

void release_buffer_holder(void* owner)
{
  delete static_cast<BufferHolder*>(owner);
}

/// Owns a slice of the device's reusable cloud buffer for as long as the host
/// holds it. The buffer is copied out because the next frame overwrites it.
struct VectorHolder
{
  std::vector<float> data;
};

void release_vector_holder(void* owner)
{
  delete static_cast<VectorHolder*>(owner);
}

/**
 * @brief Pick the fastest depth pipeline this build and machine can offer.
 *
 * Order matters: CUDA and OpenCL both move the depth decode off the CPU, and
 * either is worth several times the CPU pipeline's frame rate. Each constructor
 * can throw or produce a pipeline that fails later on a machine with no usable
 * device, so every step is guarded and the CPU pipeline is always reachable.
 */
libfreenect2::PacketPipeline* createPipeline(const char*& chosen)
{
#if defined(LIBFREENECT2_WITH_CUDA_SUPPORT)
  try
  {
    if(auto* p = new libfreenect2::CudaPacketPipeline())
    {
      chosen = "cuda";
      return p;
    }
  }
  catch(...)
  {
  }
#endif

#if defined(LIBFREENECT2_WITH_OPENCL_SUPPORT)
  try
  {
    if(auto* p = new libfreenect2::OpenCLPacketPipeline())
    {
      chosen = "opencl";
      return p;
    }
  }
  catch(...)
  {
  }
#endif

  chosen = "cpu";
  return new libfreenect2::CpuPacketPipeline();
}

struct Listener final : libfreenect2::FrameListener
{
  depthcam_device& self;
  explicit Listener(depthcam_device& s)
      : self{s}
  {
  }

  bool onNewFrame(libfreenect2::Frame::Type type, libfreenect2::Frame* frame) override
  {
    self.onFrame(type, frame);
    // We always take ownership; every path either hands the frame to the host
    // (which deletes it through release) or deletes it itself.
    return true;
  }
};

} // namespace

depthcam_device::~depthcam_device()
{
  if(dev)
  {
    dev->stop();
    dev->close();
  }
  // Freenect2Device is owned by the Freenect2 object; the pipeline is consumed
  // by openDevice and freed with the device.
}

void depthcam_device::onFrame(
    libfreenect2::Frame::Type type, libfreenect2::Frame* frame)
{
  if(!running.load(std::memory_order_acquire) || !frame)
  {
    delete frame;
    return;
  }

  uint32_t stream = 0;
  int format = DEPTHCAM_FMT_NONE;
  float unit = 0.f;

  switch(type)
  {
    case libfreenect2::Frame::Color:
      stream = DEPTHCAM_STREAM_COLOR;
      // 32bpp with an unused byte; libfreenect2 reports BGRX or RGBX.
      format = (frame->format == libfreenect2::Frame::RGBX) ? DEPTHCAM_FMT_RGB0
                                                            : DEPTHCAM_FMT_BGR0;
      break;
    case libfreenect2::Frame::Ir:
      stream = DEPTHCAM_STREAM_IR;
      format = DEPTHCAM_FMT_GRAYF32;
      break;
    case libfreenect2::Frame::Depth:
      stream = DEPTHCAM_STREAM_DEPTH;
      format = DEPTHCAM_FMT_GRAYF32;
      unit = 1.f; // libfreenect2 depth is float millimetres
      break;
    default:
      delete frame;
      return;
  }

  const size_t bytes = size_t(frame->width) * frame->height * frame->bytes_per_pixel;
  const bool want_cloud = (cfg.streams & DEPTHCAM_STREAM_POINTCLOUD) != 0;
  const bool wants_stream = (cfg.streams & stream) && on_frame;
  const bool needed_by_cloud
      = want_cloud
        && (type == libfreenect2::Frame::Depth
            || (type == libfreenect2::Frame::Color && color_pointcloud));

  // One copy, shared. The copy itself is forced -- libfreenect2's processors
  // allocate from a small pool and block until a buffer comes back, so we
  // cannot hand the original to the host -- but colour used to be copied
  // *twice* when the cloud was coloured, once for the host and once for the
  // pairing buffer. At 1920x1080x4 that was an extra 227 MB/s.
  std::shared_ptr<std::vector<uint8_t>> buf;
  if(wants_stream || needed_by_cloud)
    buf = std::make_shared<std::vector<uint8_t>>(frame->data, frame->data + bytes);

  bool keep_for_cloud = false;
  if(needed_by_cloud)
  {
    std::lock_guard lock{pair_mutex};
    ((type == libfreenect2::Frame::Color) ? last_color : last_depth) = buf;
    if(type == libfreenect2::Frame::Color)
    {
      last_color_w = frame->width;
      last_color_h = frame->height;
    }
    keep_for_cloud = true;
  }

  if(wants_stream)
  {
    auto* holder = new BufferHolder{buf};

    depthcam_frame out{};
    out.stream = stream;
    out.format = format;
    out.width = int32_t(frame->width);
    out.height = int32_t(frame->height);
    out.stride = int32_t(frame->width * frame->bytes_per_pixel);
    out.timestamp_ns = uint64_t(frame->timestamp) * 125000ull; // 1/8 ms ticks
    out.data = buf->data();
    out.bytes = buf->size();
    out.depth_unit_mm = unit;
    out.owner = holder;
    out.release = &release_buffer_holder;

    on_frame(&out, user);
  }

  // Back to libfreenect2's pool immediately, whatever happened above: its
  // processors block until a buffer is free. The cloud works off our own copies
  // in last_color/last_depth, so it does not need the original.
  delete frame;

  // Only off the depth frame. Emitting on colour too recomputed the cloud
  // against an unchanged depth image and pushed duplicates downstream -- 45
  // clouds for 8 depth frames in a 5s run.
  if(keep_for_cloud && type == libfreenect2::Frame::Depth)
    emitPointCloud();
}

void depthcam_device::applyExposure()
{
  // Caller holds control_lock.
  //
  // Only while streaming: these are bulk command transactions on the control
  // endpoint, and a Kinect v2 that has not been started answers none of them
  // (LIBUSB_ERROR_TIMEOUT, once per call, ten seconds each). The values are
  // kept regardless and re-applied from backend_start, so a setting made while
  // the transport is stopped is not lost -- it just takes effect on play.
  if(!dev || !running.load(std::memory_order_acquire))
    return;
  try
  {
    switch(exposure_mode)
    {
      case ExposureMode::Auto:
        dev->setColorAutoExposure(float(exposure_compensation));
        break;
      case ExposureMode::SemiAuto:
        dev->setColorSemiAutoExposure(float(pseudo_exposure_ms));
        break;
      case ExposureMode::Manual:
        dev->setColorManualExposure(float(integration_ms), float(analog_gain));
        break;
    }
  }
  catch(...)
  {
    // libfreenect2 writes these as raw commands to the device and can throw on
    // a camera that has gone away; a failed setting is not worth taking the
    // stream down for.
  }
}

void depthcam_device::emitPointCloud()
{
  if(!registration || !on_frame)
    return;
  if(!(cfg.streams & DEPTHCAM_STREAM_POINTCLOUD))
    return;

  std::lock_guard lock{pair_mutex};
  if(!last_depth)
    return;
  if(color_pointcloud && !last_color)
    return;

  constexpr int w = 512, h = 424;
  if(last_depth->size() != size_t(w) * h * 4)
    return;

  undistorted_buf.resize(size_t(w) * h * 4);
  registered_buf.resize(size_t(w) * h * 4);

  // Views over our own copies; libfreenect2::Frame does not own data passed to
  // this constructor.
  libfreenect2::Frame depth_view(w, h, 4, last_depth->data());
  depth_view.format = libfreenect2::Frame::Float;
  libfreenect2::Frame undistorted(w, h, 4, undistorted_buf.data());
  libfreenect2::Frame registered(w, h, 4, registered_buf.data());

  if(color_pointcloud)
  {
    libfreenect2::Frame color_view(
        last_color_w, last_color_h, 4, last_color->data());
    color_view.format = libfreenect2::Frame::BGRX;
    registration->apply(&color_view, &depth_view, &undistorted, &registered);
  }
  else
  {
    registration->undistortDepth(&depth_view, &undistorted);
  }

  const int stride = color_pointcloud ? 6 : 3;
  cloud_buf.clear();
  cloud_buf.reserve(size_t(w) * h * stride);

  for(int r = 0; r < h; ++r)
  {
    for(int c = 0; c < w; ++c)
    {
      float x{}, y{}, z{}, rgb{};
      if(color_pointcloud)
        registration->getPointXYZRGB(&undistorted, &registered, r, c, x, y, z, rgb);
      else
        registration->getPointXYZ(&undistorted, r, c, x, y, z);

      // Invalid depth comes back as NaN; skipping keeps the cloud clean and
      // shrinks it, and the host derives the count from point_count.
      if(!(x == x) || !(z == z))
        continue;

      // Already metres: libfreenect2's Registration works in metres, unlike
      // every other backend here. No conversion -- see DEPTHCAM_FMT_XYZ.
      cloud_buf.push_back(x);
      cloud_buf.push_back(y);
      cloud_buf.push_back(z);

      if(color_pointcloud)
      {
        const auto* p = reinterpret_cast<const uint8_t*>(&rgb);
        // getPointXYZRGB packs BGR into the float; normalise to 0..1 to match
        // what DEPTHCAM_FMT_XYZRGB promises.
        cloud_buf.push_back(p[2] / 255.f);
        cloud_buf.push_back(p[1] / 255.f);
        cloud_buf.push_back(p[0] / 255.f);
      }
    }
  }

  if(cloud_buf.empty())
    return;

  auto* holder = new VectorHolder{cloud_buf};

  depthcam_frame out{};
  out.stream = DEPTHCAM_STREAM_POINTCLOUD;
  out.format = color_pointcloud ? DEPTHCAM_FMT_XYZRGB : DEPTHCAM_FMT_XYZ;
  out.point_count = int32_t(holder->data.size() / stride);
  out.timestamp_ns = 0;
  out.data = holder->data.data();
  out.bytes = holder->data.size() * sizeof(float);
  out.owner = holder;
  out.release = &release_vector_holder;

  on_frame(&out, user);
}

// ---------------------------------------------------------------------------

namespace
{

int backend_init(const char*)
{
  try
  {
    // libfreenect2 logs at Info to stdout by default, far too chatty inside a
    // host application.
    libfreenect2::setGlobalLogger(
        libfreenect2::createConsoleLogger(libfreenect2::Logger::Warning));
    g_freenect2 = std::make_unique<libfreenect2::Freenect2>();
    return 1;
  }
  catch(const std::exception& e)
  {
    set_error(e.what());
    return 0;
  }
  catch(...)
  {
    return 0;
  }
}

void backend_shutdown()
{
  g_freenect2.reset();
}

int backend_enumerate(depthcam_enumerate_cb cb, void* user)
{
  if(!g_freenect2 || !cb)
    return 0;

  try
  {
    const int n = g_freenect2->enumerateDevices();
    for(int i = 0; i < n; i++)
    {
      EnumEntry e;
      e.serial = g_freenect2->getDeviceSerialNumber(i);
      e.uri = e.serial.empty() ? ("freenect2:index:" + std::to_string(i))
                               : ("freenect2:sn:" + e.serial);
      e.name = "Kinect v2";

      depthcam_device_info info{};
      info.backend = "freenect2";
      info.uri = e.uri.c_str();
      info.name = e.name.c_str();
      info.serial = e.serial.c_str();
      info.transport = "usb3";
      info.streams = DEPTHCAM_STREAM_COLOR | DEPTHCAM_STREAM_IR
                     | DEPTHCAM_STREAM_DEPTH | DEPTHCAM_STREAM_POINTCLOUD;
      cb(&info, user);
    }
    return 1;
  }
  catch(const std::exception& e)
  {
    set_error(e.what());
    return 0;
  }
  catch(...)
  {
    return 0;
  }
}

void backend_set_changed_callback(depthcam_changed_cb, void*)
{
  // libfreenect2 has no hot-plug notification; the host re-enumerates on demand.
}

depthcam_device* backend_open(const char* uri, const depthcam_open_config* config)
{
  if(!g_freenect2 || !config)
    return nullptr;

  try
  {
    const auto addr = parse_uri(uri);

    const int n = g_freenect2->enumerateDevices();
    if(n == 0)
    {
      set_error("no Kinect v2 connected");
      return nullptr;
    }

    std::string serial;
    switch(addr.kind)
    {
      case Address::Serial:
        for(int i = 0; i < n; i++)
          if(g_freenect2->getDeviceSerialNumber(i) == addr.serial)
            serial = addr.serial;
        if(serial.empty())
        {
          // No silent fallback to another camera: streaming from a different
          // one than the document names is worse than failing.
          set_error("Kinect v2 '" + addr.serial + "' is not connected");
          return nullptr;
        }
        break;
      case Address::Index:
        if(addr.index < 0 || addr.index >= n)
        {
          set_error("no Kinect v2 at index " + std::to_string(addr.index));
          return nullptr;
        }
        serial = g_freenect2->getDeviceSerialNumber(addr.index);
        break;
      case Address::Any:
        serial = g_freenect2->getDefaultDeviceSerialNumber();
        break;
    }

    auto dev = std::make_unique<depthcam_device>();
    dev->cfg = *config;
    dev->color_pointcloud = (config->streams & DEPTHCAM_STREAM_POINTCLOUD)
                            && config->color_pointcloud != 0;

    // Best available accelerated pipeline, falling back to the CPU one. The
    // Kinect v2's depth decode is the expensive part -- on the CPU pipeline it
    // is what limits the depth rate, not USB. OpenGLPacketPipeline is never
    // considered: it creates its own GLFW context and window, which has no
    // business inside a Qt RHI process.
    dev->pipeline = createPipeline(dev->pipeline_name);

    dev->dev = g_freenect2->openDevice(serial, dev->pipeline);
    if(!dev->dev)
    {
      set_error("could not open Kinect v2 " + serial);
      return nullptr;
    }

    set_error(
        std::string{"using the "} + dev->pipeline_name + " depth pipeline");

    dev->listener = std::make_unique<Listener>(*dev);
    dev->dev->setColorFrameListener(dev->listener.get());
    dev->dev->setIrAndDepthFrameListener(dev->listener.get());

    return dev.release();
  }
  catch(const std::exception& e)
  {
    set_error(e.what());
    return nullptr;
  }
  catch(...)
  {
    set_error("unknown error while opening the device");
    return nullptr;
  }
}

void backend_stop(depthcam_device* dev);

void backend_close(depthcam_device* dev)
{
  if(!dev)
    return;
  backend_stop(dev);
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

  const bool want_color = (dev->cfg.streams & DEPTHCAM_STREAM_COLOR)
                          || dev->color_pointcloud;
  const bool want_depth = (dev->cfg.streams & DEPTHCAM_STREAM_DEPTH)
                          || (dev->cfg.streams & DEPTHCAM_STREAM_IR)
                          || (dev->cfg.streams & DEPTHCAM_STREAM_POINTCLOUD);

  const bool ok = (want_color && want_depth)
                      ? dev->dev->start()
                      : dev->dev->startStreams(want_color, want_depth);
  if(!ok)
  {
    set_error("could not start the Kinect v2 streams");
    dev->running.store(false, std::memory_order_release);
    return 0;
  }

  // Whatever was set while the transport was stopped; see applyExposure.
  {
    std::lock_guard lock{dev->control_lock};
    dev->applyExposure();
  }

  if(dev->cfg.streams & DEPTHCAM_STREAM_POINTCLOUD)
  {
    // Only valid once the device is started: the parameters are read from the
    // camera during startup.
    dev->registration = std::make_unique<libfreenect2::Registration>(
        dev->dev->getIrCameraParams(), dev->dev->getColorCameraParams());
  }

  return 1;
}

void backend_stop(depthcam_device* dev)
{
  if(!dev)
    return;
  if(!dev->running.exchange(false, std::memory_order_acq_rel))
    return;
  if(dev->dev)
    dev->dev->stop();
}

int backend_list_controls(depthcam_device* dev, depthcam_control_cb cb, void* user)
{
  if(!dev || !dev->dev || !cb)
    return 0;

  for(const auto& d : freenect2_controls)
  {
    depthcam_control c{};
    c.id = d.id;
    c.name = d.name;
    c.description = d.description;
    c.kind = d.kind;
    // Readable only in the sense that we remember what we wrote; see the note
    // above freenect2_controls.
    c.access = DEPTHCAM_ACCESS_READ | DEPTHCAM_ACCESS_WRITE;
    c.min = d.min;
    c.max = d.max;
    c.step = d.step;
    c.def = d.def;
    c.enum_labels = d.labels;
    c.enum_count = d.label_count;
    cb(&c, user);
  }
  return 1;
}

int backend_get_control(depthcam_device* dev, const char* id, double* out)
{
  if(!dev || !id || !out)
    return 0;
  std::lock_guard lock{dev->control_lock};

  const std::string s{id};
  if(s == "color/exposure_mode")
    *out = double(int(dev->exposure_mode));
  else if(s == "color/exposure_compensation")
    *out = dev->exposure_compensation;
  else if(s == "color/pseudo_exposure_time")
    *out = dev->pseudo_exposure_ms;
  else if(s == "color/integration_time")
    *out = dev->integration_ms;
  else if(s == "color/analog_gain")
    *out = dev->analog_gain;
  else
    return 0;
  return 1;
}

int backend_set_control(depthcam_device* dev, const char* id, double value)
{
  if(!dev || !dev->dev || !id)
    return 0;
  std::lock_guard lock{dev->control_lock};

  const std::string s{id};
  if(s == "color/exposure_mode")
  {
    const int v = int(value);
    if(v < 0 || v > 2)
      return 0;
    dev->exposure_mode = ExposureMode(v);
  }
  else if(s == "color/exposure_compensation")
  {
    dev->exposure_compensation = value;
    dev->exposure_mode = ExposureMode::Auto;
  }
  else if(s == "color/pseudo_exposure_time")
  {
    dev->pseudo_exposure_ms = value;
    dev->exposure_mode = ExposureMode::SemiAuto;
  }
  else if(s == "color/integration_time")
  {
    dev->integration_ms = value;
    dev->exposure_mode = ExposureMode::Manual;
  }
  else if(s == "color/analog_gain")
  {
    dev->analog_gain = value;
    dev->exposure_mode = ExposureMode::Manual;
  }
  else
  {
    return 0;
  }

  dev->applyExposure();
  return 1;
}

const depthcam_backend_v1 g_backend{
    .abi_version = DEPTHCAM_ABI_VERSION,
    .name = "freenect2",
    .display_name = "Kinect v2",
    .last_error = &backend_last_error,
    .init = &backend_init,
    .shutdown = &backend_shutdown,
    .enumerate = &backend_enumerate,
    .set_changed_callback = &backend_set_changed_callback,
    .open = &backend_open,
    .close = &backend_close,
    .start = &backend_start,
    .stop = &backend_stop,
    .list_controls = &backend_list_controls,
    .get_control = &backend_get_control,
    .set_control = &backend_set_control,
};

} // namespace

extern "C" DEPTHCAM_EXPORT const depthcam_backend_v1* score_depthcam_backend_v1(void)
{
  return &g_backend;
}
