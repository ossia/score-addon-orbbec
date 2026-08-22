# Depth cameras for ossia score

Streams colour, infrared, depth and point clouds from depth cameras into
[ossia score](https://ossia.io), as an ordinary device you add from the device
explorer.

| Camera | Backend | Linux | Windows | macOS |
|---|---|---|---|---|
| Orbbec Femto Mega / Mega i / Bolt | `orbbec` | yes | yes | colour + IR only¹ |
| Orbbec Gemini 330/335/336/345, Gemini 2, Astra 2, DaBai | `orbbec` | yes | yes | yes |
| Kinect v2 (Xbox One) | `freenect2` | yes | yes | yes |
| Kinect v1 (Xbox 360) | `freenect` | yes | yes | yes |
| Azure Kinect DK | `k4a` | yes | yes | no² |
| Intel RealSense D400/D500/L500/SR300 | `realsense` | yes | yes | yes |

¹ Femto Mega and Bolt compute depth with a proprietary depth engine that Orbbec
does not ship for macOS, so those two have no depth or point cloud there. The
Gemini and Astra lines compute depth on-device and are unaffected.

² Microsoft never released an Azure Kinect SDK for macOS.

Orbbec Femto Mega also works over Ethernet — see [Connecting by address](#connecting-by-address).

## Installing

The plug-in itself contains no camera SDK, so it is always present and costs
nothing if you have no camera. The SDKs arrive as one **Depth cameras** package
you install from score's package manager. It lands in:

```
~/Documents/ossia/score/packages/depth-camera/
```

and contains one self-contained shared object per camera family, plus the
auxiliary files two of the SDKs load at runtime:

```
depth-camera/
  package.json
  score_depthcam_orbbec.so        Femto, Gemini, Astra   (USB and Ethernet)
  score_depthcam_freenect.so      Kinect v1
  score_depthcam_freenect2.so     Kinect v2
  score_depthcam_k4a.so           Azure Kinect DK
  score_depthcam_realsense.so     Intel RealSense
  extensions/                     OrbbecSDK runtime blobs, loaded by path
  udev/                           the Linux rules, see below
```

The whole thing is about 8 MB. One package rather than one per SDK: the
backends are small, three of them need files sitting next to them, and plugging
in a camera should not require working out which of five downloads yours is in.

After installing, **install the udev rules** (Linux only, see below) and restart
score.

Nothing else has to be set up — no `LD_LIBRARY_PATH`, no rpath. Each backend is
a single file with no dependency on any SDK beside it, and it is given its own
directory to find `extensions/` in.

### Linux: udev rules are not optional

Every one of these SDKs talks to the camera over raw USB. Without the matching
udev rules the device nodes stay root-owned, the SDK reports *no cameras*, and
the symptom is indistinguishable from nothing being plugged in.

The package ships the rules for all the supported hardware in its `udev/`
subdirectory:

```sh
sudo cp ~/Documents/ossia/score/packages/depth-camera/udev/*.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Then **unplug and replug the camera**, or reboot.

The rules grant access through `TAG+="uaccess"` (systemd hands the logged-in
user an ACL) with `MODE="0666"` as a fallback for systems without logind.

> If you previously installed Microsoft's own `99-k4a.rules`, remove it. It does
> not work on a current udev: it opens with `BUS!="usb"` (a key udev removed years
> ago) and assigns `GROUP="plugdev"`, a group that does not exist on Arch, Fedora
> or openSUSE. When the group cannot be resolved udev discards the whole rule,
> `MODE` included, so the camera stays inaccessible.

To check that the rules took effect:

```sh
lsusb                       # find the bus/device numbers of your camera
ls -l /dev/bus/usb/<bus>/<dev>   # should be mode 666, not 664
```

### Azure Kinect: the SDK is not redistributable

The `k4a` backend does not bundle libk4a. The Azure Kinect requires
`libdepthengine`, a closed Microsoft binary we have no licence to ship, and
libk4a is useless without it. The backend therefore looks for the SDK at
runtime, in this order:

1. next to the backend, inside its own package directory;
2. the normal library search path (a system-installed SDK).

So you can either install the [Azure Kinect Sensor
SDK](https://github.com/microsoft/Azure-Kinect-Sensor-SDK) system-wide, or drop
it into the package folder yourself:

```
depth-camera/
  libk4a.so.1.4
  libk4a1.4/
    libdepthengine.so.2.0
```

The subdirectory is not a suggestion: libk4a's own `RUNPATH` is
`$ORIGIN/libk4a1.4`, so that is the only place it looks for the depth engine.
Put the engine flat next to libk4a and the camera opens but produces no depth.

If it is missing, score's log says so explicitly rather than leaving the camera
mysteriously absent:

```
[depthcam] "k4a" reports: libk4a not found. Install the Azure Kinect Sensor SDK,
           or place libk4a and libdepthengine next to this backend.
```

### USB 3 is required for several cameras

The Azure Kinect DK, Kinect v2 and RealSense D400 series all need USB 3.

- On USB 2 the **Kinect v2 and Azure Kinect do not enumerate their cameras at
  all** — only the internal hub and microphone array appear, which looks exactly
  like a permissions problem.
- A **RealSense D435i on USB 2** *does* work, but drops to a handful of low-rate
  profiles: there is no 30fps colour mode at all, and you may see under 1fps on
  every stream. If a RealSense feels frozen, check this first.

To see what link a camera actually negotiated:

```sh
for d in /sys/bus/usb/devices/*/; do
  [ -f "$d/idVendor" ] && echo "$(basename $d) $(cat $d/idVendor):$(cat $d/idProduct) $(cat $d/speed) Mbps"
done
```

`480` is USB 2. Note that a USB-C *cable* does not imply a USB 3 *port* — check
the port, not the cable.

## Using it

Add a device, pick **Depth Camera Input** under *Video In*. Each installed
backend appears as its own category in the browser, listing the cameras it can
see. Selecting one fills in the address.

The device exposes one child node per enabled stream:

| Node | Type | Notes |
|---|---|---|
| `rgb` | texture | |
| `ir` | texture | not offered by Kinect v1, which can stream IR only *instead of* colour |
| `depth` | texture | 16-bit; the shader rescales using the unit the camera reports |
| `pointcloud` | geometry | connect to *Model Display* or any geometry input |
| `imu/accel` | vec3f | acceleration in m/s², including gravity; off by default |
| `imu/gyro` | vec3f | angular velocity in rad/s |
| `imu/temperature` | float | degrees Celsius, where the camera reports it |

Point-cloud positions are in **metres**, right-handed, +X right / +Y down /
+Z forward. Colours, where present, are normalised to 0–1.

### Orbbec cameras over Ethernet

A Femto Mega with an Ethernet port shows up in the browser like any other
camera, alongside the USB ones, with `Ethernet` as its transport. There is
nothing to configure: the SDK broadcasts a GVCP discovery request on every
interface and lists whatever answers.

**If no network camera appears, suspect your firewall before the camera.** The
cameras reply to `255.255.255.255`, not to the host that asked — the firmware
ignores the GigE Vision "unicast acknowledge" flag — so the answer arrives as a
broadcast datagram, and a host firewall that drops broadcast input hides every
networked camera on the segment while they are all answering perfectly well.

`ufw` does exactly that out of the box; its `ufw-after-input` chain sends
anything with a broadcast destination straight to the default DROP policy. Since
the reply's *destination* port is ephemeral, the rule has to match the *source*
port:

```sh
sudo ufw allow proto udp from 192.168.0.0/24 port 3956 comment 'Orbbec GVCP discovery'
```

with `firewalld`:

```sh
sudo firewall-cmd --permanent --add-rich-rule='rule family=ipv4 source address=192.168.0.0/24 port port=3956 protocol=udp accept'
sudo firewall-cmd --reload
```

To check whether the cameras are answering at all, independently of score:

```sh
sudo tcpdump -ni any 'udp port 3956'
```

Each camera should send one 256-byte reply per discovery request. If you see the
replies here but no camera in score, it is the firewall.

Discovery is a broadcast, so it also never crosses a router: a camera on another
subnet has to be addressed directly. Tick **Connect over the network** in the
device dialog and type its host and port (8090 unless you changed it). That path
is a plain TCP connection and is unaffected by any of the above.

### Connecting by address

The **Camera** field is editable. Picking a camera from the browser fills it in,
but typing one by hand is the same code path — which is how you reach a camera
that is not currently being discovered:

```
orbbec:sn:CL8L1234ABC          match by serial number
orbbec:uid:2-1.3               match by SDK uid (survives identical serials)
orbbec:net:192.168.0.12        Femto Mega over Ethernet, default port 8090
orbbec:net:192.168.0.12:8090   explicit port
freenect2:sn:012345678901      Kinect v2
freenect:index:0               Kinect v1
freenect:index:0?motor=1       Kinect v1, claiming the tilt motor as well
k4a:sn:000123456789            Azure Kinect
realsense:sn:040322070203      RealSense
```

Leave it empty to take the first camera any backend reports.

If a named camera is not present the device fails to connect rather than falling
back to a different one: silently streaming from the wrong camera is worse than
not connecting.

### Accelerometer and gyroscope

Tick **Accelerometer / gyroscope** to publish the camera's inertial sensors at
the device root. Units are SI — metres per second squared and radians per second
— for the same reason point clouds are in metres: a patch should not have to
know which camera it is reading.

| Camera | Rate | Temperature |
|---|---|---|
| Azure Kinect DK | 1.6 kHz, both | yes |
| Intel RealSense D435i | 200 Hz gyro, 63 Hz accel | no |
| Orbbec Femto Mega | as configured, 50 Hz by default | yes |
| Kinect v1 | 2 Hz, accelerometer only, needs `?motor=1` | no |
| Kinect v2 | — no inertial sensor | — |

A camera that has none publishes no nodes at all rather than three that never
move, so the tree tells you what the hardware can actually do.

Accelerometer and gyroscope are sampled independently and rarely at the same
rate, so each arrives on its own: on a D435i the gyro node updates three times
per accelerometer update. Nothing is paired up or held back.

Off by default, and not because it is expensive — it is a few hundred samples a
second and no image data. It is off because asking changes what the camera
streams: an Orbbec runs a second pipeline for it, an Azure Kinect a second
capture loop.

Where the camera lets you choose a rate, it is a setting like any other:
`controls/imu/accel_odr` and `controls/imu/gyro_odr` on an Orbbec, with
`*_full_scale` for the ranges. Those are the IMU's *configuration*, at
`controls/imu/`; the readings are at `imu/`. Same word, opposite direction.

### Camera settings

Whatever the camera's SDK will let you change appears under a `controls` node,
grouped by what it affects:

```
cam:/controls/color/exposure
cam:/controls/color/auto_exposure
cam:/controls/depth/laser_power
cam:/controls/imu/gyro_sensitivity
cam:/controls/sensors/temperature
cam:/controls/advanced/reboot_device
```

These are ordinary ossia parameters: automate them, drive them from OSC, read
them back. Each carries its real type, range and access mode, so a slider knows
its bounds and a read-only sensor is not offered as writable.

Nothing about the list is hard-coded. Orbbec and librealsense enumerate their
settings at runtime, so a camera that did not exist when this was written still
comes out organised; k4a and the two libfreenect backends have fixed lists but
ask the device what it supports. Roughly what to expect:

| Camera | Controls |
|---|---|
| Orbbec Femto Mega | 39 — colour, depth, laser, IMU, fan, sync, sensors |
| Intel RealSense D435i | 43 — per sensor: stereo module, RGB, IMU |
| Azure Kinect DK | 12 — the colour controls, plus sensor temperature |
| Kinect v2 | 5 — the three exposure modes and their parameters |
| Kinect v1 | 6 — tilt, LED, accelerometer (see below) |


Two groups are worth calling out:

- **`sensors/`** holds read-only values: temperatures, accelerometers, the
  exposure an automatic mode settled on. No SDK here reports a change, so these
  are polled — but only while something is listening to them. A control nobody
  is watching costs nothing.
- **`advanced/`** holds triggers with no readable value: rebooting the camera,
  entering recovery mode. They are grouped away deliberately so they are not one
  click from a performer mid-performance.

Writes go straight to the camera. A camera that clamps or rounds a value, or
refuses it outright (an exposure while auto-exposure is on, a fan speed the
firmware rejects), is reflected on the next read rather than silently accepted.

**Kinect v1 tilt and LED** are opt-in: append `?motor=1` to the address. The
motor is a separate USB interface, and on the original model 1414 claiming it
costs nothing — but on a model 1473 or a Kinect for Windows libfreenect drives
the motor through the *audio* interface, which needs a firmware upload this
build does not ship. Asking for it there fails the open outright and resets the
camera for several seconds. A camera that streams is worth more than a tilt
motor, so nothing is risked unless you ask.

### Alignment, and why it decides your frame rate

**Alignment** brings depth and colour into a common frame of reference. It also
decides the point cloud's resolution, which dominates everything else about its
cost. On a Femto Mega:

| Mode | Cloud resolution | Points | Per frame |
|---|---|---|---|
| Depth to color | colour (1920×1080) | 2 073 600 | 47.5 MB |
| Color to depth | depth (640×576) | 368 640 | 8.4 MB |
| None | depth | 368 640 | 8.4 MB |

*Depth to color* upsamples depth to the colour resolution, inventing roughly five
points for every one the sensor measured. *Color to depth* is the default and is
usually what you want for a point cloud; *depth to color* is right when you want
`rgb` and `depth` pixel-aligned for your own shader work.

**A coloured point cloud requires an alignment other than None** — the option is
disabled otherwise. Without one the SDK would read mismatched buffers, and at
least one of them walks off the end of the colour image.

### Resolution and frame rate

`Any` lets the backend choose. Two things worth knowing:

- Setting a **colour resolution** is the simplest way to cut the cost of a
  *depth to color* point cloud.
- With a coloured point cloud the Orbbec backend requests **raw RGB**, because
  the point-cloud filter cannot consume the MJPEG the camera negotiates by
  default. If you leave the resolution on `Any` it pins 1920×1080, since an
  explicit format with "any" resolution makes the SDK pick its *largest* profile
  — 3840×2160 on a Femto Mega, which is 8.3M points and ~199 MB per frame.

## Troubleshooting

Set `SCORE_DEPTHCAM_DEBUG=1` to have each point-cloud node report, once a
second, how many render passes, new frames and uploads it saw. That separates
"the camera is not delivering" from "the graph is not asking", which look
identical from the outside.

| Symptom | Likely cause |
|---|---|
| No cameras in the browser at all | No backend installed; check score's log for `[depthcam] loaded backend` |
| One camera family missing | That backend is missing, or its udev rules are not installed |
| Azure Kinect missing | libk4a not found — see above |
| Kinect v2 / Azure Kinect missing entirely | Plugged into USB 2 |
| Everything very slow, RealSense | Plugged into USB 2 |
| Coloured point cloud is white | Fixed; if you see it again, report the camera model |
| `Colored point cloud` greyed out | Alignment is set to None |
| No **Ethernet** Orbbec cameras, USB ones fine | A host firewall dropping broadcast input — see [Orbbec cameras over Ethernet](#orbbec-cameras-over-ethernet) |
| Kinect v1 opens once, then "is not connected" | A model 1473 reports a different serial each time it is listed; fixed, but an address saved by an older build may need re-picking from the browser |
| No `controls` node | That backend has nothing to offer for this camera, or (Kinect v1) the motor was not requested |

In a Release build score's own diagnostics go to stderr, which is not attached
to a console by default on any platform. To see them:

```sh
QT_ASSUME_STDERR_HAS_CONSOLE=1 ossia-score
```

## Building from source

Requires the submodules:

```sh
git clone --recursive https://github.com/jcelerier/score-addon-orbbec
```

Backends build only when their submodule is present, and each can be turned off:

```
-DSCORE_DEPTHCAM_BUILD_FREENECT=OFF
-DSCORE_DEPTHCAM_BUILD_FREENECT2=OFF
-DSCORE_DEPTHCAM_BUILD_REALSENSE=OFF
-DSCORE_DEPTHCAM_FREENECT2_OPENCL=ON    # Kinect v2 GPU depth pipeline, on by default
-DSCORE_DEPTHCAM_ORBBEC_STATIC=ON       # build the OrbbecSDK into the backend
```

To produce the distributable package:

```sh
cmake --build . --target depthcam-package
```

which stages, strips and zips `<build>/depth-camera/` into
`depth-camera-<arch>.zip`, named the way `score::addonArchitecture()` spells it
(`linux-x86_64`, `darwin-aarch64`, `windows-x86_64`) — that is the key score's
package manager looks up in the remote manifest. Stripping is not cosmetic: with
the SDK built in, the Orbbec backend is 139 MB unstripped and 9 MB stripped.

`<build>/depth-camera/` *is* the package layout, so anything that works in the
build tree works installed.

The Kinect v2's depth decode is by far the slowest part of that camera. With the
CPU pipeline it delivers about 1.6fps; with OpenCL it reaches 30fps, the sensor's
own rate. The backend picks CUDA, then OpenCL, then CPU at runtime and reports
which it chose. `OpenGLPacketPipeline` is never used: it creates its own GLFW
context and window, which has no business inside score's renderer.

## How it is put together

The score plug-in links **no camera SDK**. Each SDK lives in its own shared
object behind a small C ABI (`DepthCamera/Backend/depthcam_abi.h`), loaded with
`dlopen`/`RTLD_LOCAL` and exporting exactly one symbol.

That is not only for packaging. libfreenect, libfreenect2, k4a, the OrbbecSDK and
librealsense each bundle or link their own libusb, libuvc, libjpeg and logging
library. With default ELF visibility the first definition loaded wins for every
caller, so two SDKs sharing a process silently bind to one another's copies —
and libjpeg in particular bakes struct sizes into its callers, so a mismatch
corrupts memory rather than failing to link. Each backend therefore hides
everything it links (`-fvisibility=hidden` plus `--exclude-libs,ALL` plus a
linker version script) and exports exactly one symbol. CI fails the build if any
backend exports more than one, because that guarantee is what the whole design
rests on: score `dlopen`s the contents of its package directories, on Linux with
`RTLD_GLOBAL`.

Writing a backend for another camera means implementing one header. Nothing on
the score side is camera-specific.

## Testing

`Tests/` holds two suites: harnesses that drive a backend `.so` directly through
the ABI, and scripts that drive score itself through its JS API. Both need real
hardware; see [Tests/README.md](Tests/README.md).

Notes on the vendored OrbbecSDK and what upgrading it would involve are in
[Documentation/orbbec-sdk-upgrade.md](Documentation/orbbec-sdk-upgrade.md).

## Licences

This addon is under the same licence as ossia score. The SDKs it builds against
are not:

| Component | Licence | Redistributable |
|---|---|---|
| OrbbecSDK v2 | MIT | yes |
| OrbbecSDK `extensions/` blobs | Orbbec proprietary | **check before shipping** |
| `extensions/depthengine` (Femto Bolt) | Microsoft, redistributed by Orbbec | as the OrbbecSDK does |
| libfreenect / libfreenect2 | Apache-2.0 / GPLv2 dual | yes |
| librealsense | Apache-2.0 | yes |
| Azure Kinect Sensor SDK | MIT | yes |
| `libdepthengine` | Microsoft proprietary | **no — not shipped** |
