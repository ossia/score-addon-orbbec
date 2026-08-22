# Upgrading the vendored OrbbecSDK

Status: **feasible, and the patch gets smaller.** Verified by building and
streaming, not by reading.

## Where we are

| | |
|---|---|
| Pinned | `4df49bab`, SDK **2.5.5**, one local commit on top of `262604f9` |
| Upstream | **2.9.3**, 566 commits ahead |
| Fork | `github.com/jcelerier/OrbbecSDK_v2` |

The single local commit, *"cmake/logs: bunch of fixes and disablings"*, touches
18 files. Most of that turns out to be obsolete.

## What the fork patch is actually for

Only one thing in it is load-bearing, and it is not what it looks like.

**The vendored spdlog cannot be built.** ossia score already provides an
`spdlog` target through libossia, CMake target names are global, and the SDK's
`3rdparty/spdlog/CMakeLists.txt` calls `add_subdirectory(src)` unconditionally:

```
CMake Error at .../3rdparty/spdlog/src/CMakeLists.txt:170 (add_library):
  add_library cannot create target "spdlog" because an imported target with
  the same name already exists.
```

That is a hard configure failure, and it is why the fork wrapped the whole file
in `if(0)`. Everything else in the patch follows from that decision rather than
from a requirement of its own:

- with the vendored spdlog gone, the SDK compiles against **score's** spdlog
  (1.17) instead of its own (1.14), and two of its APIs moved —
  `details::registry::instance_ptr()` was removed, and fmt 10 made
  `formatter::format()` const-only;
- rather than fix those, the fork defined every `LOG_*` macro to nothing and
  emptied `LoggerTypeHelper.hpp`, which is why the SDK is currently silent even
  when something goes wrong inside it.

The rest of the patch is `cmake_minimum_required` bumps and `-Werror`.

## What upstream already fixed

- **Every `cmake_minimum_required` is now 3.10**, up from 3.5. All six of the
  fork's version bumps are dead weight — CMake 4 accepts 3.10.
- **`transformationDepthToRGBDPointCloud` handles mismatched colour and depth
  sizes.** It scales the colour lookup instead of walking off the end. There is
  still no bounds check, so `color_matches_depth()` stays, but the failure it
  guards against is much less likely.
- **`FilterExtension::setConfigValue` seeds the whole config map from the
  schema defaults** before updating one entry. The 2.5.5 version did not, and
  `setConfigValue("outputZeroPoint", 0)` segfaulted inside the SDK on an
  out-of-range `params[4]`. Worth re-testing: dropping zero points would cut
  the cloud we upload substantially.
- **New devices**: Gemini 305 / 305g, Gemini 335Le / 335Lg / 345Lg, Gemini 338Le
  / 338Lg, plus a LiDAR product line.
- **New dependency**: `3rdparty/mdns`. It is used for `_oradar_udp`, the LiDAR
  products — depth cameras are still found by GVCP broadcast, so it does not
  change the [firewall situation](../README.md#orbbec-cameras-over-ethernet).

Otherwise the dependency set is unchanged: cmrc, dylib, jsoncpp, libjpeg,
libusb, libuvc, libyuv, live555, rosbag, spdlog, tinyxml2. Nothing new and
awkward has appeared.

## The rebased patch

Prepared on branch **`rebased-2.9.3`** in the submodule clone. Six files
instead of eighteen, and it keeps the SDK's logging working:

| File | Change |
|---|---|
| `3rdparty/spdlog/CMakeLists.txt` | `if(NOT TARGET spdlog)` around the whole thing |
| `cmake/options.cmake` | new `OB_WARNINGS_AS_ERRORS`, default `ON` |
| `cmake/compiler_flags.cmake` | `-Werror` behind that option |
| `src/shared/logger/LoggerTypeHelper.hpp` | `format()` const, `fmt::format_to` qualified, include `spdlog/fmt/fmt.h` |
| `src/shared/logger/Logger.{hpp,cpp}` | `registry&` instead of `shared_ptr<registry>` |

The `-Werror` and `LoggerTypeHelper` changes are both worth offering upstream:
neither is score-specific. The spdlog guard probably is too.

The addon passes `-DOB_WARNINGS_AS_ERRORS=OFF`.

## Verified

Built the backend against 2.9.3 with that patch and streamed from a Femto Mega
over USB: 1920×1080 colour, 640×576 depth, a 368 640-point cloud, ~30fps.

One loose end: the `extensions/` blobs in the build tree were still the 2.5.5
ones, and 2.9.3 logs `Device Component 'frame processor factory' not found`
against them. They are copied from the SDK tree, so a clean build picks up the
right ones; it needs confirming rather than assuming.

## Done

Taken, at `77005bbe` on the fork's `rebased-2.9.3` branch. Three things the
move needed that were not in the rebased patch, all on our side:

- `setExtensionsDirectory` is given `<package>/extensions`, not `<package>`.
  2.5 appended the subdirectory itself; 2.9 takes the path literally, and the
  only symptom of getting it wrong is a Femto Mega producing no point cloud —
  the SDK logs a warning and swallows the failure.
- The console log is set to errors only unless `SCORE_DEPTHCAM_DEBUG` is set,
  and the log *file* is turned off. With logging working again the SDK is
  chatty — opening a Femto Mega prints a dozen "recoverable exception"
  warnings from component probes that are entirely normal — and it writes
  `Log/OrbbecSDK.log.txt` into whatever directory score was launched from.
- `setLoggerToFile` takes `""`, not `nullptr`: it builds a `std::string` from
  the argument before looking at the severity.

One known limitation, unchanged by the upgrade but newly *visible* now that
the SDK logs: `extensions/frameprocessor/libob_frame_processor.so` links
`libOrbbecSDK.so.2`, which does not exist beside it because the SDK is
compiled into the backend. It therefore never loads, and the SDK reports
`Device Component 'frame processor factory' not found`. Everything works
without it — colour, depth, IR, point clouds, alignment, controls and the IMU
are all verified — because it is the SDK's optional device-side frame
post-processing. Loading it would mean shipping the 72MB shared library
alongside, which is the whole thing the static build exists to avoid.

## Recommendation as it stood

Take it, but as its own change with its own testing pass. The gain is real —
four minor versions of device support and bug fixes, a smaller patch, and the
SDK's own logging back — and the risk is concentrated in code we do not
exercise from here. What to check afterwards:

1. all four Femto Megas, USB and Ethernet, colour + depth + IR + both cloud
   alignments;
2. the `extensions/` blobs match the SDK version;
3. `outputZeroPoint`, which may now be usable;
4. the export count is still 1 (`nm -D --defined-only`), since the SDK is
   compiled into the backend and 566 commits is a lot of new symbols.
