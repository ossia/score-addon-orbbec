#include "score_addon_orbbec.hpp"

#include <score/plugins/FactorySetup.hpp>

#include <Orbbec/ProtocolFactory.hpp>

score_addon_orbbec::score_addon_orbbec() { }

score_addon_orbbec::~score_addon_orbbec() { }

std::vector<score::InterfaceBase*>
score_addon_orbbec::factories(
    const score::ApplicationContext& ctx,
    const score::InterfaceKey& key) const
{
  return instantiate_factories<
      score::ApplicationContext,
      FW<Device::ProtocolFactory, Orbbec::ProtocolFactory>>(ctx, key);
}

#include <score/plugins/PluginInstances.hpp>
SCORE_EXPORT_PLUGIN(score_addon_orbbec)
