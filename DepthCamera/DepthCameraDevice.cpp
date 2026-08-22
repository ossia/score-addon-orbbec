#include "DepthCameraDevice.hpp"

#include <Gfx/GfxApplicationPlugin.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>

#include <DepthCamera/ApplicationPlugin.hpp>
#include <DepthCamera/Backend/BackendRegistry.hpp>
#include <DepthCamera/DepthCameraProtocol.hpp>
#include <DepthCamera/DepthCameraStream.hpp>

#include <State/Widgets/AddressFragmentLineEdit.hpp>

#include <wobjectimpl.h>

#include <map>

namespace Gfx::DepthCamera
{

class InputDevice final : public Gfx::GfxInputDevice
{
  W_OBJECT(InputDevice)
public:
  using GfxInputDevice::GfxInputDevice;
  ~InputDevice();

private:
  bool reconnect() override;
  void disconnect() override;
  Device::Node refresh() override;
  ossia::net::device_base* getDevice() const override { return m_dev.get(); }

  std::unique_ptr<ossia::net::device_base> m_dev;
};

}

W_OBJECT_IMPL(Gfx::DepthCamera::InputDevice)

namespace Gfx::DepthCamera
{

InputDevice::~InputDevice()
{
  disconnect();
}

Device::Node InputDevice::refresh()
{
  Device::Node score_device{settings(), nullptr};

  auto dev = getDevice();
  if(!dev)
    return score_device;

  // Device::ToDeviceExplorer rather than GfxInputDevice::refresh().
  //
  // The gfx one copies only the node names, which is all a device with nothing
  // but texture and geometry outputs has to say. The settings tree does have
  // more to say -- a type, a range, an access mode, a current value -- and
  // without those the explorer shows the controls as bare branches with no
  // editor. The shared version reads all of it, and it skips parameters whose
  // type has no editable value, so the streams still come out as before.
  const auto& children = dev->get_root_node().children();
  score_device.reserve(children.size());
  for(const auto& node : children)
    score_device.push_back(Device::ToDeviceExplorer(*node));

  score_device.get<Device::DeviceSettings>().name
      = QString::fromStdString(dev->get_name());
  return score_device;
}

void InputDevice::disconnect()
{
  // Before the base class, which clears the whole node tree: the settings tree
  // holds raw pointers to the parameters that live in it.
  if(auto* dev = static_cast<depthcam_device_impl*>(m_dev.get()))
    dev->releaseControls();

  GfxInputDevice::disconnect();

  // And then let the device go. Device::DeviceInterface::disconnect() only
  // clears the node tree, which leaves an object that reports itself connected
  // (connected() is getDevice() != nullptr) while having no streams at all, and
  // -- the part that actually bites -- still holding the camera open.
  //
  // That is what made an Azure Kinect show no picture until the user hit
  // Reconnect once. reconnect() calls this, then opens the camera before
  // assigning over m_dev, so the old handle was still there: k4a_device_open
  // returns LIBUSB_ERROR_BUSY for a device this process already has open, the
  // new device was never assigned, and reconnect() went on to return
  // connected() == true because the *old* one was still sitting in m_dev.
  // Score believed it had a camera; the camera had no nodes.
  m_dev.reset();
}

bool InputDevice::reconnect()
{
  disconnect();

  try
  {
    const auto set
        = this->settings().deviceSpecificSettings.value<DepthCameraSettings>();

    auto plug = m_ctx.findPlugin<Gfx::DocumentPlugin>();
    if(!plug)
      return false;

    auto& registry = BackendRegistry::instance();
    registry.load();

    // An empty address means "whatever is there": fall back to the first
    // backend that reports a camera, so a document with no address still works
    // on a machine with one camera.
    QString uri = set.device.trimmed();
    if(uri.isEmpty())
    {
      registry.enumerate([&](const depthcam_device_info& d) {
        if(uri.isEmpty() && d.uri)
          uri = QString::fromUtf8(d.uri);
      });
      if(uri.isEmpty())
      {
        qDebug() << "[depthcam] no camera found";
        return false;
      }
    }

    const auto backend_name = BackendRegistry::backendOf(uri);
    const auto* backend = registry.find(backend_name);
    if(!backend)
    {
      qDebug() << "[depthcam] no backend named" << backend_name
               << "is installed; cannot open" << uri;
      return false;
    }

    auto proto = std::make_unique<depthcam_protocol>(backend, uri, set);
    if(!proto->stream || !proto->stream->valid())
      return false;

    m_dev = std::make_unique<depthcam_device_impl>(
        set, plug->exec, std::move(proto), this->settings().name.toStdString());
  }
  catch(const std::exception& e)
  {
    qDebug() << "[depthcam] could not connect:" << e.what();
  }
  catch(...)
  {
    qDebug() << "[depthcam] could not connect: unknown error";
  }

  return connected();
}

QString InputFactory::prettyName() const noexcept
{
  return QObject::tr("Depth Camera Input");
}

QString InputFactory::category() const noexcept
{
  return StandardCategories::video_in;
}

QUrl InputFactory::manual() const noexcept
{
  return QUrl("https://ossia.io/score-docs/devices/depth-camera-device.html");
}

Device::DeviceInterface* InputFactory::makeDevice(
    const Device::DeviceSettings& settings, const Explorer::DeviceDocumentPlugin& plugin,
    const score::DocumentContext& ctx)
{
  return new InputDevice(settings, ctx);
}

const Device::DeviceSettings& InputFactory::defaultSettings() const noexcept
{
  static const Device::DeviceSettings settings = [] {
    Device::DeviceSettings s;
    s.protocol = static_concreteKey();
    s.name = "Camera";
    s.deviceSpecificSettings = QVariant::fromValue(DepthCameraSettings{});
    return s;
  }();
  return settings;
}

QVariant
InputFactory::makeProtocolSpecificSettings(const VisitorVariant& visitor) const
{
  return makeProtocolSpecificSettings_T<DepthCameraSettings>(visitor);
}

void InputFactory::serializeProtocolSpecificSettings(
    const QVariant& data, const VisitorVariant& visitor) const
{
  serializeProtocolSpecificSettings_T<DepthCameraSettings>(data, visitor);
}

/**
 * @brief Lists the cameras one backend knows about.
 *
 * One enumerator per backend rather than per transport: the device browser turns
 * each into a category header, and a category per USB speed produced eight
 * permanently empty ones.
 */
class DepthCameraEnumerator final : public Device::DeviceEnumerator
{
public:
  DepthCameraEnumerator(const score::GUIApplicationContext& ctx, QString backend)
      : m_plugin{ctx.guiApplicationPlugin<DepthCamera::ApplicationPlugin>()}
      , m_backend{std::move(backend)}
  {
    connect(
        &m_plugin, &ApplicationPlugin::deviceAdded, this,
        [this](const DeviceInfo& dev) {
      if(dev.backend != m_backend)
        return;
      m_names[dev.uri] = dev.displayName();
      deviceAdded(dev.displayName(), settingsFor(dev));
    });

    connect(
        &m_plugin, &ApplicationPlugin::deviceRemoved, this, [this](const QString& uri) {
      // Removal is by display name, so we have to remember what we announced:
      // by the time it arrives the camera is gone and cannot be queried.
      if(auto it = m_names.find(uri); it != m_names.end())
      {
        deviceRemoved(it->second);
        m_names.erase(it);
      }
    });
  }

