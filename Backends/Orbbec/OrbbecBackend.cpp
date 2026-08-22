/*
 * Orbbec backend: OrbbecSDK v2 behind depthcam_abi.h.
 *
 * Everything here was verified against a Femto Mega; the non-obvious parts are
 * commented where they cost time to find.
 */
#include <depthcam_abi.h>

#include <cstdlib>

#include <libobsensor/ObSensor.hpp>

#include <atomic>
#include <cctype>
#include <cstring>
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

// ---------------------------------------------------------------------------

depthcam_format to_depthcam_format(OBFormat fmt)
{
  switch(fmt)
  {
    case OB_FORMAT_YUYV:
    case OB_FORMAT_YUY2:
    // ObTypes.h documents OB_FORMAT_GRAY as "the actual format is the same as
    // YUYV" despite the name.
    case OB_FORMAT_GRAY:
      return DEPTHCAM_FMT_YUYV422;
    case OB_FORMAT_UYVY:
      return DEPTHCAM_FMT_UYVY422;
    case OB_FORMAT_NV12:
      return DEPTHCAM_FMT_NV12;
    case OB_FORMAT_I420:
    // YV12 is planar YUV 4:2:0 with the chroma planes swapped, not a grey
    // format; the plane order is a layout detail the host handles.
    case OB_FORMAT_YV12:
      return DEPTHCAM_FMT_YUV420P;

    case OB_FORMAT_RGB:
      return DEPTHCAM_FMT_RGB24;
    case OB_FORMAT_BGR:
      return DEPTHCAM_FMT_BGR24;
    case OB_FORMAT_RGBA:
      return DEPTHCAM_FMT_RGBA;
    case OB_FORMAT_BGRA:
      return DEPTHCAM_FMT_BGRA;

    // BA81 is documented by Orbbec as "same as Y8, used for the right IR
    // stream", despite the name matching a Bayer fourcc.
    case OB_FORMAT_Y8:
    case OB_FORMAT_BA81:
      return DEPTHCAM_FMT_GRAY8;

    // The SDK unpacks Y10/Y11/Y12/Y14 into 16-bit containers.
    case OB_FORMAT_Y16:
    case OB_FORMAT_Y10:
    case OB_FORMAT_Y11:
    case OB_FORMAT_Y12:
    case OB_FORMAT_Y14:
    case OB_FORMAT_Z16:
    case OB_FORMAT_RLE:
    case OB_FORMAT_RVL:
    case OB_FORMAT_RW16:
      return DEPTHCAM_FMT_GRAY16;

    case OB_FORMAT_MJPG:
      return DEPTHCAM_FMT_MJPEG;
    case OB_FORMAT_H264:
      return DEPTHCAM_FMT_H264;
    case OB_FORMAT_H265:
    case OB_FORMAT_HEVC:
      return DEPTHCAM_FMT_H265;

    case OB_FORMAT_POINT:
      return DEPTHCAM_FMT_XYZ;
    case OB_FORMAT_RGB_POINT:
      return DEPTHCAM_FMT_XYZRGB;

    default:
      return DEPTHCAM_FMT_NONE;
  }
}

/**
 * Address forms accepted in the URI, mirroring what the settings widget offers:
 *   orbbec:sn:SERIAL   orbbec:uid:UID   orbbec:net:HOST[:PORT]   orbbec:
 */
struct Address
{
  enum Kind
  {
    Any,
    Serial,
    Uid,
    Network
  } kind{Any};
  std::string value;
  uint16_t port{8090};
};

Address parse_uri(const char* uri)
{
  Address a;
  if(!uri)
    return a;

  std::string s{uri};
  if(s.rfind("orbbec:", 0) == 0)
    s = s.substr(7);
  if(s.empty())
    return a;

  const auto starts = [&](const char* p) { return s.rfind(p, 0) == 0; };

  if(starts("sn:"))
  {
    a.kind = Address::Serial;
    a.value = s.substr(3);
  }
  else if(starts("uid:"))
  {
    a.kind = Address::Uid;
    a.value = s.substr(4);
  }
  else if(starts("net:"))
  {
    a.kind = Address::Network;
    a.value = s.substr(4);

    // Split on the last colon only, and only if a number follows, so a bare
    // IPv6 literal is not mangled.
    if(const auto colon = a.value.rfind(':'); colon != std::string::npos && colon > 0)
    {
      const auto tail = a.value.substr(colon + 1);
      if(!tail.empty()
         && tail.find_first_not_of("0123456789") == std::string::npos)
      {
        const auto p = std::stoul(tail);
        if(p > 0 && p <= 65535)
        {
          a.port = static_cast<uint16_t>(p);
          a.value = a.value.substr(0, colon);
        }
      }
    }
  }
  else
  {
    a.kind = Address::Serial;
    a.value = s;
  }

  if(a.value.empty())
    a.kind = Address::Any;
  return a;
}

