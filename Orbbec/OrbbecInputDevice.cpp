#include "OrbbecInputDevice.hpp"

#include <Gfx/GfxApplicationPlugin.hpp>

#include <QFormLayout>
#include <QLabel>

#include <Orbbec/ApplicationPlugin.hpp>
#include <Orbbec/OrbbecInputStream.hpp>
#include <Orbbec/OrbbecProtocol.hpp>

#include <wobjectimpl.h>

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

  ossia::net::protocol_base* m_protocol;
  std::unique_ptr<ossia::net::device_base> m_dev;
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
        qDebug() << "Count?" << (int)deviceList->deviceCount();
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
      auto proto = std::make_unique<orbbec_protocol>(config, device, set);
      m_protocol = proto.get();
      m_dev = std::make_unique<orbbec_device>(
          set, plug->exec, std::move(proto), this->settings().name.toStdString());
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
  const ApplicationPlugin& orbbec;
  std::string connection_type;

  explicit OrbbecEnumerator(
      const score::GUIApplicationContext& ctx, std::string connection_type)
      : context{ctx}
      , orbbec{ctx.guiApplicationPlugin<Orbbec::ApplicationPlugin>()}
      , connection_type{connection_type}
  {
    connect(
        &orbbec, &ApplicationPlugin::deviceAdded, this,
        [this](std::shared_ptr<ob::Device> dev) {
      if(auto set = deviceInfo(dev))
        deviceAdded(set->name, *set);
    });
    connect(
        &orbbec, &ApplicationPlugin::deviceRemoved, this,
        [this](std::shared_ptr<ob::Device> dev) { deviceRemoved(deviceName(dev)); });
  }

  static QString deviceName(std::shared_ptr<ob::Device> device)
  {
    return deviceName(device->getDeviceInfo());
  }
  static QString deviceName(std::shared_ptr<ob::DeviceInfo> deviceInfo)
  {
    auto name = QString("%1 (%2)")
                    .arg(deviceInfo->getName())
                    .arg(deviceInfo->getSerialNumber());
    name.remove("orbbec ", Qt::CaseInsensitive);
    return name;
  }

  std::optional<Device::DeviceSettings>
  deviceInfo(std::shared_ptr<ob::Device> device) const
  {
    auto deviceInfo = device->getDeviceInfo();
    qDebug() << "Got device: " << deviceInfo->connectionType();
    if(deviceInfo->connectionType() != connection_type)
      return std::nullopt;

    Device::DeviceSettings set;
    SharedInputSettings specif;
    set.name = deviceName(deviceInfo);
    set.protocol = InputFactory::static_concreteKey();
    specif.path = deviceInfo->getSerialNumber();
    set.deviceSpecificSettings = QVariant::fromValue(specif);
    return set;
  }

  void enumerate(std::function<void(const QString&, const Device::DeviceSettings&)> f)
      const override
  {
    qDebug() << "oy: " << this->orbbec.m_known_devices.size();
    for(auto& dev : this->orbbec.m_known_devices)
    {
      if(auto set = deviceInfo(dev))
        f(set->name, *set);
    }
  }
};

Device::DeviceEnumerators
InputFactory::getEnumerators(const score::DocumentContext& ctx) const
{

  Device::DeviceEnumerators es;
  for(const char* con :
      {"USB", "USB1.0", "USB1.1", "USB2.0", "USB2.1", "USB3.0", "USB3.1", "USB3.2",
       "Ethernet"})
  {
    es.push_back({con, new OrbbecEnumerator{ctx.app, con}});
  }
  return es;
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
