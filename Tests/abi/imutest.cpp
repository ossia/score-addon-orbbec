// Inertial samples across the ABI: rate, units, and which fields arrive.
#include <depthcam_abi.h>
#include <dlfcn.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

static std::atomic<int> n_accel{0}, n_gyro{0}, n_temp{0}, n_video{0};
// Kept apart on purpose: a backend may deliver accelerometer and gyroscope in
// separate samples -- a D435i runs them at 63Hz and 200Hz -- so "the last
// sample" is not a reading of both.
static depthcam_imu_sample last_accel{}, last_gyro{}, last_temp{};
static std::atomic_bool have{false};

static void on_frame(const depthcam_frame* f, void*)
{
  if(f->stream == DEPTHCAM_STREAM_IMU)
  {
    depthcam_imu_sample s{};
    if(f->bytes >= sizeof(s))
    {
      std::memcpy(&s, f->data, sizeof(s));
      if(s.fields & DEPTHCAM_IMU_ACCEL) { n_accel++; last_accel = s; }
      if(s.fields & DEPTHCAM_IMU_GYRO) { n_gyro++; last_gyro = s; }
      if(s.fields & DEPTHCAM_IMU_TEMPERATURE) { n_temp++; last_temp = s; }
      have = true;
    }
  }
  else
  {
    n_video++;
  }
  if(f->release) f->release(f->owner);
}

int main(int argc, char** argv)
{
  void* lib = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
  if(!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
  const auto* b = ((score_depthcam_backend_v1_fn)dlsym(lib, "score_depthcam_backend_v1"))();
  printf("backend '%s' abi=%u\n", b->name, b->abi_version);
  if(b->abi_version != DEPTHCAM_ABI_VERSION) { printf("ABI mismatch\n"); return 1; }
  if(!b->init(argv[2])) { printf("init failed: %s\n", b->last_error()); return 1; }

  std::string uri = argc > 3 ? argv[3] : "";
  if(uri.empty())
    b->enumerate([](const depthcam_device_info* d, void* u) {
      if(((std::string*)u)->empty()) *(std::string*)u = d->uri; }, &uri);
  if(uri.empty()) { printf("no device\n"); return 1; }
  printf("uri %s\n", uri.c_str());

  depthcam_open_config cfg{};
  cfg.streams = DEPTHCAM_STREAM_DEPTH | DEPTHCAM_STREAM_IMU;
  auto* dev = b->open(uri.c_str(), &cfg);
  if(!dev) { printf("open failed: %s\n", b->last_error()); return 1; }
  if(const char* e = b->last_error()) printf("note: %s\n", e);

  if(!b->start(dev, &on_frame, nullptr)) { printf("start failed: %s\n", b->last_error()); return 1; }

  const auto t0 = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::seconds(5));
  const double secs
      = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  b->stop(dev);

  printf("in %.1fs: accel=%d (%.0f Hz)  gyro=%d (%.0f Hz)  temp=%d  video frames=%d\n",
         secs, n_accel.load(), n_accel / secs, n_gyro.load(), n_gyro / secs,
         n_temp.load(), n_video.load());

  if(have)
  {
    const auto& a = last_accel;
    const double g = std::sqrt(double(a.accel[0]) * a.accel[0]
                               + double(a.accel[1]) * a.accel[1]
                               + double(a.accel[2]) * a.accel[2]);
    printf("accel = %.3f %.3f %.3f m/s^2  |a| = %.3f (gravity is 9.807)\n",
           a.accel[0], a.accel[1], a.accel[2], g);
    printf("gyro  = %.4f %.4f %.4f rad/s\n", last_gyro.gyro[0], last_gyro.gyro[1],
           last_gyro.gyro[2]);
    if(n_temp > 0)
      printf("temp  = %.1f C\n", last_temp.temperature_c);
    else
      printf("temp  = not reported by this camera\n");

    // A camera sitting still measures one gravity and nothing else. That is the
    // only unit check available without a turntable.
    if(n_accel > 0)
      printf("%s\n", (g > 9.0 && g < 10.6) ? "  units look right (1 g at rest)"
                                            : "  UNEXPECTED magnitude -- units?");
  }
  else
  {
    printf("no IMU samples at all\n");
  }

  b->close(dev); b->shutdown();
  return have ? 0 : 2;
}
