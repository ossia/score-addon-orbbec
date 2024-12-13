#pragma once
#include <score/plugins/application/GUIApplicationPlugin.hpp>

#include <libobsensor/ObSensor.hpp>

#include <verdigris>
namespace Gfx::Orbbec
{
class ApplicationPlugin
    : public QObject
    , public score::GUIApplicationPlugin
{
  W_OBJECT(ApplicationPlugin)
public:
  explicit ApplicationPlugin(const score::GUIApplicationContext& ctx);

  ob::Context orbbec;
  std::vector<std::shared_ptr<ob::Device>> m_known_devices;

  void deviceAdded(std::shared_ptr<ob::Device> dev) W_SIGNAL(deviceAdded, dev);
  void deviceRemoved(std::shared_ptr<ob::Device> dev) W_SIGNAL(deviceRemoved, dev);
};
}

Q_DECLARE_METATYPE(std::shared_ptr<ob::Device>)
W_REGISTER_ARGTYPE(std::shared_ptr<ob::Device>)
