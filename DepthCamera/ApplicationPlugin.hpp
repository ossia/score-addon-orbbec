#pragma once
#include <score/plugins/application/GUIApplicationPlugin.hpp>

#include <QString>

#include <vector>

#include <verdigris>

namespace Gfx::DepthCamera
{

/// Everything known about a camera without having opened it.
struct DeviceInfo
{
  QString backend;   ///< "orbbec", "freenect2", ...
  QString uri;       ///< what to store in the settings and hand back to open()
  QString name;
  QString serial;
  QString transport; ///< "usb3.2", "ethernet", ... ; may be empty
  uint32_t streams{};

  bool network() const noexcept;
  QString displayName() const;
};

class ApplicationPlugin
    : public QObject
    , public score::GUIApplicationPlugin
{
  W_OBJECT(ApplicationPlugin)
public:
  explicit ApplicationPlugin(const score::GUIApplicationContext& ctx);
  ~ApplicationPlugin();

  const std::vector<DeviceInfo>& devices() const noexcept { return m_devices; }

  /// displayName() with a " - 2", " - 3" ... suffix where two connected
  /// cameras would otherwise be called the same thing.
  ///
  /// Needed, not cosmetic: DeviceExplorerModel::checkDeviceInstantiatable
  /// refuses a device whose name matches one already in the document, so two
  /// identical cameras with identical names means the second cannot be added.
  /// Numbered rather than disambiguated by serial because the name is meant to
  /// stay short; the serial is in the uri, which is what identifies the camera.
  QString uniqueName(const DeviceInfo& dev) const;

  /// Re-read every backend's device list and emit the difference. Metadata
  /// only: no camera is opened.
  void rescan();

  void deviceAdded(DeviceInfo dev) W_SIGNAL(deviceAdded, dev);
  void deviceRemoved(QString uri) W_SIGNAL(deviceRemoved, uri);

private:
  std::vector<DeviceInfo> m_devices;
};
}

Q_DECLARE_METATYPE(Gfx::DepthCamera::DeviceInfo)
W_REGISTER_ARGTYPE(Gfx::DepthCamera::DeviceInfo)
