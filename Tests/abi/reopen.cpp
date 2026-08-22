// open / close / reopen, and open-while-open, at the ABI level.
#include <depthcam_abi.h>
#include <dlfcn.h>
#include <cstdio>
#include <string>
#include <unistd.h>

int main(int argc, char** argv)
{
  void* lib = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
  const auto* b = ((score_depthcam_backend_v1_fn)dlsym(lib, "score_depthcam_backend_v1"))();
  if(!b->init(argv[2])) { printf("init failed\n"); return 1; }
  std::string uri;
  b->enumerate([](const depthcam_device_info* d, void* u) {
    if(((std::string*)u)->empty()) *(std::string*)u = d->uri; }, &uri);
  if(uri.empty()) { printf("no device\n"); return 1; }
  printf("uri %s\n", uri.c_str());

  depthcam_open_config cfg{};
  cfg.streams = DEPTHCAM_STREAM_DEPTH;

  auto* a = b->open(uri.c_str(), &cfg);
  printf("open #1: %s\n", a ? "ok" : b->last_error());
  b->close(a);

  // What does the backend see right after a close?
  for(int i = 0; i < 12; i++)
  {
    int n = 0;
    b->enumerate([](const depthcam_device_info* d, void* u) {
      (*(int*)u)++; printf("      %s\n", d->uri); }, &n);
    printf("  enumerate %d ms after close: %d device(s)\n", i * 500, n);
    if(n > 0) break;
    usleep(500000);
  }

  auto* c = b->open(uri.c_str(), &cfg);
  printf("open #2 after close: %s\n", c ? "ok" : b->last_error());
  if(!c) return 2;

  auto* d = b->open(uri.c_str(), &cfg);      // while #2 is still open
  printf("open #3 while #2 open: %s\n", d ? "ok" : b->last_error());
  if(d) b->close(d);

  b->close(c);
  auto* e = b->open(uri.c_str(), &cfg);
  printf("open #4 after the busy attempt: %s\n", e ? "ok" : b->last_error());
  if(e) b->close(e);
  b->shutdown();
  return e ? 0 : 3;
}
