#pragma once
#include <score/plugins/application/GUIApplicationPlugin.hpp>

#include <libobsensor/ObSensor.hpp>

namespace Gfx::Orbbec
{
class ApplicationPlugin
    : public QObject
    , public score::GUIApplicationPlugin
{
public:
  using GUIApplicationPlugin::GUIApplicationPlugin;

  ob::Context orbbec;
};
}
