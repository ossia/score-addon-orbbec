#include "OrbbecInputDevice.hpp"

#include "Orbbec/ApplicationPlugin.hpp"

#include <State/Widgets/AddressFragmentLineEdit.hpp>

#include <Gfx/GfxApplicationPlugin.hpp>
#include <Gfx/GfxExecContext.hpp>
#include <Gfx/Graph/VideoNode.hpp>
#include <Video/CameraInput.hpp>
#include <Video/FrameQueue.hpp>
#include <Video/GStreamerCompatibility.hpp>
#include <Video/GpuFormats.hpp>
#include <Video/VideoInterface.hpp>

#include <score/serialization/MimeVisitor.hpp>

#include <ossia/detail/flicks.hpp>
#include <ossia/detail/fmt.hpp>

#include <ossia-qt/name_utils.hpp>

#include <QComboBox>
#include <QDebug>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QLabel>
#include <QMenu>
#include <QMimeData>

#include <wobjectimpl.h>

#include <functional>

extern "C" {
#include <libavformat/avformat.h>
}

#include <libobsensor/ObSensor.hpp>

namespace Gfx::Orbbec
{

class InputDevice final : public Gfx::GfxInputDevice
{
  W_OBJECT(InputDevice)
public:
  using GfxInputDevice::GfxInputDevice;
  ~InputDevice();

private:
  bool reconnect() override;
  ossia::net::device_base* getDevice() const override { return m_dev.get(); }

  Gfx::video_texture_input_protocol* m_protocol{};
  mutable std::unique_ptr<Gfx::video_texture_input_device> m_dev;
};

}

W_OBJECT_IMPL(Gfx::Orbbec::InputDevice)

namespace Gfx::Orbbec
{

class InputStream final : public ::Video::ExternalInput
{
public:
  explicit InputStream(
      std::shared_ptr<ob::Config> conf, std::shared_ptr<ob::Device> device) noexcept
      : m_config{conf}
      , m_device{device}
  {
    realTime = true;
  }

  ~InputStream() noexcept { stop(); }

  bool start() noexcept override
  {
    if(m_running)
      return false;

    m_pipeline.start(m_config, [&](std::shared_ptr<ob::FrameSet> output) {
      {
        std::lock_guard<std::mutex> lock(m_frames_mtx);
        using namespace std;
        m_frameset.swap(output);
      }
      on_data();
    });

    m_running.store(true, std::memory_order_release);
    return true;
  }

  void stop() noexcept override
  {
    // Stop the running status
    m_running.store(false, std::memory_order_release);

    m_pipeline.stop();

    // Remove frames that were in flight
    m_frames.drain();
  }

  AVFrame* dequeue_frame() noexcept override { return m_frames.dequeue(); }

