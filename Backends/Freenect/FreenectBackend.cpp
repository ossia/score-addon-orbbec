/*
 * Kinect v1 (Xbox 360) backend: libfreenect behind depthcam_abi.h.
 *
 * libfreenect owns its frame buffers and recycles them between callbacks, so
 * unlike the Orbbec and k4a backends this one has to copy. At 640x480 that is
 * ~900KB per colour frame; the alternative (freenect_set_video_buffer with our
 * own pool) would let the device write straight into a buffer the host might
 * still be rendering from.
 *
 * libfreenect is also an event-loop API -- freenect_process_events blocks --
 * so the backend runs its own thread.
 */
#include <depthcam_abi.h>

#include <libfreenect.h>
#include <libfreenect_registration.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

freenect_context* g_ctx{};

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
  if(s.rfind("freenect:", 0) == 0)
    s = s.substr(9);
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
      a.index = std::stoi(s.substr(6));
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

struct EnumEntry
{
  std::string uri, serial;
};

/// Owns a copy of a libfreenect buffer for as long as the host holds it.
struct VectorHolder
{
  std::vector<uint8_t> data;
};

void release_vector_holder(void* owner)
{
  delete static_cast<VectorHolder*>(owner);
}

struct FloatHolder
{
  std::vector<float> data;
};

void release_float_holder(void* owner)
{
  delete static_cast<FloatHolder*>(owner);
}
} // namespace

// ---------------------------------------------------------------------------

struct depthcam_device
{
  freenect_device* dev{};

  depthcam_open_config cfg{};
  bool color_pointcloud{};

  std::atomic_bool running{};
  std::thread thread;

  depthcam_frame_cb on_frame{};
  void* user{};

  int color_w{640}, color_h{480};
  int depth_w{640}, depth_h{480};

  /// The cloud needs colour and depth together; the callbacks are separate.
  std::mutex pair_mutex;
  std::vector<uint8_t> last_color;
  std::vector<uint16_t> last_depth;
  bool have_color{}, have_depth{};

  std::vector<float> cloud_buf;

  ~depthcam_device();

  void onVideo(void* data, uint32_t timestamp);
  void onDepth(void* data, uint32_t timestamp);
  void emitPointCloud();
  void run();
};

namespace
{
void video_cb(freenect_device* dev, void* data, uint32_t timestamp)
{
  if(auto* self = static_cast<depthcam_device*>(freenect_get_user(dev)))
    self->onVideo(data, timestamp);
}

void depth_cb(freenect_device* dev, void* data, uint32_t timestamp)
{
  if(auto* self = static_cast<depthcam_device*>(freenect_get_user(dev)))
    self->onDepth(data, timestamp);
}
} // namespace

depthcam_device::~depthcam_device()
{
  if(dev)
  {
    freenect_stop_video(dev);
    freenect_stop_depth(dev);
    freenect_close_device(dev);
  }
}

void depthcam_device::onVideo(void* data, uint32_t timestamp)
{
  if(!running.load(std::memory_order_acquire) || !data)
    return;

  const size_t bytes = size_t(color_w) * color_h * 3; // FREENECT_VIDEO_RGB

  const bool want_cloud = (cfg.streams & DEPTHCAM_STREAM_POINTCLOUD) != 0;
  if(want_cloud && color_pointcloud)
  {
    std::lock_guard lock{pair_mutex};
    last_color.assign(
        static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + bytes);
    have_color = true;
  }

  if((cfg.streams & DEPTHCAM_STREAM_COLOR) && on_frame)
  {
    auto* holder = new VectorHolder{};
    holder->data.assign(
        static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + bytes);

    depthcam_frame out{};
    out.stream = DEPTHCAM_STREAM_COLOR;
    out.format = DEPTHCAM_FMT_RGB24;
    out.width = color_w;
    out.height = color_h;
    out.stride = color_w * 3;
    out.timestamp_ns = uint64_t(timestamp) * 1000ull;
    out.data = holder->data.data();
    out.bytes = holder->data.size();
    out.owner = holder;
    out.release = &release_vector_holder;
    on_frame(&out, user);
  }
}

