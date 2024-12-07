#pragma once
#include <score/application/ApplicationContext.hpp>
#include <score/command/Command.hpp>
#include <score/command/CommandGeneratorMap.hpp>
#include <score/plugins/InterfaceList.hpp>
#include <score/plugins/qt_interfaces/FactoryFamily_QtInterface.hpp>
#include <score/plugins/qt_interfaces/FactoryInterface_QtInterface.hpp>
#include <score/plugins/qt_interfaces/GUIApplicationPlugin_QtInterface.hpp>
#include <score/plugins/qt_interfaces/PluginRequirements_QtInterface.hpp>

#include <utility>
#include <vector>

class score_addon_orbbec final
    : public score::Plugin_QtInterface
    , public score::FactoryInterface_QtInterface
    , public score::ApplicationPlugin_QtInterface
{
  SCORE_PLUGIN_METADATA(1, "efc2818e-89c0-4741-b949-6c7cd82a1257")

public:
  score_addon_orbbec();
  ~score_addon_orbbec() override;

private:
  std::vector<score::InterfaceBase*> factories(
      const score::ApplicationContext& ctx,
      const score::InterfaceKey& key) const override;

  score::GUIApplicationPlugin *make_guiApplicationPlugin(const score::GUIApplicationContext &app) override;
};
