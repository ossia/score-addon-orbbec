#include "score_addon_orbbec.hpp"

#include <score/plugins/FactorySetup.hpp>

#include <Orbbec/ApplicationPlugin.hpp>
#include <Orbbec/OrbbecInputDevice.hpp>

score_addon_orbbec::score_addon_orbbec() { }

score_addon_orbbec::~score_addon_orbbec() { }

score::GUIApplicationPlugin *score_addon_orbbec::make_guiApplicationPlugin(
    const score::GUIApplicationContext &app)
{
  return new Gfx::Orbbec::ApplicationPlugin{app};
}

std::vector<score::InterfaceBase*>
score_addon_orbbec::factories(
    const score::ApplicationContext& ctx,
    const score::InterfaceKey& key) const
{
  return instantiate_factories<score::ApplicationContext,
                               FW<Device::ProtocolFactory, Gfx::Orbbec::InputFactory>>(ctx, key);
}

#include <score/plugins/PluginInstances.hpp>
SCORE_EXPORT_PLUGIN(score_addon_orbbec)
