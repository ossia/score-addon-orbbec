# Oak3DVision backend

Wraps [`oaksdk`](https://github.com/oak3dvision/oaksdk) — the `libPointCloud`
SDK — for the **Oak CDK** (`14b4:9108`, Melexis MLX75027) and **Oak QCDK**
(`14b4:9107`, MLX75026) time-of-flight modules.

> **Not the Luxonis OAK-D.** The names collide. Luxonis' OAK-D / OAK-D-Lite /
> OAK-D-Pro are stereo-depth cameras driven by
> [`depthai-core`](https://github.com/luxonis/depthai-core) and none of them is
> reachable through this backend or this SDK. If that is the camera you have,
> this is the wrong file; a `depthai` backend would be a separate one alongside
> it, built the same way as the other five in this tree.

## What it produces

These are ToF modules with an illuminator and no colour sensor, so the backend
publishes three of the five streams the ABI defines, and reports exactly that
through `active_streams()`:

| Stream | Source | Format |
|---|---|---|
| `DEPTH` | `DepthFrame::depth` | `GRAYF32`, metres, `depth_unit_mm = 1000` |
| `IR` | `DepthFrame::amplitude` | `GRAYF32`, 0..1 |
| `POINTCLOUD` | `XYZIPointCloudFrame` | `XYZ`, or `XYZRGB` with the ToF amplitude as grey when a tinted cloud is asked for |

There is no `COLOR` and no `IMU` on either model.

## Why this one is not built from source

Every other SDK here is a git submodule that this tree compiles. Oak3DVision
publishes no sources: the repository *is* the binary distribution — one
prebuilt `libpointcloud` per platform, the camera plug-ins it `dlopen`s, the
`.conf`/`.dml` files it reads, and the headers, all committed. So
`3rdparty/oaksdk` is vendored binaries, the backend links the one for the
platform being built, and the SDK's runtime tree is copied into the package.

Two consequences follow from that and are worth knowing before filing a bug.

**Platform coverage is whatever Oak3DVision built**, and it is narrower than it
first looks. There are binaries for Linux x86\_64, Linux aarch64, macOS x86\_64
and Windows — but the Windows one is **32-bit**: `pointcloud.dll` and
`pointcloud.lib` both carry machine type `0x014c` (i386). It cannot be linked
into the 64-bit module score loads; an x64 link fails with `library machine type
'x86' conflicts with target machine type 'x64'` and 23 unresolved externals.

So there is no Oak backend on Apple Silicon, and none on Windows. CMake skips
the target rather than failing, and the Windows condition is written for what is
true — MSVC targeting i386 — rather than hard-coded off, so a 32-bit build would
still pick it up.

**The submodule is large for what it gives.** A recursive clone pulls about
140 MB of history, of which roughly 68 MB is `Viewer_on_windows/` — a Windows
GUI viewer nothing here uses — and only one platform's ~7 MB of `libs/` is ever
linked. A fork trimmed to `libs/`, `script/` and the headers would cut nearly
all of that, and would be the natural place to carry any patch to the vendored
headers; see `Documentation/sdk-forks.md` for how the other two forks are
handled. Nothing here depends on that happening.

**Two of the four header trees are incomplete.** macOS and Windows ship 29
headers where Linux ships 52, and `Configuration.h` -- which `DepthCamera.h`
includes -- opens with `#include "HardwareSerializer.h"`, a file that is in
neither. As shipped, the SDK does not compile on macOS or Windows: `fatal error:
'HardwareSerializer.h' file not found`, confirmed on a Mac. The build lists the
platform's own tree first and Linux's second, so anything present locally still
wins and the 23 missing headers -- the portable half of the library, none of
which mentions Linux -- are filled in from there.

**The last commit upstream is from June 2020.** The Linux x86\_64 build is a
GCC 7-era binary using the C++11 `std::string` ABI, which is what current
toolchains produce anyway; the macOS one is `libc++`. Nothing here can be fixed
by rebuilding it.

## Runtime prerequisites

Unlike the other backends, this one is not self-contained — the vendor's binary
brings its own dependencies:

- **Linux x86\_64** — `libpointcloud.so` links `libglfw.so.3`, `libGL.so.1` and
  `libOpenCL.so.1`, none of which anything in this tree needs and none of which
  is guaranteed present. On Debian/Ubuntu: `libglfw3 libgl1 ocl-icd-libopencl1`.
  If they are missing the module simply fails to load and the host logs
  `[depthcam] could not load … libglfw.so.3: cannot open shared object file`;
  every other backend keeps working. (The **aarch64** binary needs none of them
   — it links only `libusb-1.0`.)
- **macOS** — `OpenCL.framework`, which is part of the OS, and the
  `libusb-1.0.0.dylib` the SDK ships, copied into the package beside it.
- **Windows** — the **Visual C++ redistributable**: `pointcloud.dll` imports
  `MSVCP140.dll` and `VCRUNTIME140.dll`, and score's own Windows build is
  llvm-mingw, so it does not bring one. Also `OpenCL.dll`, which comes from a
  graphics driver. Both failures are reported through `last_error()` rather than
  as a silent missing backend; see the delay-load note below.
- **Linux, all architectures** — the udev rules in `udev/`, like every other
  backend here.

## Notes on the implementation

**Where the SDK finds its own files.** libPointCloud resolves its plug-in
directory as `<base>/plugin` and its configuration directory as
`<base>/../share/pointcloud-1.0.0/conf`, both relative to wherever
`libpointcloud` was loaded from. In a package the SDK sits *beside* the backend
rather than in a `lib/` directory, so the second one would resolve above the
package and find nothing — and without the `.conf` and `.dml` files a camera
enumerates and then fails to connect. `init()` therefore sets
`POINTCLOUD_PLUGIN_PATH` and `POINTCLOUD_CONF_PATH` from its `resource_dir`,
before the `CameraSystem` is constructed, since that is what loads the plug-ins.

**Windows delay-loads `pointcloud.dll`.** Windows resolves a module's imports
through the process search order, which does not include the directory the
module itself came from — the same reason libusb is linked statically
everywhere else in this tree. `/DELAYLOAD:pointcloud.dll` lets `init()` run
first and load it by full path with `LOAD_WITH_ALTERED_SEARCH_PATH`; the
delay-load helper then finds it already in the process. It also turns a missing
VC++ runtime from "the backend is not in the list" into a sentence the user can
act on.

**Windows must be a release build.** `pointcloud.dll` is a release `/MD` binary,
and `std::vector` changes layout under `_ITERATOR_DEBUG_LEVEL` — while
`CameraSystem::scan()` returns one across the boundary. A debug MSVC build is
rejected with `#error` rather than corrupting memory. The backend is built `/MD`
for the same reason, unlike the rest of the package.

**Controls are curated.** `DepthCamera::getParameters()` returns every field the
model's DML describes: 307 raw sensor registers for the MLX75027, most of them
meaningless outside the vendor's bring-up tool and several of which will stop
the sensor if written. The backend publishes a fixed list of the ones that mean
something — integration time, unambiguous range, modulation frequencies,
binning, the two temperatures, and so on — resolved against the camera, so an
entry a model does not have is skipped, and with the kind, range and description
all taken from the SDK. Set `SCORE_DEPTHCAM_OAK_ALL_CONTROLS=1` to publish
everything under `advanced/` instead, which is what to do when bringing up a
model this list predates.

**Frame rate is a control, not just an open-time setting.** It is the one thing
here that is worth changing mid-piece, and the SDK exposes it only through
`get/setFrameRate` rather than as a parameter, so it is published by hand as
`depth/frame_rate`.

**`dynamic_cast` does not cross this boundary on macOS.** This is the only
backend that links a C++ SDK, so it is the only one that hits it. A module that
exports one symbol — which every backend here does, on purpose — exports no type
information either, so its copy of a type's RTTI cannot be merged with the SDK's.
libstdc++ and MSVC compare `type_info` by name and are unaffected; x86\_64 macOS
libc++ compares it by *address*, so every `dynamic_cast` across the boundary
returns null there. Two places would have been caught by it:

- the frame callbacks, which would have dropped every frame. They go by the
  `FrameType` the SDK passes alongside the frame instead, which is authoritative
  and needs no RTTI at all.
- reading a parameter's type, which is a `dynamic_cast` ladder because the SDK
  offers nothing else. Where it comes up empty the backend falls back to probing
  with `DepthCamera::get<T>` — a cast performed *inside* libpointcloud, against
  its own type information, so exactly one of the four instantiations succeeds.
  What is lost on macOS is the range and the enum labels, which live in the
  concrete class and have no accessor on the base: those controls are published
  unbounded rather than not at all.

## What was and was not verified

There was no Oak CDK or QCDK available while this was written.

Verified on **macOS**, on an M-series machine cross-compiling to Intel:

- `x86_64` builds and links, one exported symbol, `CheckPackage.sh` passes;
- `x86_64;x86_64h` builds a fat module carrying both slices, still one exported
  symbol, and is published under the single `darwin-x86_64` key -- x86_64h is a
  flavour of x86_64, not an architecture of its own, and the vendor's generic
  x86_64 dylib links into both slices;
- `arm64` and any Intel+Apple-Silicon mix correctly skip the backend.

Verified on **Windows**: the backend is correctly skipped, under MSVC and under
all three msys2 environments, because the vendor's binary is 32-bit.

Verified, on Linux x86\_64, against a staged package with no camera attached:

- the module builds warning-clean at `-Wall -Wextra`, links the vendored SDK,
  and exports exactly one symbol (`Deployment/CheckPackage.sh` passes);
- it `dlopen`s, reports ABI 3, and `init()` succeeds;
- the SDK honours `POINTCLOUD_PLUGIN_PATH` and loads both camera plug-ins from
  the package layout, registering the `Oakcdk` and `OakQcdk` factories;
- `enumerate()` returns zero devices, `open()` fails with `no Oak depth camera
  connected` rather than crashing, and `shutdown()` is clean.

Not verified — it follows from the SDK's headers, its shipped `.conf`/`.dml`
files and its own examples, but has not been seen to run:

- that a device connects and streams at all;
- that `POINTCLOUD_CONF_PATH` is read, which only happens on connect. It is the
  same override mechanism as the plug-in path, which was confirmed to work;
- the point cloud's axis convention. `depthcam_abi.h` requires +X right, +Y
  down, +Z forward, and `PointCloudTransform` builds its direction vectors from
  the image row and column indices through a pinhole model with the row growing
  downwards, which gives exactly that. It is the one thing here that would be
  wrong in a way that still looks plausible — a cloud that is upside down;
- which of the curated controls each model actually carries. The list is a
  superset drawn from `DepthCamera.h`'s `#define`s and the shipped camera
  profiles, and anything absent is skipped rather than published broken.