// ---------------------------------------------------------------------------

std::unique_ptr<ob::Context> g_context;
depthcam_changed_cb g_changed_cb = nullptr;
void* g_changed_user = nullptr;

/// Strings handed to the host must outlive the enumerate callback, so they are
/// parked here rather than in a temporary.
struct EnumEntry
{
  std::string uri, name, serial, transport;
};

/**
 * The colour and depth resolution a coloured point cloud must not be given.
 *
 * The SDK's transformationDepthToRGBDPointCloud indexes the colour buffer as
 * i * colorWidth * scale with no bounds check at all; when colour and depth
 * differ it walks straight off the end. Equal dimensions are the only case
 * where its arithmetic stays in range.
 */
bool color_matches_depth(ob::FrameSet& fs)
{
  auto c = fs.colorFrame();
  auto d = fs.depthFrame();
  if(!c || !d)
    return false;
  auto cv = c->as<ob::VideoFrame>();
  auto dv = d->as<ob::VideoFrame>();
  if(!cv || !dv)
    return false;
  return cv->getWidth() == dv->getWidth() && cv->getHeight() == dv->getHeight();
}

constexpr int default_raw_color_width = 1920;
constexpr int default_raw_color_height = 1080;

// --- controls ---------------------------------------------------------------

/**
 * @brief Turn OB_PROP_COLOR_AUTO_EXPOSURE_BOOL into "color/auto_exposure".
 *
 * The SDK has no category or "advanced" metadata -- OBPropertyItem carries only
 * id, name, type and permission -- but the names are systematic and the
 * permission is genuinely informative, so a useful tree falls out of the two:
 *
 *   permission -w  a trigger with no readable value: reboot, recovery mode,
 *                  forced capture. Grouped under advanced/ so they are not one
 *                  click away from a performer mid-performance.
 *   permission r-  a sensor: temperature, power state, the exposure the camera
 *                  chose for itself. Grouped under sensors/.
 *   otherwise      grouped by the first token of the name.
 */
std::string control_id_for(const std::string& raw_name, OBPermissionType perm)
{
  std::string n = raw_name;

  // Strip the type prefix and the type suffix the SDK encodes in the name.
  for(const char* p : {"OB_PROP_", "OB_STRUCT_", "OB_RAW_DATA_"})
    if(n.rfind(p, 0) == 0)
    {
      n = n.substr(std::strlen(p));
      break;
    }
  for(const char* suffix : {"_BOOL", "_INT", "_FLOAT"})
  {
    const auto len = std::strlen(suffix);
    if(n.size() > len && n.compare(n.size() - len, len, suffix) == 0)
    {
      n.resize(n.size() - len);
      break;
    }
  }

  for(auto& c : n)
    c = char(std::tolower((unsigned char)c));

  if(n.empty())
    return {};

  const auto starts = [&](const char* p) { return n.rfind(p, 0) == 0; };

  if(perm == OB_PERMISSION_WRITE)
    return "advanced/" + n;
  if(perm == OB_PERMISSION_READ)
    return "sensors/" + n;

  if(starts("color_"))
    return "color/" + n.substr(6);
  if(starts("depth_"))
    return "depth/" + n.substr(6);
  if(starts("ir_"))
    return "ir/" + n.substr(3);
  if(starts("tof_"))
    return "depth/" + n;
  if(starts("gyro_") || starts("accel_"))
    return "imu/" + n;
  if(starts("laser") || starts("ldp"))
    return "laser/" + n;

  return "device/" + n;
}

/**
 * @brief Names for the properties whose integer values are really an enum.
 *
 * The SDK reports these through getIntPropertyRange like any other number, so
 * without this an IMU's output data rate shows up as "7" rather than "100 Hz"
 * and there is no way to tell from the tree what the range even means. The
 * values are what ObTypes.h assigns; anything the table does not cover keeps
 * its number.
 */
struct NamedValue
{
  int value;
  const char* label;
};

struct NamedProperty
{
  /// The control path, not the OBPropertyID: the ids for these live in an
  /// internal header, while the name the SDK reports is public and is what the
  /// path is derived from anyway.
  const char* path;
  const NamedValue* values;
  int count;
};

constexpr NamedValue sample_rate_names[] = {
    {1, "1.5625 Hz"}, {2, "3.125 Hz"}, {3, "6.25 Hz"},  {4, "12.5 Hz"},
    {5, "25 Hz"},     {6, "50 Hz"},    {7, "100 Hz"},   {8, "200 Hz"},
    {9, "500 Hz"},    {10, "1 kHz"},   {11, "2 kHz"},   {12, "4 kHz"},
    {13, "8 kHz"},    {14, "16 kHz"},  {15, "32 kHz"},  {16, "400 Hz"},
    {17, "800 Hz"},
};

