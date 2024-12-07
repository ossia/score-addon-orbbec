#include "OrbbecInputDevice.hpp"
#include <Orbbec/ApplicationPlugin.hpp>
#include <Orbbec/OrbbecInputStream.hpp>

#include <Gfx/GfxApplicationPlugin.hpp>

#include <QLabel>
#include <QFormLayout>

#include <wobjectimpl.h>
/*
#include <Device/Protocol/DeviceInterface.hpp>
#include <Device/Protocol/DeviceSettings.hpp>


#include <ossia/gfx/texture_parameter.hpp>
#include <ossia/network/base/device.hpp>
#include <ossia/network/base/protocol.hpp>
#include <QLineEdit>
*/


/*

#include <State/Widgets/AddressFragmentLineEdit.hpp>

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
#include <magic_enum/magic_enum.hpp>

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
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}
*/

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
        // for(int i = 0; i < (int)deviceList->deviceCount(); i++)
        // {
        //   auto dev = deviceList->getDevice(i);
        //   auto dev_info = dev->getDeviceInfo();
        //   if(dev_info->serialNumber() == set.path)
        //   {
        //     device = dev;
        //     break;
        //   }
        // }

        if(!device && deviceList->deviceCount() > 0)
          device = deviceList->getDevice(0);
        if(!device)
          return false;
      }

      auto sensorList = device->getSensorList();

//      config->enableAllStream();
      config->enableVideoStream(OB_SENSOR_COLOR, OB_WIDTH_ANY, OB_HEIGHT_ANY, OB_FPS_ANY,
                                OB_FORMAT_ANY);
      config->enableVideoStream(OB_SENSOR_DEPTH, OB_WIDTH_ANY, OB_HEIGHT_ANY, OB_FPS_ANY,
                                OB_FORMAT_ANY);
      config->enableVideoStream(OB_SENSOR_IR, OB_WIDTH_ANY, OB_HEIGHT_ANY, OB_FPS_ANY,
                                OB_FORMAT_ANY);
      for(int i = 0; i < (int)sensorList->getCount(); i++)
      {
        auto type = sensorList->getSensorType(i);
        if(ob::TypeHelper::isVideoSensorType(type))
        {

        }
        else if(type == OB_SENSOR_DEPTH)
        {

        }
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
      , orbbec{ctx.guiApplicationPlugin<Orbbec::ApplicationPlugin>().orbbec}
  {
  }

  void enumerate(std::function<void(const QString&, const Device::DeviceSettings&)> f)
      const override
  {
    auto deviceList = orbbec.queryDeviceList();
    for(uint32_t index = 0, N = deviceList->getCount(); index < N; index++)
    {
      auto device = deviceList->getDevice(index);
      auto deviceInfo = device->getDeviceInfo();

      Device::DeviceSettings set;
      SharedInputSettings specif;
      set.name = QString("%1 (%2)")
                     .arg(deviceInfo->getName())
                     .arg(deviceInfo->getSerialNumber());
      set.name.remove("orbbec ", Qt::CaseInsensitive);
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
