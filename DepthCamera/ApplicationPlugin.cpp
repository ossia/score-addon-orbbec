#include "ApplicationPlugin.hpp"

#include <DepthCamera/Backend/BackendRegistry.hpp>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>

#include <wobjectimpl.h>

#include <algorithm>

W_OBJECT_IMPL(Gfx::DepthCamera::ApplicationPlugin)

namespace Gfx::DepthCamera
{
namespace
{
/**
 * @brief Explain an empty device list on Linux.
 *
 * Every one of these SDKs talks to the camera over raw USB, and without the
 * matching udev rules the device nodes stay root-owned. The SDK then reports
 * zero cameras, which is indistinguishable from "nothing is plugged in".
 */
void warnIfMissingUdevRules()
{
#if defined(__linux__)
  static bool warned = false;
  if(std::exchange(warned, true))
    return;

  for(const auto* dir :
      {"/etc/udev/rules.d", "/usr/lib/udev/rules.d", "/lib/udev/rules.d"})
  {
    const QDir d{QString::fromUtf8(dir)};
    if(!d.exists())
      continue;
    if(!d.entryList(
             {QStringLiteral("*obsensor*"), QStringLiteral("*freenect*"),
              QStringLiteral("*k4a*")},
             QDir::Files)
            .isEmpty())
      return; // some rules are installed; the list really is empty
  }

  qDebug().noquote()
      << "[depthcam] no camera found, and no depth-camera udev rules appear to\n"
         "           be installed. Without them the USB device nodes stay\n"
         "           root-owned and no SDK can see any camera. Each backend\n"
         "           package ships the rules for its own hardware.";
#endif
}
}

bool DeviceInfo::network() const noexcept
{
  return transport.compare(QLatin1String("ethernet"), Qt::CaseInsensitive) == 0;
}

QString DeviceInfo::displayName() const
{
  // The model, and nothing else: the browser groups by vendor already, and the
  // serial belongs in the uri, which is what actually identifies the camera.
  if(name.isEmpty())
    return backend.isEmpty() ? QStringLiteral("Camera") : backend;
  return name;
}

QString ApplicationPlugin::uniqueName(const DeviceInfo& dev) const
{
  const QString base = dev.displayName();

  int seen = 0;
  for(const auto& other : m_devices)
  {
    if(other.uri == dev.uri)
      break;
    if(other.displayName() == base)
      seen++;
  }

  return seen == 0 ? base : QStringLiteral("%1 - %2").arg(base).arg(seen + 1);
}

ApplicationPlugin::ApplicationPlugin(const score::GUIApplicationContext& ctx)
    : score::GUIApplicationPlugin{ctx}
{
  auto& registry = BackendRegistry::instance();
  registry.load();

  registry.setChangedCallback([this] {
    // Called from a backend thread; re-read on the UI thread.
    QMetaObject::invokeMethod(this, [this] { rescan(); }, Qt::QueuedConnection);
  });

  // The SDK contexts keep polling threads alive and must be destroyed before
  // the backend libraries' globals are; doing it from a static destructor at
  // exit() is too late and segfaults. aboutToQuit is the last point at which
  // the process is unambiguously healthy: the event loop has returned, every
  // document (and therefore every open camera) is closed.
  if(qApp)
    connect(
        qApp, &QCoreApplication::aboutToQuit, this,
        [] { BackendRegistry::instance().shutdown(); });

  // Hot-plug notifications only cover changes, so anything already connected at
  // startup would otherwise never be reported.
  rescan();
}

ApplicationPlugin::~ApplicationPlugin()
{
  BackendRegistry::instance().setChangedCallback({});

  // Idempotent, and covers the paths that never reach aboutToQuit -- headless
  // runs, and anything that tears the plug-ins down without exec().
  BackendRegistry::instance().shutdown();
}

void ApplicationPlugin::rescan()
{
  std::vector<DeviceInfo> current;

  BackendRegistry::instance().enumerate([&](const depthcam_device_info& d) {
    const auto str = [](const char* s) {
      return s ? QString::fromUtf8(s) : QString{};
    };

    DeviceInfo info;
    info.backend = str(d.backend);
    info.uri = str(d.uri);
    info.name = str(d.name);
    info.serial = str(d.serial);
    info.transport = str(d.transport);
    info.streams = d.streams;

    if(!info.uri.isEmpty())
      current.push_back(std::move(info));
  });

  const auto has = [](const std::vector<DeviceInfo>& v, const QString& uri) {
    return std::any_of(
        v.begin(), v.end(), [&](const DeviceInfo& d) { return d.uri == uri; });
  };

  // Removals first, so a camera that changed transport does not briefly appear
  // twice.
  std::vector<QString> removed;
  for(const auto& old : m_devices)
    if(!has(current, old.uri))
      removed.push_back(old.uri);

  std::vector<DeviceInfo> added;
  for(const auto& dev : current)
    if(!has(m_devices, dev.uri))
      added.push_back(dev);

  if(current.empty())
    warnIfMissingUdevRules();

  m_devices = std::move(current);

  for(const auto& uri : removed)
    deviceRemoved(uri);
  for(const auto& dev : added)
  {
    // One line per camera at discovery: the first question in any support
    // report is whether score saw the hardware at all.
    qDebug().noquote() << "[depthcam] found" << dev.name << "via" << dev.backend
                       << "->" << dev.uri;
    deviceAdded(dev);
  }
}
}
