/*
 * Luxonis OAK backend: depthai-core behind depthcam_abi.h.
 *
 * The pipeline is built out of dai::node::Depth, not dai::node::StereoDepth:
 * Depth picks stereo, ToF or on-device neural depth from what the camera
 * actually has, so one pipeline covers the whole OAK line. Hand-wired
 * StereoDepth works on an OAK-D and produces nothing on an OAK-D-SR-PoE.
 *
 * depthai-core is built as a nested CMake project rather than with
 * add_subdirectory (see Backends/CMakeLists.txt), and its telemetry is switched
 * off in init() below.
 */
#include <depthcam_abi.h>

#include <depthai/depthai.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
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

/// Only if the user has not already said otherwise: an explicit
/// DEPTHAI_TELEMETRY=1 is a deliberate opt-in and is left alone.
void set_env_default(const char* name, const char* value)
{
#if defined(_WIN32)
  size_t len = 0;
  if(getenv_s(&len, nullptr, 0, name) == 0 && len > 0)
    return;
  _putenv_s(name, value);
#else
  ::setenv(name, value, 0);
#endif
}

/// "depthai:id:14442C10D1...", "depthai:ip:10.0.0.5" or "depthai:index:0";
/// anything else is taken as a device id or an address, which is exactly what
/// dai::DeviceInfo's own string constructor accepts.
struct Address
{
  enum Kind
  {
    Any,
    Id,
    Name,
    Index
  } kind{Any};
  std::string text;
  int index{0};
};

Address parse_uri(const char* uri)
{
  Address a;
  if(!uri)
    return a;
  std::string s{uri};
  if(s.rfind("depthai:", 0) == 0)
    s = s.substr(8);
  if(s.empty())
    return a;

  if(s.rfind("id:", 0) == 0)
  {
    a.kind = Address::Id;
    a.text = s.substr(3);
  }
  else if(s.rfind("ip:", 0) == 0 || s.rfind("name:", 0) == 0)
  {
    a.kind = Address::Name;
    a.text = s.substr(s[0] == 'i' ? 3 : 5);
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
    a.kind = Address::Id;
    a.text = s;
  }

  if(a.kind != Address::Index && a.kind != Address::Any && a.text.empty())
    a.kind = Address::Any;
  return a;
}

const char* transport_of(XLinkProtocol_t p)
{
  switch(p)
  {
    case X_LINK_USB_VSC:
    case X_LINK_USB_CDC:
      return "usb";
    case X_LINK_PCIE:
      return "pcie";
    case X_LINK_TCP_IP:
      return "ethernet";
    default:
      return "";
  }
}

/// The pipeline asks for RGB888i, GRAY8 and RAW16; the rest are here because
/// Camera::requestOutput may hand back something else when a sensor cannot
/// produce what was asked for.
int32_t format_of(dai::ImgFrame::Type t)
{
  using T = dai::ImgFrame::Type;
  switch(t)
  {
    case T::RGB888i:
      return DEPTHCAM_FMT_RGB24;
    case T::BGR888i:
      return DEPTHCAM_FMT_BGR24;
    case T::RGBA8888:
      return DEPTHCAM_FMT_RGBA;
    case T::GRAY8:
    case T::YUV400p:
      return DEPTHCAM_FMT_GRAY8;
    case T::RAW8:
      return DEPTHCAM_FMT_GRAY8;
    case T::RAW16:
    case T::RAW14:
    case T::RAW12:
    case T::RAW10:
      return DEPTHCAM_FMT_GRAY16;
    case T::NV12:
      return DEPTHCAM_FMT_NV12;
    case T::YUV420p:
      return DEPTHCAM_FMT_YUV420P;
    case T::YUV422i:
      return DEPTHCAM_FMT_YUYV422;
    default:
      return DEPTHCAM_FMT_NONE;
  }
}

uint64_t timestamp_ns(const std::chrono::steady_clock::time_point& tp)
{
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      tp.time_since_epoch())
                      .count());
}

/// Keeps a depthai message alive while the host holds its frame. No copy:
/// messages are refcounted and their buffers are not recycled behind our back.
struct MessageHolder
{
  std::shared_ptr<dai::ADatatype> msg;
};

void release_message_holder(void* owner)
{
  delete static_cast<MessageHolder*>(owner);
}

/// Point clouds are converted rather than borrowed, so they carry their own
/// buffer.
struct CloudHolder
{
  std::vector<float> data;
};

void release_cloud_holder(void* owner)
{
  delete static_cast<CloudHolder*>(owner);
}

struct ImuHolder
{
  depthcam_imu_sample sample;
};

void release_imu_holder(void* owner)
{
  delete static_cast<ImuHolder*>(owner);
}

// --- controls ---------------------------------------------------------------

/**
 * depthai-core has no getters for camera settings -- dai::CameraControl is a
 * message you send into a node's input and nothing reads back -- so get()
 * returns what was last written, seeded with the SDK's defaults. The two
 * illumination controls and the chip temperature are device calls instead.
 */