constexpr NamedValue gyro_fs_names[] = {
    {1, "16 dps"},   {2, "31 dps"},   {3, "62 dps"},  {4, "125 dps"},
    {5, "250 dps"},  {6, "500 dps"},  {7, "1000 dps"},{8, "2000 dps"},
    {9, "400 dps"},  {10, "800 dps"},
};

constexpr NamedValue accel_fs_names[] = {
    {1, "2 g"},  {2, "4 g"},  {3, "8 g"},  {4, "16 g"},
    {5, "3 g"},  {6, "6 g"},  {7, "12 g"}, {8, "24 g"},
};

constexpr NamedProperty named_properties[] = {
    {"imu/gyro_odr", sample_rate_names, int(std::size(sample_rate_names))},
    {"imu/accel_odr", sample_rate_names, int(std::size(sample_rate_names))},
    {"imu/gyro_full_scale", gyro_fs_names, int(std::size(gyro_fs_names))},
    {"imu/accel_full_scale", accel_fs_names, int(std::size(accel_fs_names))},
};

const NamedProperty* named_property_for(const std::string& path)
{
  for(const auto& n : named_properties)
    if(path == n.path)
      return &n;
  return nullptr;
}

/// Cached descriptor plus everything needed to talk to the property again.
struct ControlEntry
{
  OBPropertyID id{};
  OBPropertyType type{};
  std::string path;
  std::string name;
  /// Labels for a named integer property, one per value in [min, max].
  std::vector<std::string> labels;
  std::vector<const char*> label_ptrs;
  depthcam_control desc{};
};

} // namespace

// ---------------------------------------------------------------------------

struct depthcam_device
{
  std::shared_ptr<ob::Device> device;
  std::shared_ptr<ob::Config> config;
  std::unique_ptr<ob::Pipeline> pipeline;

  /// A second pipeline, for the IMU only.
  ///
  /// Not the accelerometer and gyroscope added to the video pipeline: a
  /// frameset is emitted at the slowest enabled stream's rate, so an IMU
  /// sampling at 200Hz would be delivered at 30 and the point of having it
  /// would be gone. Orbbec's own IMU example uses a pipeline of its own for
  /// the same reason.
  std::unique_ptr<ob::Pipeline> imu_pipeline;
  std::shared_ptr<ob::Config> imu_config;
  std::shared_ptr<ob::PointCloudFilter> point_cloud;
  std::shared_ptr<ob::Align> align; // only for colour-to-depth

  depthcam_open_config cfg{};
  bool color_pointcloud{};
  std::atomic_bool running{};

  depthcam_frame_cb on_frame{};
  void* user{};

  std::vector<ControlEntry> controls;
  bool controls_scanned{};

  void scanControls();
  ControlEntry* findControl(const char* id);

  void deliver_video(uint32_t stream, const std::shared_ptr<ob::Frame>& f);
  void deliver_pointcloud(const std::shared_ptr<ob::FrameSet>& fs);
  void handle(const std::shared_ptr<ob::FrameSet>& fs);
  void handleImu(const std::shared_ptr<ob::FrameSet>& fs);
};

namespace
{
/// Keeps an SDK frame alive for exactly as long as the host holds the buffer.
struct FrameHolder
{
  std::shared_ptr<ob::Frame> frame;
};

void release_holder(void* owner)
{
  delete static_cast<FrameHolder*>(owner);
}
} // namespace

void depthcam_device::deliver_video(
    uint32_t stream, const std::shared_ptr<ob::Frame>& f)
{
  if(!f || !on_frame)
    return;
  if(!(cfg.streams & stream))
    return;

  auto vf = f->as<ob::VideoFrame>();
  if(!vf)
    return;

  const auto fmt = to_depthcam_format(f->getFormat());
  if(fmt == DEPTHCAM_FMT_NONE)
    return;

  auto* holder = new FrameHolder{f};

  depthcam_frame out{};
  out.stream = stream;
  out.format = fmt;
  out.width = int32_t(vf->getWidth());
  out.height = int32_t(vf->getHeight());
  out.stride = 0;
  out.timestamp_ns = uint64_t(f->getTimeStampUs()) * 1000ull;
  out.data = f->getData();
  out.bytes = f->getDataSize();
  out.owner = holder;
  out.release = &release_holder;

  if(stream == DEPTHCAM_STREAM_DEPTH)
  {
    if(auto df = f->as<ob::DepthFrame>())
      out.depth_unit_mm = df->getValueScale();
  }

  on_frame(&out, user);
}

