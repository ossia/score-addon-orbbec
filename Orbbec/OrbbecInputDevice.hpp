#pragma once

#include <Gfx/GfxInputDevice.hpp>
#include <Gfx/SharedInputSettings.hpp>
class QLineEdit;
class QComboBox;
namespace Gfx::Orbbec {
class InputFactory final : public SharedInputProtocolFactory
{
    SCORE_CONCRETE("a3bd48ba-f5db-43b7-aa92-2b1a37a88a79")
public:
  QString prettyName() const noexcept override;
  QUrl manual() const noexcept override;
  Device::DeviceEnumerators
  getEnumerators(const score::DocumentContext& ctx) const override;

  Device::DeviceInterface* makeDevice(
      const Device::DeviceSettings& settings,
      const Explorer::DeviceDocumentPlugin& plugin,
      const score::DocumentContext& ctx) override;
  const Device::DeviceSettings& defaultSettings() const noexcept override;

  Device::ProtocolSettingsWidget* makeSettingsWidget() override;
};

class InputSettingsWidget final : public SharedInputSettingsWidget
{
public:
  InputSettingsWidget(QWidget* parent = nullptr);

  Device::DeviceSettings getSettings() const override;
};

} // namespace Gfx::Orbbec
