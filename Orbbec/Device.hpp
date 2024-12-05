#pragma once
#include <Device/Protocol/DeviceInterface.hpp>

namespace Explorer
{
class DeviceDocumentPlugin;
}

namespace Orbbec
{
class DeviceImplementation final : public Device::OwningDeviceInterface
{
  W_OBJECT(DeviceImplementation)
public:
  DeviceImplementation(
      const Device::DeviceSettings& settings,
      const Explorer::DeviceDocumentPlugin& plugin,
      const score::DocumentContext& ctx);
  ~DeviceImplementation();

  bool reconnect() override;
  void disconnect() override;

private:
  const Explorer::DeviceDocumentPlugin& m_ctx;
};
}