void depthcam_device::deliver_pointcloud(const std::shared_ptr<ob::FrameSet>& fs)
{
  if(!point_cloud || !on_frame)
    return;
  if(!(cfg.streams & DEPTHCAM_STREAM_POINTCLOUD))
    return;

  auto depth = fs->depthFrame();
  if(!depth)
    return;
  if(color_pointcloud && !fs->colorFrame())
    return;

  try
  {
    auto source = fs;

    // Colour-to-depth is a filter pass; depth-to-colour is done by the device
    // through the Config and needs nothing here.
    if(align)
    {
      auto aligned = align->process(fs);
      if(!aligned)
        return;
      source = aligned->as<ob::FrameSet>();
      if(!source)
        return;
    }

    if(color_pointcloud && !color_matches_depth(*source))
      return;

    // getValueScale() is millimetres per depth unit; the extra 1/1000 makes the
    // filter emit metres, which is what DEPTHCAM_FMT_XYZ promises. Free: the
    // filter multiplies by this factor anyway.
    point_cloud->setPositionDataScaled(depth->getValueScale() * 0.001f);

    auto pcf = point_cloud->process(source);
    if(!pcf)
      return;

    const auto bytes = pcf->getDataSize();
    if(!pcf->getData() || bytes == 0)
      return;

    const auto fmt = to_depthcam_format(pcf->getFormat());
    const auto point_bytes
        = (fmt == DEPTHCAM_FMT_XYZRGB) ? sizeof(OBColorPoint) : sizeof(OBPoint);

    auto* holder = new FrameHolder{pcf};

    depthcam_frame out{};
    out.stream = DEPTHCAM_STREAM_POINTCLOUD;
    out.format = fmt;
    out.point_count = int32_t(bytes / point_bytes);
    out.timestamp_ns = uint64_t(depth->getTimeStampUs()) * 1000ull;
    out.data = pcf->getData();
    out.bytes = bytes;
    out.owner = holder;
    out.release = &release_holder;

    on_frame(&out, user);
  }
  catch(const std::exception& e)
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

  int count = 0;
  try
  {
    count = device->getSupportedPropertyCount();
  }
  catch(...)
  {
    return;
  }

  for(int i = 0; i < count; i++)
  {
    OBPropertyItem item{};
    try
    {
      item = device->getSupportedProperty(uint32_t(i));
    }
    catch(...)
    {
      continue;
    }

    // Struct and raw properties (serial number, calibration JSON, sync and IP
    // config, device time) have no scalar form; each would need its own
    // decoding, so they are left out rather than published as something they
    // are not.
    if(item.type != OB_BOOL_PROPERTY && item.type != OB_INT_PROPERTY
       && item.type != OB_FLOAT_PROPERTY)
      continue;
    if(item.permission == OB_PERMISSION_DENY)
      continue;

    ControlEntry e;
    e.name = item.name ? item.name : "";
    e.path = control_id_for(e.name, item.permission);
    if(e.path.empty())
      continue;

    // The device can list the same property twice; the first wins.
    if(std::any_of(
           controls.begin(), controls.end(),
           [&](const ControlEntry& o) { return o.path == e.path; }))
      continue;

    e.id = item.id;
    e.type = item.type;

    auto& d = e.desc;
    d.access = 0;
    if(item.permission == OB_PERMISSION_READ
       || item.permission == OB_PERMISSION_READ_WRITE)
      d.access |= DEPTHCAM_ACCESS_READ;
    if(item.permission == OB_PERMISSION_WRITE
       || item.permission == OB_PERMISSION_READ_WRITE)
      d.access |= DEPTHCAM_ACCESS_WRITE;

    try
    {
      switch(item.type)
      {
        case OB_BOOL_PROPERTY: {
          // A write-only bool is a trigger, not a setting.
          if(item.permission == OB_PERMISSION_WRITE)
          {
            d.kind = DEPTHCAM_CONTROL_ACTION;
            d.min = 0;
            d.max = 1;
            d.step = 1;
            d.def = 0;
          }
          else
          {
            auto r = device->getBoolPropertyRange(item.id);
            d.kind = DEPTHCAM_CONTROL_BOOL;
            d.min = 0;
            d.max = 1;
            d.step = 1;
            d.def = r.def ? 1 : 0;
          }
          break;
        }
        case OB_INT_PROPERTY: {
          auto r = device->getIntPropertyRange(item.id);
          d.kind = DEPTHCAM_CONTROL_INT;
          d.min = r.min;
          d.max = r.max;
          d.step = r.step > 0 ? r.step : 1;
          d.def = r.def;

          // An integer whose values have names is an enum, whatever the SDK
          // calls it. One label per value in [min, max], because that is how
          // the ABI indexes them.
          if(const auto* named = named_property_for(e.path);
             named && r.max >= r.min && (r.max - r.min) < 64)
          {
            for(int v = r.min; v <= r.max; v++)
            {
              const char* label = nullptr;
              for(int k = 0; k < named->count && !label; k++)
                if(named->values[k].value == v)
                  label = named->values[k].label;
              e.labels.push_back(label ? label : std::to_string(v));
            }
            d.kind = DEPTHCAM_CONTROL_ENUM;
          }
          break;
        }
        case OB_FLOAT_PROPERTY: {
          auto r = device->getFloatPropertyRange(item.id);
          d.kind = DEPTHCAM_CONTROL_FLOAT;
          d.min = r.min;
          d.max = r.max;
          d.step = r.step > 0 ? r.step : 0;
          d.def = r.def;
          break;
        }
        default:
          continue;
      }
    }
    catch(...)
    {
      // A property the device lists but refuses to describe is not usable.
      continue;
    }

    controls.push_back(std::move(e));
  }

  // Point the descriptors at their owner's stable strings, after the vector has
  // stopped reallocating.
  for(auto& e : controls)
  {
    e.label_ptrs.clear();
    e.label_ptrs.reserve(e.labels.size());
    for(const auto& l : e.labels)
      e.label_ptrs.push_back(l.c_str());

    e.desc.id = e.path.c_str();
    e.desc.name = e.name.c_str();
    e.desc.description = nullptr;
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

void depthcam_device::handleImu(const std::shared_ptr<ob::FrameSet>& fs)
{
  if(!running.load(std::memory_order_acquire) || !fs || !on_frame)
    return;

  depthcam_imu_sample sample{};

  try
  {
    if(auto raw = fs->getFrame(OB_FRAME_ACCEL))
    {
      if(auto accel = raw->as<ob::AccelFrame>())
      {
        // Metres per second squared since SDK 2.9; the ABI wants SI too, so
        // there is nothing to convert.
        const auto v = accel->getValue();
        sample.accel[0] = v.x;
        sample.accel[1] = v.y;
        sample.accel[2] = v.z;
        sample.fields |= DEPTHCAM_IMU_ACCEL;

        sample.temperature_c = accel->getTemperature();
        sample.fields |= DEPTHCAM_IMU_TEMPERATURE;
      }
    }
  }
  catch(...)
  {
  }

  try
  {
    if(auto raw = fs->getFrame(OB_FRAME_GYRO))
    {
      if(auto gyro = raw->as<ob::GyroFrame>())
      {
        const auto v = gyro->getValue(); // radians per second
        sample.gyro[0] = v.x;
        sample.gyro[1] = v.y;
        sample.gyro[2] = v.z;
        sample.fields |= DEPTHCAM_IMU_GYRO;

        if(!(sample.fields & DEPTHCAM_IMU_TEMPERATURE))
        {
          sample.temperature_c = gyro->getTemperature();
          sample.fields |= DEPTHCAM_IMU_TEMPERATURE;
        }
      }
    }
  }
  catch(...)
  {
  }

  if(sample.fields == 0)
    return;

  depthcam_frame out{};
  out.stream = DEPTHCAM_STREAM_IMU;
  out.format = DEPTHCAM_FMT_IMU;
  out.timestamp_ns = uint64_t(fs->getTimeStampUs()) * 1000ull;
  out.data = &sample;
  out.bytes = sizeof(sample);
  // No owner: the host copies the sample out before returning.
  on_frame(&out, user);
}

void depthcam_device::handle(const std::shared_ptr<ob::FrameSet>& fs)
{
  if(!running.load(std::memory_order_acquire) || !fs)
    return;

  // Each stream independently: a frameset without colour must still yield
  // depth, IR and the point cloud.
  deliver_video(DEPTHCAM_STREAM_COLOR, fs->colorFrame());
  deliver_video(DEPTHCAM_STREAM_IR, fs->irFrame());
  deliver_video(DEPTHCAM_STREAM_DEPTH, fs->depthFrame());
  deliver_pointcloud(fs);
}

// ---------------------------------------------------------------------------

namespace
{

const char* backend_last_error()
{
  std::lock_guard lock{g_error_mutex};
  return g_last_error.empty() ? nullptr : g_last_error.c_str();
}

int backend_init(const char* resource_dir)
{
  try
  {
    // Must precede context construction: the SDK caches the extensions path in
    // a function-local static the first time anything asks for it.
    //
    // The *extensions* directory, not the package directory. SDK 2.5 appended
    // "extensions" itself; 2.9 takes the path literally and looked for
    // <package>/frameprocessor/libob_frame_processor.so, which does not exist.
    // The only symptom was a Femto Mega producing no point cloud at all -- the
    // failure is a warning the SDK logs and swallows.
    if(resource_dir && *resource_dir)
      ob::Context::setExtensionsDirectory(
          (std::string{resource_dir} + "/extensions").c_str());

    // Errors only, unless someone is debugging.
    //
    // The SDK's logging works again since the fork moved to 2.9.3 (the previous
    // one had every LOG_ macro defined to nothing), and it is chatty: opening a
    // Femto Mega alone prints a dozen "recoverable exception" warnings from
    // component probes that are entirely normal.
    const bool debug = std::getenv("SCORE_DEPTHCAM_DEBUG") != nullptr;
    ob::Context::setLoggerToConsole(
        debug ? OB_LOG_SEVERITY_DEBUG : OB_LOG_SEVERITY_ERROR);

    // And no log file. The SDK otherwise creates Log/OrbbecSDK.log.txt in the
    // process's working directory -- which for score is wherever the user
    // happened to launch it from -- and rotates 100MB of it.
    // "" rather than nullptr: the SDK builds a std::string from the argument
    // before looking at the severity, so a null directory throws.
    ob::Context::setLoggerToFile(OB_LOG_SEVERITY_OFF, "");

    g_context = std::make_unique<ob::Context>();

    try
    {
      // Off by default; without it a Femto Mega on the network is only
      // reachable by explicitly typing its address.
      g_context->enableNetDeviceEnumeration(true);
    }
    catch(const std::exception& e)
    {
      set_error(std::string{"network enumeration unavailable: "} + e.what());
    }

    g_context->setDeviceChangedCallback(
        [](std::shared_ptr<ob::DeviceList>, std::shared_ptr<ob::DeviceList>) {
      if(g_changed_cb)
        g_changed_cb(g_changed_user);
    });

    return 1;
  }
  catch(const std::exception& e)
  {
    set_error(e.what());
    return 0;
  }
  catch(...)
  {
    set_error("unknown error during init");
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
    auto list = g_context->queryDeviceList();
    if(!list)
      return 0;

    const auto str = [](const char* s) { return s ? std::string{s} : std::string{}; };

    const uint32_t n = list->getCount();
    for(uint32_t i = 0; i < n; i++)
    {
      EnumEntry e;
      e.name = str(list->getName(i));
      e.serial = str(list->getSerialNumber(i));
      e.transport = str(list->getConnectionType(i));

      const bool network = e.transport == "Ethernet";
      if(network)
      {
        // Reachable by address whether or not it is currently broadcasting.
        const auto ip = str(list->getIpAddress(i));
        e.uri = ip.empty() ? ("orbbec:uid:" + str(list->getUid(i)))
                           : ("orbbec:net:" + ip + ":8090");
      }
      else if(!e.serial.empty())
      {
        e.uri = "orbbec:sn:" + e.serial;
      }
      else
      {
        e.uri = "orbbec:uid:" + str(list->getUid(i));
      }

      depthcam_device_info info{};
      info.backend = "orbbec";
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

void backend_set_changed_callback(depthcam_changed_cb cb, void* user)
{
  g_changed_cb = cb;
  g_changed_user = user;
}

bool has_sensor(ob::SensorList& sensors, OBSensorType type)
{
  for(uint32_t i = 0, n = sensors.getCount(); i < n; i++)
    if(sensors.getSensorType(i) == type)
      return true;
  return false;
}

/// Tolerates a device that advertises a sensor but rejects the requested
/// profile: a camera that cannot do 1080p colour should still give us depth.
bool try_enable(
    ob::Config& config, ob::SensorList& sensors, OBSensorType type, int w, int h,
    int fps, OBFormat format = OB_FORMAT_ANY)
{
  if(!has_sensor(sensors, type))
    return false;
  try
  {
    config.enableVideoStream(
        type, w > 0 ? uint32_t(w) : OB_WIDTH_ANY, h > 0 ? uint32_t(h) : OB_HEIGHT_ANY,
        fps > 0 ? uint32_t(fps) : OB_FPS_ANY, format);
    return true;
  }
  catch(const std::exception& e)
  {
    set_error(e.what());
    return false;
  }
}

std::shared_ptr<ob::Device>
resolve_device(const Address& addr)
{
  if(addr.kind == Address::Network)
    return g_context->createNetDevice(addr.value.c_str(), addr.port);

  auto list = g_context->queryDeviceList();
  if(!list)
    throw std::runtime_error("could not query the device list");

  const uint32_t count = list->getCount();
  if(count == 0)
    throw std::runtime_error("no Orbbec device connected");

  if(addr.kind == Address::Any)
    return list->getDevice(0);

  for(uint32_t i = 0; i < count; i++)
  {
    const char* s = (addr.kind == Address::Serial) ? list->getSerialNumber(i)
                                                   : list->getUid(i);
    if(s && addr.value == s)
      return list->getDevice(i);
  }

  // Deliberately no fallback to "any camera": streaming from a different one
  // than the document names is worse than failing to connect.
  throw std::runtime_error("device '" + addr.value + "' is not connected");
}

depthcam_device* backend_open(const char* uri, const depthcam_open_config* config)
{
  if(!g_context || !config)
    return nullptr;

  try
  {
    auto dev = std::make_unique<depthcam_device>();
    dev->cfg = *config;
    dev->device = resolve_device(parse_uri(uri));
    if(!dev->device)
      return nullptr;

    // A coloured cloud requires alignment; see color_matches_depth.
    dev->color_pointcloud = (config->streams & DEPTHCAM_STREAM_POINTCLOUD)
                            && config->color_pointcloud != 0
                            && config->align != DEPTHCAM_ALIGN_NONE;

    auto sensors = dev->device->getSensorList();
    dev->config = std::make_shared<ob::Config>();

    const bool want_cloud = (config->streams & DEPTHCAM_STREAM_POINTCLOUD) != 0;
    const bool need_color
        = (config->streams & DEPTHCAM_STREAM_COLOR) || dev->color_pointcloud;
    const bool need_depth = (config->streams & DEPTHCAM_STREAM_DEPTH) || want_cloud;

    // Raw RGB is needed whenever anything is going to *process* the colour
    // frame rather than just hand it over.
    //
    // The camera negotiates MJPG by default and neither ob::Align nor the
    // point-cloud filter will decode it: the align pass logs "Unsupported
    // format for C2D conversion yet!" once per frame and produces nothing,
    // which is a console full of errors and a dead depth stream. Any alignment
    // is enough to need it -- not just a coloured cloud, which is what this
    // used to check.
    const bool need_raw_color
        = dev->color_pointcloud || config->align != DEPTHCAM_ALIGN_NONE;

    if(sensors && need_color)
    {
      int cw = config->color_width, ch = config->color_height;

      if(need_raw_color)
      {
        // Pinning the resolution matters: an explicit format with
        // OB_WIDTH_ANY makes the SDK select the *largest* matching profile,
        // which is 3840x2160 here -- 8.3M points and ~199MB per frame.
        if(cw <= 0 && ch <= 0)
        {
          cw = default_raw_color_width;
          ch = default_raw_color_height;
        }

        if(!try_enable(
               *dev->config, *sensors, OB_SENSOR_COLOR, cw, ch, config->color_fps,
               OB_FORMAT_RGB))
        {
          set_error(
              "raw RGB colour unavailable; alignment and the coloured point "
              "cloud will not work on this camera");
          dev->color_pointcloud = false;
          try_enable(
              *dev->config, *sensors, OB_SENSOR_COLOR, config->color_width,
              config->color_height, config->color_fps);
        }
      }
      else
      {
        try_enable(
            *dev->config, *sensors, OB_SENSOR_COLOR, cw, ch, config->color_fps);
      }
    }

    if(sensors && need_depth)
      try_enable(
          *dev->config, *sensors, OB_SENSOR_DEPTH, config->depth_width,
          config->depth_height, config->depth_fps);

    if(sensors && (config->streams & DEPTHCAM_STREAM_IR))
    {
      // Stereo devices (Gemini 330 family) expose left/right rather than a
      // single IR sensor.
      const auto sensor = has_sensor(*sensors, OB_SENSOR_IR) ? OB_SENSOR_IR
                                                             : OB_SENSOR_IR_LEFT;
      if(!try_enable(
             *dev->config, *sensors, sensor, config->ir_width, config->ir_height,
             config->ir_fps))
      {
        // A requested profile the camera does not have should not cost the
        // stream: fall back to whatever it does offer.
        try_enable(*dev->config, *sensors, sensor, 0, 0, 0);
      }
    }

    if(need_color && config->align == DEPTHCAM_ALIGN_DEPTH_TO_COLOR)
    {
      try
      {
        dev->config->setAlignMode(ALIGN_D2C_HW_MODE);
      }
      catch(const std::exception&)
      {
        try
        {
          dev->config->setAlignMode(ALIGN_D2C_SW_MODE);
        }
        catch(const std::exception& e2)
        {
          set_error(std::string{"no D2C alignment available: "} + e2.what());
        }
      }
    }

    // Bound to *this* device: the no-argument ob::Pipeline constructor silently
    // takes the first device the SDK enumerates.
    dev->pipeline = std::make_unique<ob::Pipeline>(dev->device);

    if(config->streams & DEPTHCAM_STREAM_IMU)
    {
      // Tolerated rather than required: several Orbbec models have no IMU, and
      // a camera without one should still stream video.
      try
      {
        dev->imu_pipeline = std::make_unique<ob::Pipeline>(dev->device);
        dev->imu_config = std::make_shared<ob::Config>();
        dev->imu_config->enableAccelStream();
        dev->imu_config->enableGyroStream();
      }
      catch(const std::exception& e)
      {
        set_error(std::string{"no IMU on this camera: "} + e.what());
        dev->imu_pipeline.reset();
        dev->imu_config.reset();
      }
    }

    if(config->align == DEPTHCAM_ALIGN_COLOR_TO_DEPTH)
      dev->align = std::make_shared<ob::Align>(OB_STREAM_DEPTH);

    if(want_cloud)
    {
      dev->point_cloud = std::make_shared<ob::PointCloudFilter>();
      dev->point_cloud->setCreatePointFormat(
          dev->color_pointcloud ? OB_FORMAT_RGB_POINT : OB_FORMAT_POINT);
      if(dev->color_pointcloud)
      {
        // Without this the filter emits r/g/b in 0-255 (its documented
        // default), and every channel clamps to white downstream.
        dev->point_cloud->setColorDataNormalization(true);
      }
    }

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
  if(!dev || !dev->pipeline)
    return 0;
  if(dev->running.load(std::memory_order_acquire))
    return 1;

  dev->on_frame = on_frame;
  dev->user = user;
  dev->running.store(true, std::memory_order_release);

  try
  {
    dev->pipeline->start(dev->config, [dev](std::shared_ptr<ob::FrameSet> fs) {
      dev->handle(fs);
    });

    if(dev->imu_pipeline)
    {
      // Its own try: a camera whose IMU refuses to start still has video, and
      // that is the more important of the two.
      try
      {
        dev->imu_pipeline->start(
            dev->imu_config,
            [dev](std::shared_ptr<ob::FrameSet> fs) { dev->handleImu(fs); });
      }
      catch(const std::exception& e)
      {
        set_error(std::string{"could not start the IMU: "} + e.what());
        dev->imu_pipeline.reset();
      }
    }
    return 1;
  }
  catch(const std::exception& e)
  {
    set_error(e.what());
    dev->running.store(false, std::memory_order_release);
    return 0;
  }
  catch(...)
  {
    dev->running.store(false, std::memory_order_release);
    return 0;
  }
}

void backend_stop(depthcam_device* dev)
{
  if(!dev)
    return;
  // The SDK throws when stopping a pipeline that was never started, and close()
  // reaches this unconditionally.
  if(!dev->running.exchange(false, std::memory_order_acq_rel))
    return;
  if(dev->imu_pipeline)
  {
    try
    {
      dev->imu_pipeline->stop();
    }
    catch(...)
    {
    }
  }

  try
  {
    dev->pipeline->stop();
  }
  catch(const std::exception& e)
  {
    set_error(e.what());
  }
  catch(...)
  {
  }
}

int backend_list_controls(depthcam_device* dev, depthcam_control_cb cb, void* user)
{
  if(!dev || !cb || !dev->device)
    return 0;
  dev->scanControls();
  for(const auto& e : dev->controls)
    cb(&e.desc, user);
  return 1;
}

int backend_get_control(depthcam_device* dev, const char* id, double* out)
{
  if(!dev || !out || !dev->device)
    return 0;
  dev->scanControls();

  auto* e = dev->findControl(id);
  if(!e || !(e->desc.access & DEPTHCAM_ACCESS_READ))
    return 0;

  try
  {
    switch(e->type)
    {
      case OB_BOOL_PROPERTY:
        *out = dev->device->getBoolProperty(e->id) ? 1.0 : 0.0;
        return 1;
      case OB_INT_PROPERTY:
        *out = double(dev->device->getIntProperty(e->id));
        return 1;
      case OB_FLOAT_PROPERTY:
        *out = double(dev->device->getFloatProperty(e->id));
        return 1;
      default:
        return 0;
    }
  }
  catch(const std::exception& ex)
  {
    set_error(std::string{"get "} + id + ": " + ex.what());
    return 0;
  }
  catch(...)
  {
    return 0;
  }
}

int backend_set_control(depthcam_device* dev, const char* id, double value)
{
  if(!dev || !dev->device)
    return 0;
  dev->scanControls();

  auto* e = dev->findControl(id);
  if(!e || !(e->desc.access & DEPTHCAM_ACCESS_WRITE))
    return 0;

  try
  {
    switch(e->type)
    {
      case OB_BOOL_PROPERTY:
        dev->device->setBoolProperty(e->id, value != 0.0);
        return 1;
      case OB_INT_PROPERTY:
        dev->device->setIntProperty(e->id, int32_t(value));
        return 1;
      case OB_FLOAT_PROPERTY:
        dev->device->setFloatProperty(e->id, float(value));
        return 1;
      default:
        return 0;
    }
  }
  catch(const std::exception& ex)
  {
    set_error(std::string{"set "} + id + ": " + ex.what());
    return 0;
  }
  catch(...)
  {
    return 0;
  }
}

uint32_t backend_active_streams(depthcam_device* dev)
{
  if(!dev)
    return 0;
  uint32_t streams = dev->cfg.streams;
  // The IMU is the only one that can be asked for and quietly not granted:
  // several models have none, and the pipeline is only created when one does.
  if(!dev->imu_pipeline)
    streams &= ~uint32_t(DEPTHCAM_STREAM_IMU);
  return streams;
}

const depthcam_backend_v1 g_backend{
    .abi_version = DEPTHCAM_ABI_VERSION,
    .name = "orbbec",
    .display_name = "Orbbec",
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
