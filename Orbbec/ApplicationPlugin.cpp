#include "ApplicationPlugin.hpp"

#include <QGuiApplication>
#include <QThread>

#include <wobjectimpl.h>

W_OBJECT_IMPL(Gfx::Orbbec::ApplicationPlugin)
namespace Gfx::Orbbec
{
ApplicationPlugin::ApplicationPlugin(const score::GUIApplicationContext& ctx)
    : score::GUIApplicationPlugin{ctx}
{
  orbbec.setDeviceChangedCallback([this](
                                      std::shared_ptr<ob::DeviceList> removedList,
                                      std::shared_ptr<ob::DeviceList> deviceList) {
    QMetaObject::invokeMethod(this, [this, removedList, deviceList] {
      for(uint32_t index = 0, N = deviceList->getCount(); index < N; index++)
      {
        m_known_devices.push_back(deviceList->getDevice(index));
        deviceAdded(m_known_devices.back());
      }
      for(uint32_t index = 0, N = removedList->getCount(); index < N; index++)
      {
        ossia::remove_erase(m_known_devices, deviceList->getDevice(index));
        deviceRemoved(m_known_devices.back());
      }
    });
  });
}
}