  Device::DeviceSettings settingsFor(const DeviceInfo& dev) const
  {
    Device::DeviceSettings set;
    set.name = dev.displayName();
    set.protocol = InputFactory::static_concreteKey();

    DepthCameraSettings specif;
    specif.device = dev.uri;
    set.deviceSpecificSettings = QVariant::fromValue(specif);
    return set;
  }

  void enumerate(std::function<void(const QString&, const Device::DeviceSettings&)> f)
      const override
  {
    for(const auto& dev : m_plugin.devices())
    {
      if(dev.backend != m_backend)
        continue;
      const_cast<DepthCameraEnumerator*>(this)->m_names[dev.uri] = dev.displayName();
      f(dev.displayName(), settingsFor(dev));
    }
  }

private:
  const ApplicationPlugin& m_plugin;
  QString m_backend;
  std::map<QString, QString> m_names;
};

Device::DeviceEnumerators
InputFactory::getEnumerators(const score::DocumentContext& ctx) const
{
  auto& registry = BackendRegistry::instance();
  registry.load();

  Device::DeviceEnumerators es;
  for(const auto& b : registry.backends())
    es.push_back(
        {b.display_name, new DepthCameraEnumerator{ctx.app, b.name}});
  return es;
}

Device::ProtocolSettingsWidget* InputFactory::makeSettingsWidget()
{
  return new InputSettingsWidget;
}

InputSettingsWidget::InputSettingsWidget(QWidget* parent)
    : Device::ProtocolSettingsWidget{parent}
{
  auto layout = new QFormLayout;

  m_deviceNameEdit = new State::AddressFragmentLineEdit{this};
  checkForChanges(m_deviceNameEdit);
  layout->addRow(tr("Device name"), m_deviceNameEdit);

  // Editable on purpose: picking a camera in the browser fills this in, and
  // typing one by hand is simply the case where enumeration was skipped -- which
  // is how a networked camera, or one that is not broadcasting, gets connected.
  m_address = new QLineEdit{this};
  m_address->setPlaceholderText(
      tr("empty for the first camera, or e.g. orbbec:sn:SERIAL, "
         "orbbec:net:192.168.0.12:8090, freenect2:sn:SERIAL, k4a:index:0"));
  checkForChanges(m_address);
  layout->addRow(tr("Camera"), m_address);

  // A network camera by address, without having to know the URI syntax.
  //
  // Discovery is a GVCP broadcast, and the reply comes back addressed to
  // 255.255.255.255 -- the firmware ignores the "unicast acknowledge" flag -- so
  // any host firewall that drops broadcast input hides every networked camera on
  // the segment while the cameras themselves are answering perfectly well. ufw
  // does exactly that by default. A camera on another subnet is out of reach of
  // a broadcast regardless. Either way, typing the address is the way in.
  m_network = new QGroupBox{tr("Connect over the network"), this};
  m_network->setCheckable(true);
  m_network->setChecked(false);
  {
    auto net_layout = new QFormLayout{m_network};
    m_networkHost = new QLineEdit{this};
    m_networkHost->setPlaceholderText(tr("192.168.0.12"));
    net_layout->addRow(tr("Host"), m_networkHost);

    m_networkPort = new QSpinBox{this};
    m_networkPort->setRange(1, 65535);
    m_networkPort->setValue(8090);
    net_layout->addRow(tr("Port"), m_networkPort);

    auto note = new QLabel{
        tr("Orbbec only (Femto Mega). The camera also has to be reachable: "
           "automatic discovery needs UDP broadcast to arrive, which a host "
           "firewall usually blocks."),
        this};
    note->setWordWrap(true);
    net_layout->addRow(note);
  }
  // No checkForChanges overload for a QGroupBox; the signal is the same.
  connect(
      m_network, &QGroupBox::toggled, this, &Device::ProtocolSettingsWidget::changed);
  checkForChanges(m_networkHost);
  checkForChanges(m_networkPort);
  layout->addRow(m_network);

  connect(m_network, &QGroupBox::toggled, this, [this] { updateEnabledState(); });
  connect(
      m_networkHost, &QLineEdit::textChanged, this, [this] { updateEnabledState(); });
  connect(
      m_networkPort, &QSpinBox::valueChanged, this, [this] { updateEnabledState(); });

  // Say so rather than presenting an empty browser and letting the user wonder.
  if(BackendRegistry::instance().backends().empty())
  {
    auto warn = new QLabel{
        tr("No camera backend is installed.\nInstall one from the package "
           "manager to use this device."),
        this};
    warn->setWordWrap(true);
    layout->addRow(warn);
  }

  auto streams = new QGroupBox{tr("Streams"), this};
  auto streams_layout = new QFormLayout{streams};
  streams_layout->addRow(m_rgb = new QCheckBox{tr("Color"), this});
  streams_layout->addRow(m_ir = new QCheckBox{tr("Infrared"), this});
  streams_layout->addRow(m_depth = new QCheckBox{tr("Depth"), this});
  streams_layout->addRow(m_pointcloud = new QCheckBox{tr("Point cloud"), this});
  streams_layout->addRow(
      m_colorPointcloud = new QCheckBox{tr("Colored point cloud"), this});

  // Alignment decides the point cloud's resolution, which dominates its cost:
  // on a Femto Mega, depth-to-colour is 2.07M points per frame against 369k the
  // other way. Which trade-off is right depends on the piece.
  m_align = new QComboBox{this};
  m_align->addItem(tr("None"), int(DepthCameraSettings::AlignMode::None));
  m_align->addItem(
      tr("Depth to color (color resolution)"),
      int(DepthCameraSettings::AlignMode::DepthToColor));
  m_align->addItem(
      tr("Color to depth (depth resolution)"),
      int(DepthCameraSettings::AlignMode::ColorToDepth));
  checkForChanges(m_align);
  streams_layout->addRow(tr("Alignment"), m_align);

  layout->addRow(streams);

  const auto makeSpin = [this] {
    auto sb = new QSpinBox{this};
    sb->setRange(0, 8192);
    sb->setSpecialValueText(tr("Any"));
    checkForChanges(sb);
    return sb;
  };

  auto color = new QGroupBox{tr("Color format"), this};
  auto color_layout = new QFormLayout{color};
  color_layout->addRow(tr("Width"), m_colorWidth = makeSpin());
  color_layout->addRow(tr("Height"), m_colorHeight = makeSpin());
  color_layout->addRow(tr("FPS"), m_colorFps = makeSpin());
  layout->addRow(color);

  auto depth = new QGroupBox{tr("Depth format"), this};
  auto depth_layout = new QFormLayout{depth};
  depth_layout->addRow(tr("Width"), m_depthWidth = makeSpin());
  depth_layout->addRow(tr("Height"), m_depthHeight = makeSpin());
  depth_layout->addRow(tr("FPS"), m_depthFps = makeSpin());
  layout->addRow(depth);

  for(auto* cb : {m_rgb, m_ir, m_depth, m_pointcloud, m_colorPointcloud})
  {
    checkForChanges(cb);
    connect(cb, &QCheckBox::toggled, this, [this] { updateEnabledState(); });
  }
  connect(
      m_align, &QComboBox::currentIndexChanged, this,
      [this] { updateEnabledState(); });

  setLayout(layout);
  setSettings(InputFactory{}.defaultSettings());
}

QString InputSettingsWidget::currentAddress() const
{
  if(!m_network->isChecked())
    return m_address->text();

  const auto host = m_networkHost->text().trimmed();
  if(host.isEmpty())
    return {};

  // The backend takes an IPv6 literal in brackets, so a raw one has to be
  // wrapped before the port is appended -- otherwise the last colon of the
  // address reads as the port separator.
  const auto quoted
      = host.contains(':') && !host.startsWith('[') ? "[" + host + "]" : host;
  return QStringLiteral("orbbec:net:%1:%2").arg(quoted).arg(m_networkPort->value());
}

void InputSettingsWidget::updateEnabledState()
{
  // One source of truth: with the network group on, the address is composed
  // from it, so showing the result read-only is clearer than leaving a second
  // editable copy that silently loses.
  const bool net = m_network->isChecked();
  m_address->setReadOnly(net);
  if(net)
    m_address->setText(currentAddress());

  const bool aligned
      = m_align->currentData().toInt() != int(DepthCameraSettings::AlignMode::None);
  // A coloured cloud needs depth and colour in one frame of reference; without
  // an alignment a backend would be asked to read mismatched buffers.
  const bool can_color = m_pointcloud->isChecked() && aligned;
  m_colorPointcloud->setEnabled(can_color);

  // Uncheck, not just disable: a disabled QCheckBox keeps its checked state and
  // getSettings() reads isChecked(), so greying it out alone would still let the
  // invalid combination through.
  if(!can_color && m_colorPointcloud->isChecked())
    m_colorPointcloud->setChecked(false);
}

Device::DeviceSettings InputSettingsWidget::getSettings() const
{
  Device::DeviceSettings s = m_settings;
  s.name = m_deviceNameEdit->text();
  s.protocol = InputFactory::static_concreteKey();

  DepthCameraSettings set;
  set.device = currentAddress();
  set.rgb = m_rgb->isChecked();
  set.ir = m_ir->isChecked();
  set.depth = m_depth->isChecked();
  set.pointcloud = m_pointcloud->isChecked();
  set.colorPointcloud = m_colorPointcloud->isChecked();
  set.align
      = static_cast<DepthCameraSettings::AlignMode>(m_align->currentData().toInt());
  set.colorWidth = m_colorWidth->value();
  set.colorHeight = m_colorHeight->value();
  set.colorFps = m_colorFps->value();
  set.depthWidth = m_depthWidth->value();
  set.depthHeight = m_depthHeight->value();
  set.depthFps = m_depthFps->value();

  s.deviceSpecificSettings = QVariant::fromValue(set);
  return s;
}

void InputSettingsWidget::setSettings(const Device::DeviceSettings& settings)
{
  m_settings = settings;
  m_deviceNameEdit->setText(settings.name);

  const auto set = settings.deviceSpecificSettings.value<DepthCameraSettings>();
  m_address->setText(set.device);

  // Split a network address back into the fields it came from, so an existing
  // device opens the dialog looking the way it was set up rather than as a URI.
  m_network->setChecked(false);
  if(static const QString prefix{"orbbec:net:"}; set.device.startsWith(prefix))
  {
    auto rest = set.device.mid(prefix.size());
    QString host = rest;
    int port = 8090;

    if(rest.startsWith('['))
    {
      // "[::1]:8090"
      if(const auto close = rest.indexOf(']'); close > 0)
      {
        host = rest.mid(1, close - 1);
        if(const auto colon = rest.indexOf(':', close); colon > 0)
          port = rest.mid(colon + 1).toInt();
      }
    }
    else if(const auto colon = rest.lastIndexOf(':'); colon > 0)
    {
      bool ok = false;
      if(const int p = rest.mid(colon + 1).toInt(&ok); ok && p > 0 && p <= 65535)
      {
        host = rest.left(colon);
        port = p;
      }
    }

    if(!host.isEmpty())
    {
      m_network->setChecked(true);
      m_networkHost->setText(host);
      m_networkPort->setValue(port);
    }
  }
  m_rgb->setChecked(set.rgb);
  m_ir->setChecked(set.ir);
  m_depth->setChecked(set.depth);
  m_pointcloud->setChecked(set.pointcloud);
  m_colorPointcloud->setChecked(set.colorPointcloud);
  if(const int idx = m_align->findData(int(set.align)); idx >= 0)
    m_align->setCurrentIndex(idx);
  m_colorWidth->setValue(set.colorWidth);
  m_colorHeight->setValue(set.colorHeight);
  m_colorFps->setValue(set.colorFps);
  m_depthWidth->setValue(set.depthWidth);
  m_depthHeight->setValue(set.depthHeight);
  m_depthFps->setValue(set.depthFps);

  updateEnabledState();
}

}
