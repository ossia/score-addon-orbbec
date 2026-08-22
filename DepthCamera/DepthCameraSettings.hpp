#pragma once

#include <QMetaType>
#include <QString>

#include <verdigris>

namespace Gfx::DepthCamera
{

/**
 * @brief Which camera to talk to, and what to ask it for.
 *
 * The camera is addressed by a text URI whose first component names the backend
 * and whose remainder is opaque to everything on this side:
 *
 *   orbbec:sn:CL8L1234ABC        orbbec:net:192.168.0.12:8090
 *   freenect2:sn:012345678901    freenect:index:0    k4a:index:0
 *
 * One field, one widget, and the same code path whether the camera was picked
 * from the device browser or typed in by hand -- which is how a networked
 * camera, or one that is not currently broadcasting, gets connected.
 */
struct DepthCameraSettings
{
  QString device;

  bool rgb{true};
  bool ir{false};
  bool depth{true};
  bool pointcloud{true};

  /// Colour the point cloud from the RGB sensor. Needs rgb + depth and an
  /// alignment other than None.
  bool colorPointcloud{false};

  /**
   * @brief Accelerometer and gyroscope, published as `imu/accel` and `imu/gyro`.
   *
   * Off by default, and not because it is expensive -- it is a few hundred
   * samples a second and no image data at all. It is off because asking for it
   * changes what the camera streams: an Orbbec has to have the IMU sensors
   * enabled in the pipeline configuration, and an Azure Kinect runs a second
   * capture loop for it. A device that did not ask should not pay.
   */
  bool imu{false};

  /**
   * @brief How depth and colour are brought into a common frame of reference.
   *
   * This also decides the point cloud's resolution, which dominates its cost.
   * Measured on a Femto Mega:
   *   DepthToColor  1920x1080 -> 2 073 600 points, 47.5 MB/frame
   *   ColorToDepth   640x576  ->   368 640 points,  8.4 MB/frame
   * DepthToColor upsamples depth to the colour resolution, inventing roughly
   * five points for every one the sensor actually measured.
   */
  enum class AlignMode
  {
    None,
    DepthToColor,
    ColorToDepth,
  };
  AlignMode align{AlignMode::ColorToDepth};

  /// 0 means "let the backend choose".
  int colorWidth{0}, colorHeight{0}, colorFps{0};
  int depthWidth{0}, depthHeight{0}, depthFps{0};
  int irWidth{0}, irHeight{0}, irFps{0};

  /// The backend a device URI names, e.g. "orbbec". Empty if none.
  QString backend() const;
};

}

Q_DECLARE_METATYPE(Gfx::DepthCamera::DepthCameraSettings)
W_REGISTER_ARGTYPE(Gfx::DepthCamera::DepthCameraSettings)