  void release_frame(AVFrame* frame) noexcept override { m_frames.release(frame); }

private:
  AVPixelFormat get_pixelfmt(OBFormat fmt)
  {
    switch(fmt)
    {
      case OB_FORMAT_YUYV:
      case OB_FORMAT_YUY2:
      case OB_FORMAT_GRAY:
        return AVPixelFormat::AV_PIX_FMT_YUYV422;
        return AVPixelFormat::AV_PIX_FMT_YUYV422;
      case OB_FORMAT_UYVY:
        return AVPixelFormat::AV_PIX_FMT_UYVY422;
      case OB_FORMAT_NV12:
        return AVPixelFormat::AV_PIX_FMT_NV12;
      case OB_FORMAT_NV21:
        return AVPixelFormat::AV_PIX_FMT_NV21;
      case OB_FORMAT_MJPG:
        return AVPixelFormat::AV_PIX_FMT_YUVJ420P;
      case OB_FORMAT_Y16:
      case OB_FORMAT_RLE:
      case OB_FORMAT_RVL:
      case OB_FORMAT_Z16:
        return AVPixelFormat::AV_PIX_FMT_GRAY16LE;

      case OB_FORMAT_Y8:
        return AVPixelFormat::AV_PIX_FMT_GRAY8;

      // Y10..14: SDK will unpack into Y16 by default
      case OB_FORMAT_Y10:
        return AVPixelFormat::AV_PIX_FMT_GRAY16LE;
      case OB_FORMAT_Y12:
      case OB_FORMAT_YV12: // Y12: left / YV12: right
        return AVPixelFormat::AV_PIX_FMT_GRAY16LE;
      case OB_FORMAT_Y14:
        return AVPixelFormat::AV_PIX_FMT_GRAY16LE;

      case OB_FORMAT_I420:
        return AVPixelFormat::AV_PIX_FMT_YUV420P;
      case OB_FORMAT_RGB:
        return AV_PIX_FMT_RGB24;
      case OB_FORMAT_BGR:
        return AV_PIX_FMT_BGR24;
      case OB_FORMAT_BGRA:
        return AV_PIX_FMT_BGRA;
      case OB_FORMAT_RGBA:
        return AV_PIX_FMT_RGBA;

      case OB_FORMAT_BYR2: // V4L2_PIX_FMT_SBGGR8 / bayer
        return AV_PIX_FMT_BAYER_BGGR8;

      case OB_FORMAT_UNKNOWN:
      case OB_FORMAT_Y11:
      case OB_FORMAT_H264:
      case OB_FORMAT_H265:
      case OB_FORMAT_HEVC:
      case OB_FORMAT_ACCEL:
      case OB_FORMAT_GYRO:
      case OB_FORMAT_POINT:
      case OB_FORMAT_RGB_POINT:
      case OB_FORMAT_COMPRESSED:
      case OB_FORMAT_BA81: // V4L2_PIX_FMT_SBGGR8 / bayer... but orbbec doc says "Is same as Y8, using for right ir stream" ???
      case OB_FORMAT_RW16:
      default:
        return AV_PIX_FMT_NONE;
    }
    return AV_PIX_FMT_NONE;
  }

  void on_data()
  {
    if(!m_running)
      return;
    if(!m_frameset)
      return;
    if(!m_frameset->colorFrame())
      return;

    auto& color = *m_frameset->colorFrame();
    void* p = color.data();
    std::size_t sz = color.dataSize();
    if(sz <= 1)
      return;

    auto pixfmt = get_pixelfmt(color.getFormat());
    if(pixfmt == AV_PIX_FMT_NONE)
      return;

    ::Video::AVFramePointer frame = m_frames.newFrame();

    frame->format = pixfmt;
    frame->width = color.getWidth();
    frame->height = color.getHeight();

    // Here we need to copy the buffer.
    const auto storage = ::Video::initFrameBuffer(*frame, sz);
    if(::Video::initFrameFromRawData(frame.get(), storage, sz))
    {
      // Copy the content as we're going on *adventures*
      memcpy(storage, p, sz);

      m_frames.enqueue(frame.release());
    }
  }

  ::Video::FrameQueue m_frames;

  std::atomic_bool m_running{};

