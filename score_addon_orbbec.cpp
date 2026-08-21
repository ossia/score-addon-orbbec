#include "score_addon_orbbec.hpp"

#include <score/plugins/FactorySetup.hpp>

#include <DepthCamera/ApplicationPlugin.hpp>
#include <DepthCamera/DepthCameraDevice.hpp>
#include <DepthCamera/DepthCameraSettings.hpp>

score_addon_orbbec::score_addon_orbbec()
{
  // Both travel through QVariant in Device::DeviceSettings.
  qRegisterMetaType<Gfx::DepthCamera::DepthCameraSettings>();
  qRegisterMetaType<Gfx::DepthCamera::DeviceInfo>();
}

score_addon_orbbec::~score_addon_orbbec() { }

score::GUIApplicationPlugin* score_addon_orbbec::make_guiApplicationPlugin(
    const score::GUIApplicationContext& app)
{
  return new Gfx::DepthCamera::ApplicationPlugin{app};
}

std::vector<score::InterfaceBase*> score_addon_orbbec::factories(
    const score::ApplicationContext& ctx, const score::InterfaceKey& key) const
{
  return instantiate_factories<
      score::ApplicationContext,
      FW<Device::ProtocolFactory, Gfx::DepthCamera::InputFactory>>(ctx, key);
}

#include <score/plugins/PluginInstances.hpp>
SCORE_EXPORT_PLUGIN(score_addon_orbbec)
