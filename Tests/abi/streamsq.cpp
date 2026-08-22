#include <depthcam_abi.h>
#include <dlfcn.h>
#include <cstdio>
#include <string>
int main(int argc, char** argv)
{
  void* lib = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
  const auto* b = ((score_depthcam_backend_v1_fn)dlsym(lib, "score_depthcam_backend_v1"))();
  if(!b->init(argv[2])) { printf("init failed\n"); return 1; }
  std::string uri;
  b->enumerate([](const depthcam_device_info* d, void* u) {
    if(((std::string*)u)->empty()) *(std::string*)u = d->uri; }, &uri);
  if(uri.empty()) { printf("no device\n"); return 1; }
  depthcam_open_config cfg{};
  cfg.streams = DEPTHCAM_STREAM_COLOR | DEPTHCAM_STREAM_IR | DEPTHCAM_STREAM_DEPTH | DEPTHCAM_STREAM_POINTCLOUD | DEPTHCAM_STREAM_IMU;
  cfg.align = DEPTHCAM_ALIGN_COLOR_TO_DEPTH;
  auto* dev = b->open(uri.c_str(), &cfg);
  if(!dev) { printf("open failed: %s\n", b->last_error()); return 1; }
  printf("active_streams fn: %s\n", b->active_streams ? "present" : "NULL");
  if(b->active_streams)
  {
    const uint32_t a = b->active_streams(dev);
    printf("asked 0x%x, got 0x%x  imu=%s\n", cfg.streams, a,
           (a & DEPTHCAM_STREAM_IMU) ? "yes" : "NO");
  }
  b->close(dev); b->shutdown();
  return 0;
}
