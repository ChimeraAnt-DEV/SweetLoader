// A complete, minimal `.universe` native mod.
//
// It logs the host ABI it found, reads the local player once the world feed is bound, and counts
// frames. Build it as a shared library and ship it at `lib/libexample_universe.so` inside a
// `.universe` package (see the manifest beside this file).
//
// Build (NDK):
//   $CC -shared -fPIC -I<repo>/include/pl/universe example_mod.c -o libexample_universe.so

#include <pl/universe/universe_api.h>

#include <stdint.h>
#include <string.h>

static const universe_host_t *g_host = NULL;
static uint64_t g_frames = 0;
static int g_reported_player = 0;

static void host_log(const universe_host_t *host, int level, const char *message) {
  if (host != NULL && host->log != NULL) {
    host->log(level, message);
  }
}

int32_t universe_init(const universe_host_t *host) {
  // Check the ABI before touching any field: a mismatched table is refused by a number, not by
  // calling through a moved pointer.
  if (host == NULL || host->size < sizeof(universe_host_t) ||
      host->abi_version != UNIVERSE_HOST_ABI_VERSION) {
    return 1;
  }
  g_host = host;
  host_log(host, UNIVERSE_LOG_INFO, "example_universe: init ok");
  return 0;
}

void universe_tick(uint64_t frame, float dt) {
  (void)dt;
  if (g_host == NULL) {
    return;
  }
  g_frames = frame;

  // The local-player feed is optional; the pointer is null until the loader binds it, so a mod
  // must check before calling.
  if (g_host->get_local_player != NULL && !g_reported_player) {
    float position[3] = {0.0f, 0.0f, 0.0f};
    float rotation[2] = {0.0f, 0.0f};
    if (g_host->get_local_player(position, rotation)) {
      g_reported_player = 1;
      host_log(g_host, UNIVERSE_LOG_INFO, "example_universe: local player read");
    }
  }

  // Log once every 600 frames (roughly every 10 seconds at 60 fps) so the log stays readable.
  if (frame % 600 == 0) {
    char message[64];
    // A fixed, short message: the loader's log takes a NUL-terminated string.
    strcpy(message, "example_universe: still running");
    host_log(g_host, UNIVERSE_LOG_DEBUG, message);
  }
}

void universe_shutdown(void) {
  if (g_host != NULL) {
    host_log(g_host, UNIVERSE_LOG_INFO, "example_universe: shutdown");
  }
  g_host = NULL;
}