enum class ControlId
{
  AutoExposure,
  ExposureTime,
  Iso,
  AutoFocus,
  Focus,
  AutoWhiteBalance,
  WhiteBalance,
  Brightness,
  Contrast,
  Saturation,
  Sharpness,
  LumaDenoise,
  ChromaDenoise,
  LaserPower,
  FloodLight,
  Temperature,
};

struct ControlDef
{
  ControlId id;
  const char* path;
  const char* name;
  const char* description;
  int32_t kind;
  int32_t access;
  double min, max, step, def;
};

/// Ranges from the doc comments on dai::CameraControl: 1us..33ms is what the
/// ISP accepts, 100..1600 what the IMX378/OV9282 sensors report.
const ControlDef control_defs[] = {
    {ControlId::AutoExposure, "color/auto_exposure", "auto exposure",
     "Let the camera choose exposure time and sensitivity",
     DEPTHCAM_CONTROL_BOOL, DEPTHCAM_ACCESS_READ | DEPTHCAM_ACCESS_WRITE, 0, 1,
     1, 1},
    {ControlId::ExposureTime, "color/exposure_time", "exposure time",
     "Microseconds; writing it also turns auto exposure off",
     DEPTHCAM_CONTROL_INT, DEPTHCAM_ACCESS_READ | DEPTHCAM_ACCESS_WRITE, 1,
     33000, 1, 20000},
    {ControlId::Iso, "color/iso", "ISO",
     "Sensitivity; writing it also turns auto exposure off",
     DEPTHCAM_CONTROL_INT, DEPTHCAM_ACCESS_READ | DEPTHCAM_ACCESS_WRITE, 100,
     1600, 1, 800},
    {ControlId::AutoFocus, "color/auto_focus", "auto focus",
     "Continuous autofocus, on models with a focus motor",
     DEPTHCAM_CONTROL_BOOL, DEPTHCAM_ACCESS_READ | DEPTHCAM_ACCESS_WRITE, 0, 1,
     1, 1},
    {ControlId::Focus, "color/focus", "focus",
     "Lens position; writing it also turns auto focus off",
     DEPTHCAM_CONTROL_INT, DEPTHCAM_ACCESS_READ | DEPTHCAM_ACCESS_WRITE, 0, 255,
     1, 130},
    {ControlId::AutoWhiteBalance, "color/auto_white_balance",
     "auto white balance", nullptr, DEPTHCAM_CONTROL_BOOL,
     DEPTHCAM_ACCESS_READ | DEPTHCAM_ACCESS_WRITE, 0, 1, 1, 1},
    {ControlId::WhiteBalance, "color/white_balance", "white balance",
     "Colour temperature in kelvin; writing it also turns auto white balance off",
     DEPTHCAM_CONTROL_INT, DEPTHCAM_ACCESS_READ | DEPTHCAM_ACCESS_WRITE, 1000,
     12000, 1, 4000},
    {ControlId::Brightness, "color/brightness", "brightness", nullptr,
     DEPTHCAM_CONTROL_INT, DEPTHCAM_ACCESS_READ | DEPTHCAM_ACCESS_WRITE, -10, 10,
     1, 0},
    {ControlId::Contrast, "color/contrast", "contrast", nullptr,
     DEPTHCAM_CONTROL_INT, DEPTHCAM_ACCESS_READ | DEPTHCAM_ACCESS_WRITE, -10, 10,
     1, 0},
    {ControlId::Saturation, "color/saturation", "saturation", nullptr,
     DEPTHCAM_CONTROL_INT, DEPTHCAM_ACCESS_READ | DEPTHCAM_ACCESS_WRITE, -10, 10,
     1, 0},
    {ControlId::Sharpness, "color/sharpness", "sharpness", nullptr,
     DEPTHCAM_CONTROL_INT, DEPTHCAM_ACCESS_READ | DEPTHCAM_ACCESS_WRITE, 0, 4, 1,
     1},
    {ControlId::LumaDenoise, "color/luma_denoise", "luma denoise", nullptr,
     DEPTHCAM_CONTROL_INT, DEPTHCAM_ACCESS_READ | DEPTHCAM_ACCESS_WRITE, 0, 4, 1,
     1},
    {ControlId::ChromaDenoise, "color/chroma_denoise", "chroma denoise", nullptr,
     DEPTHCAM_CONTROL_INT, DEPTHCAM_ACCESS_READ | DEPTHCAM_ACCESS_WRITE, 0, 4, 1,
     1},
};

/// Published only where the hardware has the drivers; see getIrDrivers().
const ControlDef ir_control_defs[] = {
    {ControlId::LaserPower, "depth/laser_power", "laser dot projector",
     "Intensity of the structured-light projector, 0 to 1. It is what makes "
     "depth work on a blank wall, and it is off by default",
     DEPTHCAM_CONTROL_FLOAT, DEPTHCAM_ACCESS_READ | DEPTHCAM_ACCESS_WRITE, 0, 1,
     0, 0},
    {ControlId::FloodLight, "ir/flood_light", "infrared illumination",
     "Intensity of the infrared floodlight, 0 to 1", DEPTHCAM_CONTROL_FLOAT,
     DEPTHCAM_ACCESS_READ | DEPTHCAM_ACCESS_WRITE, 0, 1, 0, 0},
};

