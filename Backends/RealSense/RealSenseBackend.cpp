/*
 * Intel RealSense backend: librealsense2 behind depthcam_abi.h.
 *
 * The closest fit of any SDK here: frames are refcounted (so zero-copy), the
 * pipeline is push-based (so no thread of our own), rs2::align maps one-to-one
 * onto our alignment modes, and vertices are already float metres.
 */
#include <depthcam_abi.h>

#include <librealsense2/rs.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace
{

/// The device browser groups by vendor already, so the model alone is enough
/// and keeps the name short. Case-insensitive: the SDKs are not consistent.
std::string strip_vendor(std::string name, std::string_view prefix)
{
  if(name.size() > prefix.size()
     && std::equal(
         prefix.begin(), prefix.end(), name.begin(), [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a))
           == std::tolower(static_cast<unsigned char>(b));
  }))
    name.erase(0, prefix.size());
  return name;
}
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

std::unique_ptr<rs2::context> g_context;
depthcam_changed_cb g_changed_cb = nullptr;
void* g_changed_user = nullptr;

int to_depthcam_format(rs2_format f)
{
  switch(f)
  {
    case RS2_FORMAT_RGB8:
      return DEPTHCAM_FMT_RGB24;
    case RS2_FORMAT_BGR8:
      return DEPTHCAM_FMT_BGR24;
    case RS2_FORMAT_RGBA8:
      return DEPTHCAM_FMT_RGBA;
    case RS2_FORMAT_BGRA8:
      return DEPTHCAM_FMT_BGRA;
    case RS2_FORMAT_YUYV:
      return DEPTHCAM_FMT_YUYV422;
    case RS2_FORMAT_UYVY:
      return DEPTHCAM_FMT_UYVY422;
    case RS2_FORMAT_Y8:
      return DEPTHCAM_FMT_GRAY8;
    case RS2_FORMAT_Y16:
    case RS2_FORMAT_Z16:
      return DEPTHCAM_FMT_GRAY16;
    case RS2_FORMAT_MJPEG:
      return DEPTHCAM_FMT_MJPEG;
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
  int index{0};
};

Address parse_uri(const char* uri)
{
  Address a;
  if(!uri)
    return a;
  std::string s{uri};
  if(s.rfind("realsense:", 0) == 0)
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

std::string info_of(const rs2::device& d, rs2_camera_info i)
{
  try
  {
    return d.supports(i) ? d.get_info(i) : std::string{};
  }
  catch(...)
  {
    return {};
  }
}

struct EnumEntry
{
  std::string uri, name, serial, transport;
};
// --- controls ---------------------------------------------------------------

/**
 * @brief Turn RS2_OPTION_ENABLE_AUTO_EXPOSURE into "auto_exposure".
 *
 * rs2_option_to_string gives "Enable Auto Exposure": human-readable, but not an
 * address. The enum spelling is the stable one, so that is what is used.
 */
std::string option_slug(rs2_option opt)
{
  std::string n = rs2_option_to_string(opt);
  for(auto& c : n)
    c = (c == ' ' || c == '-' || c == '/') ? '_'
                                           : char(std::tolower((unsigned char)c));
  return n;
}

/**
 * @brief Which group a sensor's options land in.
 *
 * librealsense names its sensors after the hardware block -- "Stereo Module",
 * "RGB Camera", "Motion Module" -- and every option belongs to exactly one, so
 * the sensor is the natural category. Anything unrecognised keeps its own name
 * rather than being lumped together, because a camera we have never seen is
 * exactly the case where guessing is wrong.
 */
std::string sensor_group(const std::string& name)
{
  std::string n = name;
  for(auto& c : n)
    c = (c == ' ' || c == '-') ? '_' : char(std::tolower((unsigned char)c));

  if(n.find("stereo") != std::string::npos || n.find("depth") != std::string::npos)
    return "depth";
  if(n.find("rgb") != std::string::npos || n.find("color") != std::string::npos)
    return "color";
  if(n.find("motion") != std::string::npos)
    return "imu";
  if(n.find("safety") != std::string::npos)
    return "safety";
  return n.empty() ? "device" : n;
}

/// One option, plus the sensor it has to be addressed through.
struct ControlEntry
{
  rs2::sensor sensor;
  rs2_option option{};
  std::string path;
  std::string name;
  std::string description;
  std::vector<std::string> labels;
  std::vector<const char*> label_ptrs;
  depthcam_control desc{};
};

} // namespace

// ---------------------------------------------------------------------------

struct depthcam_device
{
  rs2::pipeline pipeline;
  rs2::config config;
  std::unique_ptr<rs2::align> align;
  rs2::pointcloud pointcloud;

  /// Kept only for the controls: an rs2::device is a handle, so holding it
  /// alongside the pipeline costs nothing and does not claim the camera twice.
  rs2::device rsdev;

  depthcam_open_config cfg{};
  std::string serial;
  bool color_pointcloud{};
  bool want_imu{};
  std::atomic_bool running{};
  float depth_scale_mm{1.f};

  depthcam_frame_cb on_frame{};
  void* user{};

  std::vector<float> cloud_buf;

  std::vector<ControlEntry> controls;
  bool controls_scanned{};
  std::mutex control_lock;

  void scanControls();
  ControlEntry* findControl(const char* id);

  void handle(const rs2::frameset& fs);
  void handleMotion(const rs2::motion_frame& f);
  void emitImage(uint32_t stream, const rs2::video_frame& f, float depth_unit);
  void emitPointCloud(const rs2::frameset& fs);
};

namespace
{
/// Keeps a librealsense frame alive for exactly as long as the host holds its
/// buffer. rs2::frame is refcounted, so this is a reference, not a copy.
struct FrameHolder
{
  rs2::frame frame;
};

void release_frame_holder(void* owner)
{
  delete static_cast<FrameHolder*>(owner);
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

void depthcam_device::emitImage(
    uint32_t stream, const rs2::video_frame& f, float depth_unit)
{
  if(!f || !on_frame)
    return;

  const int format = to_depthcam_format(f.get_profile().format());
  if(format == DEPTHCAM_FMT_NONE)
    return;

  auto* holder = new FrameHolder{f};

  depthcam_frame out{};
  out.stream = stream;
  out.format = format;
  out.width = f.get_width();
  out.height = f.get_height();
  out.stride = f.get_stride_in_bytes();
  out.timestamp_ns = uint64_t(f.get_timestamp() * 1e6); // ms -> ns
  out.data = f.get_data();
  out.bytes = size_t(f.get_data_size());
  out.depth_unit_mm = depth_unit;
  out.owner = holder;
  out.release = &release_frame_holder;

  on_frame(&out, user);
}

void depthcam_device::emitPointCloud(const rs2::frameset& fs)
{
  if(!on_frame)
    return;

  auto depth = fs.get_depth_frame();
  if(!depth)
    return;

  rs2::video_frame color = fs.get_color_frame();
  if(color_pointcloud && !color)
    return;

  try
  {
    if(color_pointcloud)
      pointcloud.map_to(color);

    rs2::points pts = pointcloud.calculate(depth);
    if(!pts)
      return;

    const size_t n = pts.size();
    if(n == 0)
      return;

    const auto* verts = pts.get_vertices();
    if(!verts)
      return;

    // Uncoloured: hand librealsense's own buffer over untouched. rs2::vertex is
    // three floats in metres, which is exactly DEPTHCAM_FMT_XYZ.
    if(!color_pointcloud)
    {
      static_assert(sizeof(rs2::vertex) == 3 * sizeof(float));

      auto* holder = new FrameHolder{pts};

      depthcam_frame out{};
      out.stream = DEPTHCAM_STREAM_POINTCLOUD;
      out.format = DEPTHCAM_FMT_XYZ;
      out.point_count = int32_t(n);
      out.timestamp_ns = uint64_t(depth.get_timestamp() * 1e6);
      out.data = verts;
      out.bytes = n * sizeof(rs2::vertex);
      out.owner = holder;
      out.release = &release_frame_holder;

      on_frame(&out, user);
      return;
    }

    // Coloured: librealsense gives texture coordinates rather than per-point
    // colour, so the interleave has to be built here.
    const auto* uvs = pts.get_texture_coordinates();
    if(!uvs)
      return;

    const int cw = color.get_width();
    const int ch = color.get_height();
    const int cstride = color.get_stride_in_bytes();
    const int cbpp = color.get_bytes_per_pixel();
    const auto* cdata = static_cast<const uint8_t*>(color.get_data());
    if(!cdata || cbpp < 3)
      return;

    const bool bgr = color.get_profile().format() == RS2_FORMAT_BGR8
                     || color.get_profile().format() == RS2_FORMAT_BGRA8;

    cloud_buf.clear();
    cloud_buf.reserve(n * 6);

    for(size_t i = 0; i < n; i++)
    {
      // z == 0 means no reading at that pixel.
      if(verts[i].z == 0.f)
        continue;

      const int u = int(uvs[i].u * cw + 0.5f);
      const int v = int(uvs[i].v * ch + 0.5f);
      if(u < 0 || v < 0 || u >= cw || v >= ch)
        continue;

      cloud_buf.push_back(verts[i].x);
      cloud_buf.push_back(verts[i].y);
      cloud_buf.push_back(verts[i].z);

      const uint8_t* p = cdata + size_t(v) * cstride + size_t(u) * cbpp;
      if(bgr)
      {
        cloud_buf.push_back(p[2] / 255.f);
        cloud_buf.push_back(p[1] / 255.f);
        cloud_buf.push_back(p[0] / 255.f);
      }
      else
      {
        cloud_buf.push_back(p[0] / 255.f);
        cloud_buf.push_back(p[1] / 255.f);
        cloud_buf.push_back(p[2] / 255.f);
      }
    }

    if(cloud_buf.empty())
      return;

    auto* holder = new VectorHolder{cloud_buf};

    depthcam_frame out{};
    out.stream = DEPTHCAM_STREAM_POINTCLOUD;
    out.format = DEPTHCAM_FMT_XYZRGB;
    out.point_count = int32_t(holder->data.size() / 6);
    out.timestamp_ns = uint64_t(depth.get_timestamp() * 1e6);
    out.data = holder->data.data();
    out.bytes = holder->data.size() * sizeof(float);
    out.owner = holder;
    out.release = &release_vector_holder;

    on_frame(&out, user);
  }
  catch(const rs2::error& e)
  {
    set_error(std::string{"point cloud: "} + e.what());
  }
  catch(...)
  {
  }
}

void depthcam_device::scanControls()
{
  if(controls_scanned)
    return;
  controls_scanned = true;

  std::vector<rs2::sensor> sensors;
  try
  {
    sensors = rsdev.query_sensors();
  }
  catch(...)
  {
    return;
  }

  for(auto& sensor : sensors)
  {
    std::string sname;
    try
    {
      if(sensor.supports(RS2_CAMERA_INFO_NAME))
        sname = sensor.get_info(RS2_CAMERA_INFO_NAME);
    }
    catch(...)
    {
    }
    const auto group = sensor_group(sname);

    std::vector<rs2_option> options;
    try
    {
      options = sensor.get_supported_options();
    }
    catch(...)
    {
      continue;
    }

    for(auto opt : options)
    {
      ControlEntry e;
      e.sensor = sensor;
      e.option = opt;
      e.name = rs2_option_to_string(opt);

      rs2::option_range range{};
      bool read_only = false;
      try
      {
        if(!sensor.supports(opt))
          continue;
        range = sensor.get_option_range(opt);
        read_only = sensor.is_option_read_only(opt);
        if(sensor.supports(opt))
          e.description = sensor.get_option_description(opt);
      }
      catch(...)
      {
        // An option the sensor lists but refuses to describe is not usable.
        continue;
      }

      // Read-only options are temperatures, frame counters and the settings an
      // `auto` mode chose for itself: worth watching, never worth a slider next
      // to the ones that do something. The sensor name stays in the leaf there,
      // because two sensors each report their own temperature.
      e.path = read_only ? "sensors/" + group + "_" + option_slug(opt)
                         : group + "/" + option_slug(opt);

      if(std::any_of(controls.begin(), controls.end(), [&](const ControlEntry& o) {
           return o.path == e.path;
         }))
        continue;

      auto& d = e.desc;
      d.access = DEPTHCAM_ACCESS_READ | (read_only ? 0 : DEPTHCAM_ACCESS_WRITE);
      d.min = range.min;
      d.max = range.max;
      d.step = range.step;
      d.def = range.def;

      // Every librealsense option is a float over the wire; the range is what
      // says whether it is really a switch, a menu or a number.
      const bool integral = range.step >= 1.f
                            && range.min == std::floor(range.min)
                            && range.max == std::floor(range.max);

      if(integral && range.min == 0.f && range.max == 1.f && range.step == 1.f)
      {
        d.kind = DEPTHCAM_CONTROL_BOOL;
      }
      else if(integral && (range.max - range.min) <= 32.f)
      {
        // Values with names are a menu. get_option_value_description returns
        // null for anything that is just a number, so this only fires for the
        // handful that really are enumerations (visual preset, sequence id).
        bool named = false;
        for(float v = range.min; v <= range.max; v += range.step)
        {
          const char* label = nullptr;
          try
          {
            label = sensor.get_option_value_description(opt, v);
          }
          catch(...)
          {
          }
          e.labels.push_back(label ? label : std::to_string(int(v)));
          named = named || (label != nullptr);
        }
        if(named)
        {
          d.kind = DEPTHCAM_CONTROL_ENUM;
        }
        else
        {
          e.labels.clear();
          d.kind = DEPTHCAM_CONTROL_INT;
        }
      }
      else
      {
        d.kind = integral ? DEPTHCAM_CONTROL_INT : DEPTHCAM_CONTROL_FLOAT;
      }

      controls.push_back(std::move(e));
    }
  }

  // Point the descriptors at their owner's stable storage, after the vector has
  // stopped reallocating.
  for(auto& e : controls)
  {
    e.label_ptrs.clear();
    e.label_ptrs.reserve(e.labels.size());
    for(const auto& l : e.labels)
      e.label_ptrs.push_back(l.c_str());

    e.desc.id = e.path.c_str();
    e.desc.name = e.name.c_str();
    e.desc.description = e.description.empty() ? nullptr : e.description.c_str();
    e.desc.enum_labels = e.label_ptrs.empty() ? nullptr : e.label_ptrs.data();
    e.desc.enum_count = int32_t(e.label_ptrs.size());
  }
}

ControlEntry* depthcam_device::findControl(const char* id)
{
  if(!id)
    return nullptr;
  for(auto& e : controls)
    if(e.path == id)
      return &e;
  return nullptr;
}

void depthcam_device::handleMotion(const rs2::motion_frame& f)
{
  if(!running.load(std::memory_order_acquire) || !on_frame)
    return;

  const auto stream = f.get_profile().stream_type();
  const auto v = f.get_motion_data();

  depthcam_imu_sample sample{};
  if(stream == RS2_STREAM_ACCEL)
  {
    // librealsense reports metres per second squared, which the ABI wants too.
    sample.accel[0] = v.x;
    sample.accel[1] = v.y;
    sample.accel[2] = v.z;
    sample.fields = DEPTHCAM_IMU_ACCEL;
  }
  else if(stream == RS2_STREAM_GYRO)
  {
    // Radians per second, likewise.
    sample.gyro[0] = v.x;
    sample.gyro[1] = v.y;
    sample.gyro[2] = v.z;
    sample.fields = DEPTHCAM_IMU_GYRO;
  }
  else
  {
    return;
  }

  // One field per sample, deliberately: the D435i's accelerometer and gyroscope
  // run at different rates (63Hz and 200Hz by default) and pairing them would
  // mean either holding the fast one back or repeating the slow one.
  depthcam_frame out{};
  out.stream = DEPTHCAM_STREAM_IMU;
  out.format = DEPTHCAM_FMT_IMU;
  out.timestamp_ns = uint64_t(f.get_timestamp() * 1e6);
  out.data = &sample;
  out.bytes = sizeof(sample);
  on_frame(&out, user);
}

void depthcam_device::handle(const rs2::frameset& frames)
{
  if(!running.load(std::memory_order_acquire))
    return;

  rs2::frameset fs = frames;
  if(align)
    fs = align->process(fs);

  // Each stream independently: a frameset without colour must still yield
  // depth, IR and the point cloud.
  if(cfg.streams & DEPTHCAM_STREAM_COLOR)
    if(auto c = fs.get_color_frame())
      emitImage(DEPTHCAM_STREAM_COLOR, c, 0.f);

  if(cfg.streams & DEPTHCAM_STREAM_IR)
    if(auto ir = fs.get_infrared_frame())
      emitImage(DEPTHCAM_STREAM_IR, ir, 0.f);

  if(cfg.streams & DEPTHCAM_STREAM_DEPTH)
    if(auto d = fs.get_depth_frame())
      emitImage(DEPTHCAM_STREAM_DEPTH, d, depth_scale_mm);

  if(cfg.streams & DEPTHCAM_STREAM_POINTCLOUD)
    emitPointCloud(fs);
}

// ---------------------------------------------------------------------------

namespace
{

int backend_init(const char*)
{
  try
  {
    g_context = std::make_unique<rs2::context>();

    g_context->set_devices_changed_callback(
        [](rs2::event_information&) {
      if(g_changed_cb)
        g_changed_cb(g_changed_user);
    });
    return 1;
  }
  catch(const rs2::error& e)
  {
    set_error(e.what());
    return 0;
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
  g_changed_cb = nullptr;
  g_changed_user = nullptr;
  g_context.reset();
}

int backend_enumerate(depthcam_enumerate_cb cb, void* user)
{
  if(!g_context || !cb)
    return 0;

  try
  {
    // Metadata only; no pipeline is started.
    for(auto&& d : g_context->query_devices())
    {
      EnumEntry e;
      e.serial = info_of(d, RS2_CAMERA_INFO_SERIAL_NUMBER);
      e.name = strip_vendor(info_of(d, RS2_CAMERA_INFO_NAME), "Intel RealSense ");
      e.transport = info_of(d, RS2_CAMERA_INFO_USB_TYPE_DESCRIPTOR);
      if(e.name.empty())
        e.name = "RealSense";
      if(!e.transport.empty())
        e.transport = "usb" + e.transport;

      e.uri = e.serial.empty() ? std::string{"realsense:index:0"}
                               : ("realsense:sn:" + e.serial);

      depthcam_device_info info{};
      info.backend = "realsense";
      info.uri = e.uri.c_str();
      info.name = e.name.c_str();
      info.serial = e.serial.c_str();
      info.transport = e.transport.c_str();
      info.streams = DEPTHCAM_STREAM_COLOR | DEPTHCAM_STREAM_IR
                     | DEPTHCAM_STREAM_DEPTH | DEPTHCAM_STREAM_POINTCLOUD
                     | DEPTHCAM_STREAM_IMU;
      cb(&info, user);
    }
    return 1;
  }
  catch(const rs2::error& e)
  {
    set_error(e.what());
    return 0;
  }
  catch(...)
  {
    return 0;
  }
}

void backend_set_changed_callback(depthcam_changed_cb cb, void* user)
{
  g_changed_cb = cb;
  g_changed_user = user;
}

int backend_list_controls(
    depthcam_device* dev, depthcam_control_cb cb, void* user)
{
  if(!dev || !cb)
    return 0;
  std::lock_guard lock{dev->control_lock};
  dev->scanControls();
  for(const auto& e : dev->controls)
    cb(&e.desc, user);
  return 1;
}

int backend_get_control(depthcam_device* dev, const char* id, double* out)
{
  if(!dev || !out)
    return 0;
  std::lock_guard lock{dev->control_lock};
  auto* e = dev->findControl(id);
  if(!e || !(e->desc.access & DEPTHCAM_ACCESS_READ))
    return 0;
  try
  {
    *out = double(e->sensor.get_option(e->option));
    return 1;
  }
  catch(const std::exception& ex)
  {
    set_error(ex.what());
    return 0;
  }
  catch(...)
  {
    return 0;
  }
}

int backend_set_control(depthcam_device* dev, const char* id, double value)
{
  if(!dev)
    return 0;
  std::lock_guard lock{dev->control_lock};
  auto* e = dev->findControl(id);
  if(!e || !(e->desc.access & DEPTHCAM_ACCESS_WRITE))
    return 0;
  try
  {
    e->sensor.set_option(e->option, float(value));
    return 1;
  }
  catch(const std::exception& ex)
  {
    // Routine rather than exceptional: several options are only writable while
    // their `auto` counterpart is off, and librealsense reports that by
    // throwing.
    set_error(ex.what());
    return 0;
  }
  catch(...)
  {
    return 0;
  }
}

depthcam_device* backend_open(const char* uri, const depthcam_open_config* config)
{
  if(!g_context || !config)
    return nullptr;

  try
  {
    const auto addr = parse_uri(uri);

    auto devices = g_context->query_devices();
    if(devices.size() == 0)
    {
      set_error("no RealSense device connected");
      return nullptr;
    }

    std::string serial;
    switch(addr.kind)
    {
      case Address::Serial: {
        for(auto&& d : devices)
          if(info_of(d, RS2_CAMERA_INFO_SERIAL_NUMBER) == addr.serial)
            serial = addr.serial;
        if(serial.empty())
        {
          // No silent fallback to another camera.
          set_error("RealSense '" + addr.serial + "' is not connected");
          return nullptr;
        }
        break;
      }
      case Address::Index:
        if(addr.index < 0 || addr.index >= int(devices.size()))
        {
          set_error("no RealSense at index " + std::to_string(addr.index));
          return nullptr;
        }
        serial = info_of(devices[addr.index], RS2_CAMERA_INFO_SERIAL_NUMBER);
        break;
      case Address::Any:
        serial = info_of(devices[0], RS2_CAMERA_INFO_SERIAL_NUMBER);
        break;
    }

    auto dev = std::make_unique<depthcam_device>();
    dev->cfg = *config;

    // Held for the controls; see depthcam_device::rsdev.
    for(auto&& d : devices)
      if(info_of(d, RS2_CAMERA_INFO_SERIAL_NUMBER) == serial)
        dev->rsdev = d;

    const bool want_cloud = (config->streams & DEPTHCAM_STREAM_POINTCLOUD) != 0;
    dev->color_pointcloud = want_cloud && config->color_pointcloud != 0
                            && config->align != DEPTHCAM_ALIGN_NONE;

    dev->serial = serial;
    if(!serial.empty())
      dev->config.enable_device(serial);

    const bool need_color
        = (config->streams & DEPTHCAM_STREAM_COLOR) || dev->color_pointcloud;
    const bool need_depth = (config->streams & DEPTHCAM_STREAM_DEPTH) || want_cloud;

    // 0 means "let the backend choose"; librealsense reads 0 the same way.
    if(need_color)
      dev->config.enable_stream(
          RS2_STREAM_COLOR, config->color_width, config->color_height,
          RS2_FORMAT_RGB8, config->color_fps);

    if(need_depth)
      dev->config.enable_stream(
          RS2_STREAM_DEPTH, config->depth_width, config->depth_height,
          RS2_FORMAT_Z16, config->depth_fps);

    if(config->streams & DEPTHCAM_STREAM_IR)
    {
      // Index 1 is the left imager. 0 for the dimensions means "any", which is
      // how librealsense reads it too.
      dev->config.enable_stream(
          RS2_STREAM_INFRARED, 1, config->ir_width, config->ir_height,
          RS2_FORMAT_Y8, config->ir_fps);
    }

    if(config->streams & DEPTHCAM_STREAM_IMU)
    {
      // Same pipeline as the video, unlike the Orbbec backend: librealsense
      // delivers motion frames on their own rather than folding them into a
      // frameset, so the IMU keeps its own rate without a second pipeline.
      //
      // Tolerated rather than required -- a D435 has no IMU where a D435i does,
      // and the two are told apart only by trying.
      try
      {
        dev->config.enable_stream(RS2_STREAM_ACCEL, RS2_FORMAT_MOTION_XYZ32F);
        dev->config.enable_stream(RS2_STREAM_GYRO, RS2_FORMAT_MOTION_XYZ32F);
        dev->want_imu = true;
      }
      catch(const std::exception& e)
      {
        set_error(std::string{"no IMU on this camera: "} + e.what());
      }
    }

    switch(config->align)
    {
      case DEPTHCAM_ALIGN_DEPTH_TO_COLOR:
        dev->align = std::make_unique<rs2::align>(RS2_STREAM_COLOR);
        break;
      case DEPTHCAM_ALIGN_COLOR_TO_DEPTH:
        dev->align = std::make_unique<rs2::align>(RS2_STREAM_DEPTH);
        break;
      default:
        break;
    }

    return dev.release();
  }
  catch(const rs2::error& e)
  {
    set_error(e.what());
    return nullptr;
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
  if(!dev)
    return 0;
  if(dev->running.load(std::memory_order_acquire))
    return 1;

  dev->on_frame = on_frame;
  dev->user = user;
  dev->running.store(true, std::memory_order_release);

  const auto callback = [dev](const rs2::frame& f) {
    try
    {
      if(auto fs = f.as<rs2::frameset>())
        dev->handle(fs);
      else if(auto mf = f.as<rs2::motion_frame>())
        dev->handleMotion(mf);
    }
    catch(...)
    {
      // Never let anything escape into librealsense's caller.
    }
  };

  try
  {
    rs2::pipeline_profile profile;
    try
    {
      profile = dev->pipeline.start(dev->config, callback);
    }
    catch(const rs2::error& e)
    {
      // The requested combination is unavailable. This is routine on USB2,
      // where a D400 drops to a handful of low-rate profiles (a D435i on
      // USB 2.1 offers colour only at 1080p@8 or 720p@15, and no 30fps colour
      // at all), so the defaults a USB3 camera would accept simply do not
      // exist. Let librealsense choose rather than failing outright.
      set_error(
          std::string{"requested stream configuration unavailable ("} + e.what()
          + "); falling back to the camera's own defaults");

      rs2::config fallback;
      if(!dev->serial.empty())
        fallback.enable_device(dev->serial);
      profile = dev->pipeline.start(fallback, callback);
    }

    // get_depth_scale() is metres per unit; the ABI wants millimetres per unit.
    try
    {
      auto ds = profile.get_device().first<rs2::depth_sensor>();
      dev->depth_scale_mm = ds.get_depth_scale() * 1000.f;
    }
    catch(...)
    {
      dev->depth_scale_mm = 1.f;
    }

    return 1;
  }
  catch(const rs2::error& e)
  {
    set_error(e.what());
    dev->running.store(false, std::memory_order_release);
    return 0;
  }
  catch(const std::exception& e)
  {
    set_error(e.what());
    dev->running.store(false, std::memory_order_release);
    return 0;
  }
}

void backend_stop(depthcam_device* dev)
{
  if(!dev)
    return;
  if(!dev->running.exchange(false, std::memory_order_acq_rel))
    return;
  try
  {
    dev->pipeline.stop();
  }
  catch(...)
  {
  }
}

uint32_t backend_active_streams(depthcam_device* dev)
{
  if(!dev)
    return 0;
  uint32_t streams = dev->cfg.streams;
  // A D435 has no IMU where a D435i does, and enable_stream is where that
  // shows up.
  if(!dev->want_imu)
    streams &= ~uint32_t(DEPTHCAM_STREAM_IMU);
  return streams;
}

const depthcam_backend_v1 g_backend{
    .abi_version = DEPTHCAM_ABI_VERSION,
    .name = "realsense",
    .display_name = "Intel RealSense",
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
    .active_streams = &backend_active_streams,
};

} // namespace

extern "C" DEPTHCAM_EXPORT const depthcam_backend_v1* score_depthcam_backend_v1(void)
{
  return &g_backend;
}
