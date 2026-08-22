#pragma once

/**
 * @file DepthCameraControls.hpp
 * @brief Publish a camera's settings as `<cam>/controls/<group>/<name>`.
 *
 * The backend side of this is depthcam_abi.h's list_controls/get_control/
 * set_control. Nothing here names a specific control or a specific camera: an
 * Orbbec publishes its ~50 properties, a RealSense its options, an Azure Kinect
 * its colour controls and a Kinect v1 its tilt motor, all through the same
 * three calls, and the grouping comes from the identifier the backend chose.
 *
 * Two things shape the implementation:
 *
 *  - **No SDK here reports control changes.** Orbbec, librealsense, k4a,
 *    libfreenect and libfreenect2 all only offer a getter, so a read-only
 *    sensor -- a temperature, an accelerometer -- can only be tracked by asking
 *    repeatedly. Polling every such control on every camera all the time would
 *    put a steady stream of USB control transfers behind the video, so the
 *    timer only ever reads controls something is actually listening to:
 *    ossia calls protocol_base::observe() when a parameter gains its first
 *    callback and again when it loses its last.
 *
 *  - **Never push into a parameter from inside that parameter's callback.**
 *    callback_container::send holds a non-recursive mutex while it runs
 *    callbacks, so a corrective push made inline deadlocks the writing thread.
 *    A device that clamps or rounds a value is reported on the next poll
 *    instead.
 */

#include <ossia/network/base/device.hpp>
#include <ossia/network/base/node.hpp>
#include <ossia/network/base/parameter.hpp>
#include <ossia/network/value/value.hpp>

#include <QObject>
#include <QTimer>

#include <depthcam_abi.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Gfx::DepthCamera
{

class ControlTree
{
public:
  /**
   * @brief Enumerates the camera's controls and mirrors them into the tree.
   *
   * Does nothing at all if the backend predates ABI 2 or has no controls to
   * offer, so a device whose backend implements none of this simply gets no
   * `controls` node rather than an empty one.
   */
  ControlTree(
      const depthcam_backend_v1& backend, depthcam_device& device,
      ossia::net::device_base& dev, ossia::net::node_base& parent);
  ~ControlTree();

  ControlTree(const ControlTree&) = delete;
  ControlTree& operator=(const ControlTree&) = delete;

  bool empty() const noexcept { return m_entries.empty(); }

  /// @return true if @p param is one of ours and the write was accepted.
  bool write(const ossia::net::parameter_base& param, const ossia::value& v);

  /// Reads one control straight from the camera and publishes it.
  bool read(ossia::net::parameter_base& param);

  /// ossia's observation hook: starts and stops the poll timer's interest in
  /// one read-only control.
  bool observe(const ossia::net::parameter_base& param, bool enable);

  /// Re-reads every readable control. Used when the device tree is refreshed.
  void refresh();

private:
  struct Entry
  {
    std::string id; ///< as the backend spelled it, e.g. "color/exposure"
    ossia::net::parameter_base* param{};
    int kind{};
    int access{};
    /// Index into m_descs, for the range and the enum labels.
    int desc{-1};
    /// Number of things listening. Only polled while non-zero.
    int observers{};
    /// Cached so the timer does not push an identical value every tick.
    double last{};
    bool has_last{};
  };

  Entry* find(const ossia::net::parameter_base& param) noexcept;
  void publish(Entry& e, double v);
  void poll();
  void updateTimer();

  const depthcam_backend_v1& m_backend;
  depthcam_device& m_device;

  /// Every get_control/set_control call is serialised: writes arrive from the
  /// explorer, from OSC and from the execution engine, while the timer reads
  /// from the Qt thread.
  std::mutex m_lock;

  std::vector<Entry> m_entries;

  /// The descriptors as the backend gave them, kept for the ranges and the enum
  /// labels. Their strings belong to the backend and stay valid until close,
  /// which is why this object has to be destroyed before the device is closed.
  std::vector<depthcam_control> m_descs;

  std::unique_ptr<QTimer> m_timer;
  QObject m_context;
};

}