  std::shared_ptr<ob::Config> m_config;
  std::shared_ptr<ob::Device> m_device;
  ob::Pipeline m_pipeline;
  std::mutex m_frames_mtx;
  std::shared_ptr<ob::FrameSet> m_frameset = nullptr;
};

InputDevice::~InputDevice() { }

bool InputDevice::reconnect()
{
  disconnect();

  try
  {
    auto set = this->settings().deviceSpecificSettings.value<SharedInputSettings>();

    auto plug = m_ctx.findPlugin<Gfx::DocumentPlugin>();
    auto& orbbec_app = m_ctx.app.guiApplicationPlugin<Orbbec::ApplicationPlugin>();
    if(plug)
    {
      auto config = std::make_shared<ob::Config>();
      auto device = std::shared_ptr<ob::Device>{};

      {
        auto deviceList = orbbec_app.orbbec.queryDeviceList();
        for(int i = 0; i < (int)deviceList->deviceCount(); i++)
        {
          auto dev = deviceList->getDevice(i);
          auto dev_info = dev->getDeviceInfo();
          if(dev_info->serialNumber() == set.path)
          {
            device = dev;
            break;
          }
        }

        if(!device && deviceList->deviceCount() > 0)
          device = deviceList->getDevice(0);
        if(!device)
          return false;
      }

      auto sensorList = device->getSensorList();

      for(int i = 0; i < (int)sensorList->getCount(); i++)
      {
        auto type = sensorList->getSensorType(i);
        if(ob::TypeHelper::isVideoSensorType(type))
          config->enableStream(type);
      }
      auto stream = std::make_shared<InputStream>(config, device);

      m_protocol = new Gfx::video_texture_input_protocol{std::move(stream), plug->exec};
      m_dev = std::make_unique<Gfx::video_texture_input_device>(
          std::unique_ptr<ossia::net::protocol_base>(m_protocol),
          this->settings().name.toStdString());
    }
  }
  catch(std::exception& e)
  {
    qDebug() << "Could not connect: " << e.what();
  }
  catch(...)
  {
    // TODO save the reason of the non-connection.
  }

  return connected();
}

QString InputFactory::prettyName() const noexcept
{
  return QObject::tr("Orbbec Input");
}

QUrl InputFactory::manual() const noexcept
{
  return QUrl("https://ossia.io/score-docs/devices/orbbec-device.html");
}

Device::DeviceInterface* InputFactory::makeDevice(
    const Device::DeviceSettings& settings, const Explorer::DeviceDocumentPlugin& plugin,
    const score::DocumentContext& ctx)
{
  return new InputDevice(settings, ctx);
}

const Device::DeviceSettings& InputFactory::defaultSettings() const noexcept
{
  static const Device::DeviceSettings settings = [&]() {
    Device::DeviceSettings s;
    s.protocol = concreteKey();
    s.name = "Orbbec Input";
    SharedInputSettings specif;
    specif.path = "";
    s.deviceSpecificSettings = QVariant::fromValue(specif);
    return s;
  }();
  return settings;
}

class OrbbecEnumerator : public Device::DeviceEnumerator
{
public:
  const score::GUIApplicationContext& context;
  const ob::Context& orbbec;

  explicit OrbbecEnumerator(const score::GUIApplicationContext& ctx)
      : context{ctx}
      , orbbec{ctx.applicationPlugin<Orbbec::ApplicationPlugin>().orbbec}
  {
  }

  void enumerate(std::function<void(const QString&, const Device::DeviceSettings&)> f)
      const override
  {
    auto deviceList = orbbec.queryDeviceList();
    for(uint32_t index = 0; index < deviceList->getCount(); index++)
    {
      auto device = deviceList->getDevice(index);
      auto deviceInfo = device->getDeviceInfo();

      Device::DeviceSettings set;
      SharedInputSettings specif;
      set.name = QString("%1 (%2)")
                     .arg(deviceInfo->getName())
                     .arg(deviceInfo->getSerialNumber());
      set.protocol = InputFactory::static_concreteKey();
      specif.path = deviceInfo->getSerialNumber();
      set.deviceSpecificSettings = QVariant::fromValue(specif);
      f(set.name, set);
    }
  }
};

Device::DeviceEnumerators
InputFactory::getEnumerators(const score::DocumentContext& ctx) const
{
  return {{"Cameras", new OrbbecEnumerator{ctx.app}}};
}

Device::ProtocolSettingsWidget* InputFactory::makeSettingsWidget()
{
  return new InputSettingsWidget;
}

InputSettingsWidget::InputSettingsWidget(QWidget* parent)
    : SharedInputSettingsWidget{parent}
{
  m_deviceNameEdit->setText("Orbbec In");
  ((QLabel*)m_layout->labelForField(m_shmPath))->setText("Orbbec label");
  setSettings(InputFactory{}.defaultSettings());
}

Device::DeviceSettings InputSettingsWidget::getSettings() const
{
  auto set = SharedInputSettingsWidget::getSettings();
  set.protocol = InputFactory::static_concreteKey();
  return set;
}

}
