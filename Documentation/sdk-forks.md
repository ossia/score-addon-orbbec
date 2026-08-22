# Forked SDKs

Two of the vendored SDKs are pinned to forks under `jcelerier/` rather than to
upstream, each carrying a memory-safety fix that AddressSanitizer found while
running this add-on's own tests. Both fixes are small and both are worth
upstreaming; one already has a PR open.

## librealsense → `jcelerier/librealsense`, branch `score-2.58.1`

Based on `v2.58.1` (`bf2778061`), the version we were already pinned to.

**Out-of-bounds read of every inertial sample.**
`rs_hid_device::handle_interrupt` sets `data.fo.pixels = &hid.x` — a `hid_data`,
three `int32_t`, 12 bytes — but `data.fo.frame_size = sizeof(REALSENSE_HID_REPORT)`,
which is packed and 38. `hid_sensor::start` then copies `frame_size` bytes from
`pixels`, reading 26 bytes past the end of a stack object on every accelerometer
and gyroscope sample, and copying whatever was there into the motion frame. Both
occurrences use `sizeof(hid)` now. The Media Foundation backend
(`mf-hid.cpp`) already pairs the pointer and the size correctly, so this brings
the RSUSB backend in line with it.

Filed upstream as [realsenseai/librealsense#15583](https://github.com/realsenseai/librealsense/pull/15583).

**Use-after-return when `invoke_and_wait` gives up.**
`dispatcher::invoke_and_wait` waits on `done || exit_condition()`, so it can
return before the queued action has run — and the action captured `done` by
reference from the frame that just went away. `rs_uvc_device::set_power_state`
compounds it by capturing its `state` parameter by reference too. ASan reports
it on close. Fixed here by making the flag shared and capturing `state` by
value.

Not filed upstream: `development` has since replaced that machinery with a
shared `invocation_state` that *cancels* the action when the wait gives up,
which fixes it properly. This is a backport for the version we pin.

## libfreenect2 → `jcelerier/libfreenect2`, branch `fix-buffer-delete`

Based on `v0.2.1` (`fd64c5d`), the version we were already pinned to.

**`Buffer` had no virtual destructor.** Five allocator types derive from it —
`OpenCLBuffer`, `OpenCLKdeBuffer`, `VaapiImage`, `VaapiBuffer`, `TegraImage` —
and every `Allocator::free` deletes through a `Buffer*`, which is undefined.
ASan reports it as `new-delete-type-mismatch` on any teardown of an OpenCL
pipeline, including the trial pipeline `Freenect2Impl::openDevice` builds and
immediately destroys, so it fires on every device open.

Upstream has been dormant for a long time; the branch is here if anyone wants
to carry it further.

## What was verified

| Fix | Verified how |
|---|---|
| libfreenect2 virtual destructor | ASan: `new-delete-type-mismatch` 1 → 0, whole run 0 errors, Kinect v2 still streams |
| librealsense `invoke_and_wait` | ASan: `stack-use-after-return` gone, D435i streams 167 frames, open/close/reopen clean |
| librealsense HID `frame_size` | **Not verified at runtime.** See below |

The HID fix is correct by construction — the pointer is 12 bytes and the size
said 38, and the sibling backend pairs them correctly — but the D435i's inertial
streams stopped delivering on the development machine partway through this work
and could not be made to deliver again. The failure reproduces identically on
**pristine** librealsense (0 samples, and 0 video frames when the IMU is
enabled), so it is not caused by these changes, but it does mean the overflow
was not observed to be gone with the IMU actually running.

What was ruled out: the motion sensor is still enumerated (the `imu` control
group is intact, 43 controls); unbinding `usbhid` from the HID interface did not
help; nor did forcing a full USB re-enumeration. The kernel's `hid-sensor` stack
does claim that interface and exposes it as `iio:device0`/`iio:device1`
(`accel_3d`, `gyro_3d`), which is the usual suspect when a `FORCE_RSUSB_BACKEND`
build cannot reach the IMU — but unbinding it was not sufficient here. Needs a
physical replug, or the `hid-sensor-*` modules blacklisted, to retest.