const ControlDef temperature_def{
    ControlId::Temperature, "sensors/temperature", "temperature",
    "Average of the on-chip sensors, in degrees Celsius",
    DEPTHCAM_CONTROL_FLOAT, DEPTHCAM_ACCESS_READ, -40, 125, 0, 0};

} // namespace

// ---------------------------------------------------------------------------

struct depthcam_device
{
  std::shared_ptr<dai::Device> device;
  std::unique_ptr<dai::Pipeline> pipeline;

  std::shared_ptr<dai::node::Camera> colorCam, irCam;
  std::shared_ptr<dai::node::Depth> depthNode;
  std::shared_ptr<dai::node::PointCloud> pcNode;
  std::shared_ptr<dai::node::IMU> imuNode;

  std::shared_ptr<dai::MessageQueue> colorQ, irQ, depthQ, pcQ, imuQ;
  std::shared_ptr<dai::InputQueue> colorControlQ;

  depthcam_open_config cfg{};
  /// What the pipeline actually wired up: an OAK-1 has no depth, a plain OAK-D
  /// no IMU.
  uint32_t active{};
  bool tinted_cloud{};

  std::atomic_bool running{};
  /// One-way, both of them: the pipeline is never torn down until close.
  /// See backend_stop.
  bool started{};
  bool callbacks_added{};
  // Read on the depthai queue threads, written by backend_start. stop() does
  // not join those threads -- see backend_stop -- so the two must be taken
  // together or a restart can pair a new callback with a freed user pointer.
  struct Sink
  {
    depthcam_frame_cb cb{};
    void* user{};
  };
  std::mutex sink_mutex;
  Sink sink;

  Sink currentSink()
  {
    std::lock_guard lock{sink_mutex};
    return running.load(std::memory_order_acquire) ? sink : Sink{};
  }

  std::vector<ControlDef> controls;

  /// Mirrors of what was last written; depthai has no getters.
  std::mutex control_lock;
  bool auto_exposure{true}, auto_focus{true}, auto_white_balance{true};
  double exposure_us{20000}, iso{800}, focus{130}, white_balance{4000};
  double brightness{0}, contrast{0}, saturation{0}, sharpness{1};
  double luma_denoise{1}, chroma_denoise{1};
  double laser_power{0}, flood_light{0};

  ~depthcam_device();

  void emitImage(uint32_t stream, const std::shared_ptr<dai::ImgFrame>& f, float unit_mm);
  void emitPointCloud(const std::shared_ptr<dai::PointCloudData>& pc);
  void emitImu(const std::shared_ptr<dai::IMUData>& imu);

  void applyCameraControl();
  const ControlDef* findControl(const char* id) const;
};

depthcam_device::~depthcam_device()
{
  try
  {
    if(pipeline)
    {
      // The queue callbacks capture `this`, so the pipeline has to be fully
      // stopped -- not merely asked to -- before this object goes away.
      if(pipeline->isRunning())
        pipeline->stop();
      pipeline->wait();
    }
  }
  catch(...)
  {
  }

  colorQ.reset();
  irQ.reset();
  depthQ.reset();
  pcQ.reset();
  imuQ.reset();
  colorControlQ.reset();
  pipeline.reset();
  device.reset();
}

void depthcam_device::emitImage(
    uint32_t stream, const std::shared_ptr<dai::ImgFrame>& f, float unit_mm)
{
  const auto out_sink = currentSink();
  if(!out_sink.cb || !f)
    return;

  const auto data = f->getData();
  if(data.empty())
    return;

  const int32_t format = format_of(f->getType());
  if(format == DEPTHCAM_FMT_NONE)
    return;

  auto* holder = new MessageHolder{f};

  depthcam_frame out{};
  out.stream = stream;
  out.format = format;
  out.width = int32_t(f->getWidth());
  out.height = int32_t(f->getHeight());
  out.stride = int32_t(f->getStride());
  out.timestamp_ns = timestamp_ns(f->getTimestamp());
  out.data = data.data();
  out.bytes = data.size();
  out.depth_unit_mm = unit_mm;
  out.owner = holder;
  out.release = &release_message_holder;

  out_sink.cb(&out, out_sink.user);
}