void depthcam_device::onDepth(void* data, uint32_t timestamp)
{
  if(!running.load(std::memory_order_acquire) || !data)
    return;

  const size_t count = size_t(depth_w) * depth_h;
  const auto* d = static_cast<const uint16_t*>(data);

  const bool want_cloud = (cfg.streams & DEPTHCAM_STREAM_POINTCLOUD) != 0;
  if(want_cloud)
  {
    std::lock_guard lock{pair_mutex};
    last_depth.assign(d, d + count);
    have_depth = true;
  }

  if((cfg.streams & DEPTHCAM_STREAM_DEPTH) && on_frame)
  {
    auto* holder = new VectorHolder{};
    holder->data.resize(count * sizeof(uint16_t));
    std::memcpy(holder->data.data(), d, holder->data.size());

    depthcam_frame out{};
    out.stream = DEPTHCAM_STREAM_DEPTH;
    out.format = DEPTHCAM_FMT_GRAY16;
    out.width = depth_w;
    out.height = depth_h;
    out.stride = depth_w * int(sizeof(uint16_t));
    out.timestamp_ns = uint64_t(timestamp) * 1000ull;
    // FREENECT_DEPTH_MM / _REGISTERED are already millimetres.
    out.depth_unit_mm = 1.f;
    out.data = holder->data.data();
    out.bytes = holder->data.size();
    out.owner = holder;
    out.release = &release_vector_holder;
    on_frame(&out, user);
  }

  if(want_cloud)
    emitPointCloud();
}

void depthcam_device::emitPointCloud()
{
  if(!on_frame || !dev)
    return;

  std::lock_guard lock{pair_mutex};
  if(!have_depth)
    return;
  if(color_pointcloud && !have_color)
    return;

  const int stride = color_pointcloud ? 6 : 3;
  cloud_buf.clear();
  cloud_buf.reserve(size_t(depth_w) * depth_h * stride);

  for(int y = 0; y < depth_h; y++)
  {
    for(int x = 0; x < depth_w; x++)
    {
      const int i = y * depth_w + x;
      const uint16_t z = last_depth[i];
      // 0 and the 11-bit saturation value both mean "no reading".
      if(z == 0 || z == 2047 || z >= 10000)
        continue;

      double wx{}, wy{};
      freenect_camera_to_world(dev, x, y, z, &wx, &wy);

      // freenect_camera_to_world and DEPTH_MM are both millimetres; the ABI
      // wants metres.
      constexpr float mm_to_m = 0.001f;
      cloud_buf.push_back(float(wx) * mm_to_m);
      cloud_buf.push_back(float(wy) * mm_to_m);
      cloud_buf.push_back(float(z) * mm_to_m);

      if(color_pointcloud)
      {
        // Depth is registered to the colour camera, so the indices line up.
        const uint8_t* p = last_color.data() + size_t(i) * 3;
        cloud_buf.push_back(p[0] / 255.f);
        cloud_buf.push_back(p[1] / 255.f);
        cloud_buf.push_back(p[2] / 255.f);
      }
    }
  }

  if(cloud_buf.empty())
    return;

  auto* holder = new FloatHolder{cloud_buf};

  depthcam_frame out{};
  out.stream = DEPTHCAM_STREAM_POINTCLOUD;
  out.format = color_pointcloud ? DEPTHCAM_FMT_XYZRGB : DEPTHCAM_FMT_XYZ;
  out.point_count = int32_t(holder->data.size() / stride);
  out.data = holder->data.data();
  out.bytes = holder->data.size() * sizeof(float);
  out.owner = holder;
  out.release = &release_float_holder;

  on_frame(&out, user);
}

void depthcam_device::run()
{
  while(running.load(std::memory_order_acquire))
  {
    // Returns after handling pending USB events or the 100ms timeout, so the
    // running flag is checked regularly.
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    if(freenect_process_events_timeout(g_ctx, &tv) < 0)
      break;
  }
}

// ---------------------------------------------------------------------------

