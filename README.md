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
nothing if you have no camera. Support for each camera family arrives as a
separate **backend package** you install from score's package manager. Install
only the ones you need.

After installing a backend, **install its udev rules** (Linux only, see below)
and restart score.

### Linux: udev rules are not optional

Every one of these SDKs talks to the camera over raw USB. Without the matching
udev rules the device nodes stay root-owned, the SDK reports *no cameras*, and
the symptom is indistinguishable from nothing being plugged in.

Each backend package ships the rules for its own hardware in a `udev/`
subdirectory:

```sh
sudo cp <backend-package>/udev/*.rules /etc/udev/rules.d/
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
`libk4a.so.1.4` and `libdepthengine.so.2.0` into the backend's package folder.

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

Point-cloud positions are in **metres**, right-handed, +X right / +Y down /
+Z forward. Colours, where present, are normalised to 0–1.

### Connecting by address

The **Camera** field is editable. Picking a camera from the browser fills it in,
but typing one by hand is the same code path — which is how you reach a camera
that is not currently being discovered, most importantly a Femto Mega on the
network:

```
orbbec:sn:CL8L1234ABC          match by serial number
orbbec:uid:2-1.3               match by SDK uid (survives identical serials)
orbbec:net:192.168.0.12        Femto Mega over Ethernet, default port 8090
orbbec:net:192.168.0.12:8090   explicit port
freenect2:sn:012345678901      Kinect v2
freenect:index:0               Kinect v1
k4a:sn:000123456789            Azure Kinect
realsense:sn:040322070203      RealSense
```

Leave it empty to take the first camera any backend reports.

If a named camera is not present the device fails to connect rather than falling
back to a different one: silently streaming from the wrong camera is worse than
not connecting.

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
```

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
everything it links (`-fvisibility=hidden` plus `--exclude-libs,ALL`) and is
verified to export a single symbol.

Writing a backend for another camera means implementing one header. Nothing on
the score side is camera-specific.

## Licences

This addon is under the same licence as ossia score. The SDKs it builds against
are not:

| Component | Licence | Redistributable |
|---|---|---|
| OrbbecSDK v2 | MIT | yes |
| OrbbecSDK `extensions/` blobs | Orbbec proprietary | **check before shipping** |
| libfreenect / libfreenect2 | Apache-2.0 / GPLv2 dual | yes |
| librealsense | Apache-2.0 | yes |
| Azure Kinect Sensor SDK | MIT | yes |
| `libdepthengine` | Microsoft proprietary | **no — not shipped** |
