// Does enumeration interfere with an open device, and vice versa?
#include <depthcam_abi.h>
#include <dlfcn.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

static std::atomic<int> frames{0};
static void on_frame(const depthcam_frame* f, void*) { frames++; if(f->release) f->release(f->owner); }

static int enum_count(const depthcam_backend_v1* b)
{
  int n = 0;
  b->enumerate([](const depthcam_device_info*, void* u) { (*(int*)u)++; }, &n);
  return n;
}

int main(int argc, char** argv)
{
  void* lib = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
  const auto* b = ((score_depthcam_backend_v1_fn)dlsym(lib, "score_depthcam_backend_v1"))();
  if(!b->init(argv[2])) { printf("init failed: %s\n", b->last_error()); return 1; }

  std::string uri;
  b->enumerate([](const depthcam_device_info* d, void* u) {
    if(((std::string*)u)->empty()) *(std::string*)u = d->uri; }, &uri);
  printf("enumerate #1: %s\n", uri.empty() ? "(nothing)" : uri.c_str());
  if(uri.empty()) return 1;

  // What the device browser does while the user is still choosing.
  printf("enumerate #2 immediately after: %d device(s)\n", enum_count(b));

  depthcam_open_config cfg{};
  cfg.streams = DEPTHCAM_STREAM_COLOR | DEPTHCAM_STREAM_DEPTH;
  auto* dev = b->open(uri.c_str(), &cfg);
  printf("open right after enumerate: %s\n", dev ? "OK" : b->last_error());
  if(!dev) return 2;

  printf("enumerate while the device is open: %d device(s)  (err: %s)\n",
         enum_count(b), b->last_error() ? b->last_error() : "-");

  b->start(dev, &on_frame, nullptr);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  printf("frames after 2s of streaming: %d\n", frames.load());

  printf("enumerate while streaming: %d device(s)\n", enum_count(b));
  std::this_thread::sleep_for(std::chrono::seconds(2));
  printf("frames after 2 more s: %d\n", frames.load());

  b->stop(dev); b->close(dev); b->shutdown();
  return 0;
}