namespace
{

int backend_init(const char*)
{
  if(g_ctx)
    return 1;

  if(freenect_init(&g_ctx, nullptr) < 0)
  {
    set_error("freenect_init failed");
    g_ctx = nullptr;
    return 0;
  }

  // libfreenect logs at Notice on stdout by default.
  freenect_set_log_level(g_ctx, FREENECT_LOG_WARNING);

  // The motor and audio subdevices are separate USB interfaces; only the camera
  // is needed here, and asking for the others makes open fail on a Kinect whose
  // audio firmware has not been uploaded.
  freenect_select_subdevices(
      g_ctx, static_cast<freenect_device_flags>(FREENECT_DEVICE_CAMERA));
  return 1;
}

void backend_shutdown()
{
  if(g_ctx)
  {
    freenect_shutdown(g_ctx);
    g_ctx = nullptr;
  }
}

int backend_enumerate(depthcam_enumerate_cb cb, void* user)
{
  if(!g_ctx || !cb)
    return 0;

  freenect_device_attributes* attrs{};
  const int n = freenect_list_device_attributes(g_ctx, &attrs);
  if(n < 0)
    return 0;

  int i = 0;
  for(auto* a = attrs; a; a = a->next, i++)
  {
    EnumEntry e;
    e.serial = a->camera_serial ? a->camera_serial : "";
    e.uri = e.serial.empty() ? ("freenect:index:" + std::to_string(i))
                             : ("freenect:sn:" + e.serial);

    depthcam_device_info info{};
    info.backend = "freenect";
    info.uri = e.uri.c_str();
    info.name = "Kinect v1";
    info.serial = e.serial.c_str();
    info.transport = "usb2";
    // No infrared: libfreenect can stream IR, but only instead of colour, not
    // alongside it, and the ABI has no way to express that exclusivity.
    info.streams = DEPTHCAM_STREAM_COLOR | DEPTHCAM_STREAM_DEPTH
                   | DEPTHCAM_STREAM_POINTCLOUD;
    cb(&info, user);
  }

  if(attrs)
    freenect_free_device_attributes(attrs);
  return 1;
}

void backend_set_changed_callback(depthcam_changed_cb, void*)
{
  // libfreenect has no hot-plug notification.
}

depthcam_device* backend_open(const char* uri, const depthcam_open_config* config)
{
  if(!g_ctx || !config)
    return nullptr;

  const auto addr = parse_uri(uri);

  auto dev = std::make_unique<depthcam_device>();
  dev->cfg = *config;
  dev->color_pointcloud = (config->streams & DEPTHCAM_STREAM_POINTCLOUD)
                          && config->color_pointcloud != 0;

  int rc = -1;
  switch(addr.kind)
  {
    case Address::Serial:
      rc = freenect_open_device_by_camera_serial(g_ctx, &dev->dev, addr.serial.c_str());
      if(rc < 0)
      {
        // No silent fallback to another camera.
        set_error("Kinect v1 '" + addr.serial + "' is not connected");
        return nullptr;
      }
      break;
    case Address::Index:
      rc = freenect_open_device(g_ctx, &dev->dev, addr.index);
      break;
    case Address::Any:
      rc = freenect_open_device(g_ctx, &dev->dev, 0);
      break;
  }

  if(rc < 0 || !dev->dev)
  {
    set_error("could not open the Kinect v1");
    return nullptr;
  }

  freenect_set_user(dev->dev, dev.get());

  const auto video = freenect_find_video_mode(FREENECT_RESOLUTION_MEDIUM,
                                              FREENECT_VIDEO_RGB);
  if(freenect_set_video_mode(dev->dev, video) < 0)
  {
    set_error("could not set the Kinect v1 video mode");
    return nullptr;
  }
  dev->color_w = video.width;
  dev->color_h = video.height;

  // REGISTERED maps depth onto the colour camera, which is what makes a
  // coloured point cloud possible at all here -- libfreenect has no
  // colour-to-depth direction, so both alignment modes use it and only the
  // uncoloured/unaligned case takes raw millimetres.
  const bool want_registered
      = config->align != DEPTHCAM_ALIGN_NONE || dev->color_pointcloud;
  const auto depth = freenect_find_depth_mode(
      FREENECT_RESOLUTION_MEDIUM,
      want_registered ? FREENECT_DEPTH_REGISTERED : FREENECT_DEPTH_MM);
  if(freenect_set_depth_mode(dev->dev, depth) < 0)
  {
    set_error("could not set the Kinect v1 depth mode");
    return nullptr;
  }
  dev->depth_w = depth.width;
  dev->depth_h = depth.height;

  freenect_set_video_callback(dev->dev, &video_cb);
  freenect_set_depth_callback(dev->dev, &depth_cb);

  return dev.release();
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
                          || (dev->cfg.streams & DEPTHCAM_STREAM_POINTCLOUD);

  if(want_color && freenect_start_video(dev->dev) < 0)
  {
    set_error("could not start the Kinect v1 video stream");
    dev->running.store(false, std::memory_order_release);
    return 0;
  }
  if(want_depth && freenect_start_depth(dev->dev) < 0)
  {
    set_error("could not start the Kinect v1 depth stream");
    dev->running.store(false, std::memory_order_release);
    return 0;
  }

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
  if(dev->dev)
  {
    freenect_stop_video(dev->dev);
    freenect_stop_depth(dev->dev);
  }
}

const depthcam_backend_v1 g_backend{
    .abi_version = DEPTHCAM_ABI_VERSION,
    .name = "freenect",
    .display_name = "Kinect v1",
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