void depthcam_device::emitPointCloud(const std::shared_ptr<dai::PointCloudData>& pc)
{
  const auto out_sink = currentSink();
  if(!out_sink.cb || !pc)
    return;

  // Already metres: the PointCloud node is built with LengthUnit::METER, which
  // is not its default. Axes are depthai's camera optical frame, which is the
  // +X right / +Y down / +Z forward depthcam_abi.h asks for.
  //
  // The message buffer is read directly rather than through getPoints(), which
  // materialises a std::vector per call (two, on a colour cloud). The layouts
  // are what PointCloudData itself casts to.
  const bool color = pc->isColor();
  const bool tinted = tinted_cloud && color;
  const int floats_per_point = tinted ? 6 : 3;

  const auto raw = pc->getData();
  const std::size_t point_bytes
      = color ? sizeof(dai::Point3fRGBA) : sizeof(dai::Point3f);
  const std::size_t n = raw.size() / point_bytes;
  if(n == 0)
    return;

  auto holder = std::make_unique<CloudHolder>();
  auto& out = holder->data;
  out.reserve(n * floats_per_point);

  // depthai writes an all-zero point where it had no depth.
  const auto keep = [](float z) { return std::isfinite(z) && z > 0.f; };

  if(color)
  {
    const auto* pts = reinterpret_cast<const dai::Point3fRGBA*>(raw.data());
    for(std::size_t i = 0; i < n; i++)
    {
      const auto& p = pts[i];
      if(!keep(p.z))
        continue;
      out.push_back(p.x);
      out.push_back(p.y);
      out.push_back(p.z);
      if(tinted)
      {
        out.push_back(p.r / 255.f);
        out.push_back(p.g / 255.f);
        out.push_back(p.b / 255.f);
      }
    }
  }
  else
  {
    const auto* pts = reinterpret_cast<const dai::Point3f*>(raw.data());
    for(std::size_t i = 0; i < n; i++)
    {
      const auto& p = pts[i];
      if(!keep(p.z))
        continue;
      out.push_back(p.x);
      out.push_back(p.y);
      out.push_back(p.z);
    }
  }

  if(out.empty())
    return;

  depthcam_frame frame{};
  frame.stream = DEPTHCAM_STREAM_POINTCLOUD;
  frame.format = tinted ? DEPTHCAM_FMT_XYZRGB : DEPTHCAM_FMT_XYZ;
  frame.point_count = int32_t(out.size() / floats_per_point);
  frame.timestamp_ns = timestamp_ns(pc->getTimestamp());
  frame.data = out.data();
  frame.bytes = out.size() * sizeof(float);
  frame.owner = holder.get();
  frame.release = &release_cloud_holder;

  holder.release();
  out_sink.cb(&frame, out_sink.user);
}

void depthcam_device::emitImu(const std::shared_ptr<dai::IMUData>& imu)
{
  const auto out_sink = currentSink();
  if(!out_sink.cb || !imu)
    return;

  for(const auto& packet : imu->packets)
  {
    auto holder = std::make_unique<ImuHolder>();
    auto& sample = holder->sample;

    // Both fields: the IMU node batches the two reports into one packet on the
    // device, so nothing is waited for here.
    //
    // No temperature: IMUPacket carries none, and getChipTemperature() is the
    // SoC's rather than the inertial sensor's -- published as the
    // sensors/temperature control instead.
    sample.fields = DEPTHCAM_IMU_ACCEL | DEPTHCAM_IMU_GYRO;

    // ACCELEROMETER_CALIBRATED is m/s^2 and GYROSCOPE_CALIBRATED rad/s, which
    // is what the ABI asks for.
    sample.accel[0] = packet.acceleroMeter.x;
    sample.accel[1] = packet.acceleroMeter.y;
    sample.accel[2] = packet.acceleroMeter.z;
    sample.gyro[0] = packet.gyroscope.x;
    sample.gyro[1] = packet.gyroscope.y;
    sample.gyro[2] = packet.gyroscope.z;

    depthcam_frame frame{};
    frame.stream = DEPTHCAM_STREAM_IMU;
    frame.format = DEPTHCAM_FMT_IMU;
    frame.timestamp_ns = timestamp_ns(packet.acceleroMeter.getTimestamp());
    frame.data = &sample;
    frame.bytes = sizeof(sample);
    frame.owner = holder.get();
    frame.release = &release_imu_holder;

    holder.release();
    out_sink.cb(&frame, out_sink.user);
  }
}

void depthcam_device::applyCameraControl()
{
  // Caller holds control_lock.
  if(!colorControlQ)
    return;

  try
  {
    auto ctrl = std::make_shared<dai::CameraControl>();

    if(auto_exposure)
      ctrl->setAutoExposureEnable();
    else
      ctrl->setManualExposure(uint32_t(exposure_us), uint32_t(iso));

    if(auto_focus)
      ctrl->setAutoFocusMode(dai::CameraControl::AutoFocusMode::CONTINUOUS_VIDEO);
    else
      ctrl->setManualFocus(uint8_t(std::clamp(focus, 0., 255.)));

    if(auto_white_balance)
      ctrl->setAutoWhiteBalanceMode(dai::CameraControl::AutoWhiteBalanceMode::AUTO);
    else
      ctrl->setManualWhiteBalance(int(white_balance));

    ctrl->setBrightness(int(brightness));
    ctrl->setContrast(int(contrast));
    ctrl->setSaturation(int(saturation));
    ctrl->setSharpness(int(sharpness));
    ctrl->setLumaDenoise(int(luma_denoise));
    ctrl->setChromaDenoise(int(chroma_denoise));

    colorControlQ->send(ctrl);
  }
  catch(...)
  {
  }
}

const ControlDef* depthcam_device::findControl(const char* id) const
{
  if(!id)
    return nullptr;
  for(const auto& c : controls)
    if(std::strcmp(c.path, id) == 0)
      return &c;
  return nullptr;
}

// ---------------------------------------------------------------------------

namespace
{

int backend_init([[maybe_unused]] const char* resource_dir)
{
  try
  {
#if defined(_WIN32)
    /*
     * depthai-core.dll is delay-loaded (see Backends/CMakeLists.txt) purely so
     * that this can run first: the process search order does not include the
     * directory this module came from, so the DLL sitting right beside it is
     * not found. LOAD_WITH_ALTERED_SEARCH_PATH covers the whole chain in one
     * call -- depthai's Windows triplet keeps libusb dynamic, and that DLL is
     * in the same directory.
     */
    if(resource_dir && *resource_dir)
    {
      const std::string dll = std::string{resource_dir} + "\\depthai-core.dll";
      if(!GetModuleHandleA("depthai-core.dll")
         && !LoadLibraryExA(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
      {
        set_error(
            "could not load depthai-core.dll from the package directory (error "
            + std::to_string(GetLastError())
            + "). It is built against the Visual C++ runtime, so that "
              "redistributable has to be installed.");
        return 0;
      }
    }
#endif

    // depthai-core reports usage to Luxonis unless this says otherwise, from
    // inside a media application the user did not point at the internet.
    set_env_default("DEPTHAI_TELEMETRY", "0");

    // Some builds log to stderr at info level.
    set_env_default("DEPTHAI_LEVEL", "warn");

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

void backend_shutdown() { }

int backend_enumerate(depthcam_enumerate_cb cb, void* user)
{
  if(!cb)
    return 0;

  try
  {
    // No device is opened: this is an XLink discovery sweep over USB and the
    // local network.
    for(const auto& info : dai::Device::getAllAvailableDevices())
    {
      const std::string id = info.getDeviceId();
      const std::string uri
          = "depthai:id:" + (id.empty() ? info.name : id);
      // The model is only knowable once the device is booted, and info.name is
      // the USB path or the IP address, not a name. The transport and the
      // address are already carried by their own fields.
      const char* const name = "OAK";

      depthcam_device_info out{};
      out.backend = "depthai";
      out.uri = uri.c_str();
      out.name = name;
      out.serial = id.c_str();
      out.transport = transport_of(info.protocol);
      // A wish, not a promise: what a given OAK can deliver is only known once
      // it is open, and active_streams() reports it then.
      out.streams = DEPTHCAM_STREAM_COLOR | DEPTHCAM_STREAM_IR
                    | DEPTHCAM_STREAM_DEPTH | DEPTHCAM_STREAM_POINTCLOUD
                    | DEPTHCAM_STREAM_IMU;
      cb(&out, user);
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
  // depthai-core has no hot-plug notification; the host re-enumerates on demand.
}

std::optional<dai::DeviceInfo> find_device(const Address& addr)
{
  const auto all = dai::Device::getAllAvailableDevices();
  if(all.empty())
    return std::nullopt;

  switch(addr.kind)
  {
    case Address::Any:
      return all.front();
    case Address::Index:
      if(addr.index >= 0 && size_t(addr.index) < all.size())
        return all[size_t(addr.index)];
      return std::nullopt;
    case Address::Id:
      for(const auto& d : all)
        if(d.getDeviceId() == addr.text)
          return d;
      return std::nullopt;
    case Address::Name:
      for(const auto& d : all)
        if(d.name == addr.text)
          return d;
      return std::nullopt;
  }
  return std::nullopt;
}

depthcam_device* backend_open(const char* uri, const depthcam_open_config* config)
{
  if(!config)
    return nullptr;

  try
  {
    const auto addr = parse_uri(uri);

    auto dev = std::make_unique<depthcam_device>();
    dev->cfg = *config;

    if(const auto info = find_device(addr))
    {
      dev->device = std::make_shared<dai::Device>(*info);
    }
    else if(addr.kind == Address::Id || addr.kind == Address::Name)
    {
      // A PoE camera routed from another subnet does not answer discovery but
      // can still be reached by address, which DeviceInfo's string constructor
      // accepts. A wrong name throws and is reported below.
      dev->device = std::make_shared<dai::Device>(dai::DeviceInfo{addr.text});
    }
    else if(addr.kind == Address::Index)
    {
      set_error("no Luxonis OAK at index " + std::to_string(addr.index));
      return nullptr;
    }
    else
    {
      set_error("no Luxonis OAK camera connected");
      return nullptr;
    }

    auto& device = dev->device;
    dev->pipeline = std::make_unique<dai::Pipeline>(device);
    auto& pipeline = *dev->pipeline;

    const bool want_color = (config->streams & DEPTHCAM_STREAM_COLOR) != 0;
    const bool want_ir = (config->streams & DEPTHCAM_STREAM_IR) != 0;
    const bool want_depth = (config->streams & DEPTHCAM_STREAM_DEPTH) != 0;
    const bool want_cloud = (config->streams & DEPTHCAM_STREAM_POINTCLOUD) != 0;
    const bool want_imu = (config->streams & DEPTHCAM_STREAM_IMU) != 0;
    const bool tint = want_cloud && config->color_pointcloud != 0;

    /*
     * The IR camera must be created *before* the Depth node: Depth's
     * ensureStereoOutputs() reuses a Camera already on the stereo sockets, and
     * creating a second one on an occupied socket is an error. Claiming the
     * left socket here gets IR out of the same sensor at no extra bandwidth.
     */
    dai::Node::Output* irOut{};
    if(want_ir)
    {
      const auto pairs = device->getAvailableStereoPairs();
      if(!pairs.empty())
      {
        dev->irCam = pipeline.create<dai::node::Camera>()->build(pairs.front().left);
        const auto w = config->ir_width > 0 ? uint32_t(config->ir_width) : 640u;
        const auto h = config->ir_height > 0 ? uint32_t(config->ir_height) : 400u;
        irOut = dev->irCam->requestOutput(
            std::make_pair(w, h), dai::ImgFrame::Type::GRAY8,
            dai::ImgResizeMode::CROP,
            config->ir_fps > 0 ? std::optional<float>(float(config->ir_fps))
                               : std::nullopt);
        if(irOut)
          dev->active |= DEPTHCAM_STREAM_IR;
      }
    }

    dai::Node::Output* colorOut{};
    if(want_color || tint)
    {
      const auto colorSockets = device->getConnectedCameras(dai::CameraSensorType::COLOR);
      const auto socket = colorSockets.empty() ? dai::CameraBoardSocket::CAM_A
                                               : colorSockets.front();
      dev->colorCam = pipeline.create<dai::node::Camera>()->build(socket);

      const auto w = config->color_width > 0 ? uint32_t(config->color_width) : 1280u;
      const auto h = config->color_height > 0 ? uint32_t(config->color_height) : 720u;
      colorOut = dev->colorCam->requestOutput(
          std::make_pair(w, h), dai::ImgFrame::Type::RGB888i,
          dai::ImgResizeMode::CROP,
          config->color_fps > 0 ? std::optional<float>(float(config->color_fps))
                                : std::nullopt);
      if(colorOut && want_color)
        dev->active |= DEPTHCAM_STREAM_COLOR;
    }

    if(want_depth || want_cloud)
    {
      dev->depthNode = pipeline.create<dai::node::Depth>();

      const std::optional<std::pair<uint32_t, uint32_t>> size
          = (config->depth_width > 0 && config->depth_height > 0)
                ? std::optional<std::pair<uint32_t, uint32_t>>{{
                      uint32_t(config->depth_width),
                      uint32_t(config->depth_height)}}
                : std::nullopt;
      const std::optional<float> fps
          = config->depth_fps > 0 ? std::optional<float>(float(config->depth_fps))
                                  : std::nullopt;

      // AUTO: stereo on an OAK-D, ToF on an OAK-D-SR-PoE, neural depth on RVC4.
      dev->depthNode->build(dai::node::Depth::Algorithm::AUTO, fps, size);

      // Before the first depth() access, hence before the queues below.
      if(colorOut && config->align == DEPTHCAM_ALIGN_DEPTH_TO_COLOR)
        dev->depthNode->setAlignTo(*colorOut);

      if(want_depth)
      {
        dev->depthQ = dev->depthNode->depth().createOutputQueue(4, false);
        dev->active |= DEPTHCAM_STREAM_DEPTH;
      }

      if(want_cloud)
      {
        dev->pcNode = pipeline.create<dai::node::PointCloud>();
        // Metres, not the millimetres depthai defaults to; the node scales on
        // the device, so this is free.
        dev->pcNode->initialConfig->setLengthUnit(dai::LengthUnit::METER);

        dev->depthNode->depth().link(dev->pcNode->inputDepth);
        if(tint && colorOut)
        {
          colorOut->link(dev->pcNode->getColorInput());
          dev->tinted_cloud = true;
        }

        dev->pcQ = dev->pcNode->outputPointCloud.createOutputQueue(4, false);
        dev->active |= DEPTHCAM_STREAM_POINTCLOUD;
      }
    }

    if(want_imu)
    {
      // Guarded so a document that asked for everything still gets its video on
      // a camera with no IMU.
      try
      {
        dev->imuNode = pipeline.create<dai::node::IMU>();
        // CALIBRATED, not RAW: m/s^2 and rad/s with the stored calibration
        // applied, which is what depthcam_abi.h documents.
        dev->imuNode->enableIMUSensor(dai::IMUSensor::ACCELEROMETER_CALIBRATED, 200);
        dev->imuNode->enableIMUSensor(dai::IMUSensor::GYROSCOPE_CALIBRATED, 200);
        // One packet per report: batching would trade latency for throughput.
        dev->imuNode->setBatchReportThreshold(1);
        dev->imuNode->setMaxBatchReports(10);
        dev->imuQ = dev->imuNode->out.createOutputQueue(50, false);
        dev->active |= DEPTHCAM_STREAM_IMU;
      }
      catch(const std::exception& e)
      {
        set_error(std::string{"no IMU on this camera: "} + e.what());
        dev->imuNode.reset();
        dev->imuQ.reset();
      }
    }

    if(irOut)
      dev->irQ = irOut->createOutputQueue(4, false);
    if(colorOut && want_color)
      dev->colorQ = colorOut->createOutputQueue(4, false);

    if(dev->colorCam)
      dev->colorControlQ = dev->colorCam->inputControl.createInputQueue();

    if(dev->active == 0)
    {
      set_error("this camera could not provide any of the requested streams");
      return nullptr;
    }

    // --- controls ---------------------------------------------------------
    if(dev->colorCam)
      for(const auto& d : control_defs)
        dev->controls.push_back(d);

    // Only where the hardware has them: a plain OAK-D has neither.
    try
    {
      if(!device->getIrDrivers().empty())
        for(const auto& d : ir_control_defs)
          dev->controls.push_back(d);
    }
    catch(...)
    {
    }

    dev->controls.push_back(temperature_def);

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

  try
  {
    {
      std::lock_guard lock{dev->sink_mutex};
      dev->sink = {on_frame, user};
      dev->running.store(true, std::memory_order_release);
    }

    if(dev->started)
    {
      // Second play: the pipeline was never torn down (see backend_stop), so
      // frames simply start being delivered again.
      std::lock_guard lock{dev->control_lock};
      dev->applyCameraControl();
      return 1;
    }

    // Once only, even if pipeline->start() below fails and the host retries:
    // MessageQueue::addCallback appends, so a second pass would deliver every
    // frame twice.
    if(!dev->callbacks_added)
    {
      dev->callbacks_added = true;

      // Callbacks rather than a polling thread: MessageQueue already runs one per
      // queue. Nothing may escape into those threads.
      if(dev->colorQ)
        dev->colorQ->addCallback([dev](std::shared_ptr<dai::ADatatype> msg) {
          try
          {
            dev->emitImage(
                DEPTHCAM_STREAM_COLOR,
                std::dynamic_pointer_cast<dai::ImgFrame>(msg), 0.f);
          }
          catch(...)
          {
          }
        });

      if(dev->irQ)
        dev->irQ->addCallback([dev](std::shared_ptr<dai::ADatatype> msg) {
          try
          {
            dev->emitImage(
                DEPTHCAM_STREAM_IR, std::dynamic_pointer_cast<dai::ImgFrame>(msg),
                0.f);
          }
          catch(...)
          {
          }
        });

      if(dev->depthQ)
        dev->depthQ->addCallback([dev](std::shared_ptr<dai::ADatatype> msg) {
          try
          {
            // Depth frames are uint16 millimetres, so the multiplier is 1.
            dev->emitImage(
                DEPTHCAM_STREAM_DEPTH,
                std::dynamic_pointer_cast<dai::ImgFrame>(msg), 1.f);
          }
          catch(...)
          {
          }
        });

      if(dev->pcQ)
        dev->pcQ->addCallback([dev](std::shared_ptr<dai::ADatatype> msg) {
          try
          {
            dev->emitPointCloud(std::dynamic_pointer_cast<dai::PointCloudData>(msg));
          }
          catch(...)
          {
          }
        });

      if(dev->imuQ)
        dev->imuQ->addCallback([dev](std::shared_ptr<dai::ADatatype> msg) {
          try
          {
            dev->emitImu(std::dynamic_pointer_cast<dai::IMUData>(msg));
          }
          catch(...)
          {
          }
        });
    }

    dev->pipeline->start();
    dev->started = true;

    // The control queue only reaches the device once the pipeline is running,
    // so anything set before play is applied here rather than lost.
    {
      std::lock_guard lock{dev->control_lock};
      dev->applyCameraControl();
      try
      {
        if(dev->laser_power > 0)
          dev->device->setIrLaserDotProjectorIntensity(float(dev->laser_power));
        if(dev->flood_light > 0)
          dev->device->setIrFloodLightIntensity(float(dev->flood_light));
      }
      catch(...)
      {
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
  /*
   * Stops *delivery*, not the pipeline.
   *
   * dai::Pipeline::stop() is one-way -- it closes every output queue and then
   * the device -- so a stopped pipeline cannot be restarted, and the host
   * calls stop()/start() around every play. Rebuilding it instead would mean
   * re-uploading firmware, which takes seconds.
   *
   * So the camera keeps streaming into callbacks that drop what they get, and
   * the pipeline is torn down in the destructor.
   */
  if(!dev)
    return;
  dev->running.store(false, std::memory_order_release);
}

int backend_list_controls(depthcam_device* dev, depthcam_control_cb cb, void* user)
{
  if(!dev || !cb)
    return 0;

  for(const auto& d : dev->controls)
  {
    depthcam_control c{};
    c.id = d.path;
    c.name = d.name;
    c.description = d.description;
    c.kind = d.kind;
    c.access = d.access;
    c.min = d.min;
    c.max = d.max;
    c.step = d.step;
    c.def = d.def;
    cb(&c, user);
  }
  return 1;
}

int backend_get_control(depthcam_device* dev, const char* id, double* out)
{
  if(!dev || !id || !out)
    return 0;

  const ControlDef* def = dev->findControl(id);
  if(!def)
    return 0;

  if(def->id == ControlId::Temperature)
  {
    try
    {
      *out = dev->device->getChipTemperature().average;
      return 1;
    }
    catch(...)
    {
      return 0;
    }
  }

  std::lock_guard lock{dev->control_lock};
  switch(def->id)
  {
    case ControlId::AutoExposure:     *out = dev->auto_exposure ? 1. : 0.; break;
    case ControlId::ExposureTime:     *out = dev->exposure_us; break;
    case ControlId::Iso:              *out = dev->iso; break;
    case ControlId::AutoFocus:        *out = dev->auto_focus ? 1. : 0.; break;
    case ControlId::Focus:            *out = dev->focus; break;
    case ControlId::AutoWhiteBalance: *out = dev->auto_white_balance ? 1. : 0.; break;
    case ControlId::WhiteBalance:     *out = dev->white_balance; break;
    case ControlId::Brightness:       *out = dev->brightness; break;
    case ControlId::Contrast:         *out = dev->contrast; break;
    case ControlId::Saturation:       *out = dev->saturation; break;
    case ControlId::Sharpness:        *out = dev->sharpness; break;
    case ControlId::LumaDenoise:      *out = dev->luma_denoise; break;
    case ControlId::ChromaDenoise:    *out = dev->chroma_denoise; break;
    case ControlId::LaserPower:       *out = dev->laser_power; break;
    case ControlId::FloodLight:       *out = dev->flood_light; break;
    case ControlId::Temperature:      return 0; // handled above
  }
  return 1;
}

int backend_set_control(depthcam_device* dev, const char* id, double value)
{
  if(!dev || !id)
    return 0;

  const ControlDef* def = dev->findControl(id);
  if(!def || !(def->access & DEPTHCAM_ACCESS_WRITE))
    return 0;

  // Device calls, not pipeline messages.
  if(def->id == ControlId::LaserPower || def->id == ControlId::FloodLight)
  {
    const float v = float(std::clamp(value, 0., 1.));
    try
    {
      std::lock_guard lock{dev->control_lock};
      if(def->id == ControlId::LaserPower)
      {
        if(!dev->device->setIrLaserDotProjectorIntensity(v))
          return 0;
        dev->laser_power = v;
      }
      else
      {
        if(!dev->device->setIrFloodLightIntensity(v))
          return 0;
        dev->flood_light = v;
      }
      return 1;
    }
    catch(...)
    {
      return 0;
    }
  }

  std::lock_guard lock{dev->control_lock};
  switch(def->id)
  {
    case ControlId::AutoExposure:
      dev->auto_exposure = value != 0.;
      break;
    // Writing a manual value implies manual mode.
    case ControlId::ExposureTime:
      dev->exposure_us = std::clamp(value, def->min, def->max);
      dev->auto_exposure = false;
      break;
    case ControlId::Iso:
      dev->iso = std::clamp(value, def->min, def->max);
      dev->auto_exposure = false;
      break;
    case ControlId::AutoFocus:
      dev->auto_focus = value != 0.;
      break;
    case ControlId::Focus:
      dev->focus = std::clamp(value, def->min, def->max);
      dev->auto_focus = false;
      break;
    case ControlId::AutoWhiteBalance:
      dev->auto_white_balance = value != 0.;
      break;
    case ControlId::WhiteBalance:
      dev->white_balance = std::clamp(value, def->min, def->max);
      dev->auto_white_balance = false;
      break;
    case ControlId::Brightness:
      dev->brightness = std::clamp(value, def->min, def->max);
      break;
    case ControlId::Contrast:
      dev->contrast = std::clamp(value, def->min, def->max);
      break;
    case ControlId::Saturation:
      dev->saturation = std::clamp(value, def->min, def->max);
      break;
    case ControlId::Sharpness:
      dev->sharpness = std::clamp(value, def->min, def->max);
      break;
    case ControlId::LumaDenoise:
      dev->luma_denoise = std::clamp(value, def->min, def->max);
      break;
    case ControlId::ChromaDenoise:
      dev->chroma_denoise = std::clamp(value, def->min, def->max);
      break;
    default:
      return 0;
  }

  dev->applyCameraControl();
  return 1;
}

uint32_t backend_active_streams(depthcam_device* dev)
{
  // What was actually wired, not what was asked for.
  return dev ? dev->active : 0;
}

const depthcam_backend_v1 g_backend{
    .abi_version = DEPTHCAM_ABI_VERSION,
    .name = "depthai",
    .display_name = "Luxonis",
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
