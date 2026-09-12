#include "fluidgrain_browser.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)

static FGProvider *provider(FGConfig *config) {
  size_t bytes = fg_provider_size(config);
  FGProvider *p = fg_provider_init(malloc(bytes), bytes, config, 4800, UINT64_C(9007199254740993));
  CHECK(p); return p;
}
static FGEngine *engine(FGConfig *config, double *controls) {
  size_t bytes = fg_memory_size(config, 997, 4800, 4800);
  FGEngine *e = fg_init(malloc(bytes), bytes, config, 997, 4800, 4800);
  CHECK(e);
  for (unsigned i = 0; i < 997; ++i) fg_source(e)[i] = .3 * sin(i * .17);
  fg_controls(e, controls); return e;
}
static void split(unsigned n) {
  double cfg[FG_CONFIG_COUNT], controls[FG_CONTROL_COUNT];
  fg_defaults(cfg, controls);
  cfg[FG_CONFIG_BACKEND] = 1; cfg[FG_CONFIG_INSTANCE_ID] = 1;
  cfg[FG_CONFIG_GRID_SIZE] = n; cfg[FG_CONFIG_EMITTER_COUNT] = 16;
  cfg[FG_CONFIG_MAX_GRAINS] = 64; cfg[FG_CONFIG_SOURCE_LOOP] = 1;
  FGConfig config; CHECK(!fg_config_parse(&config, cfg, FG_CONFIG_COUNT));
  FGProvider *audio = provider(&config), *solver = provider(&config), *native = provider(&config);
  FGEngine *a = engine(&config, controls), *b = engine(&config, controls);
  CHECK(fg_provider_attach(audio, a)); CHECK(fg_provider_attach(native, b));
  FGBrowserMailbox *in = malloc(sizeof(*in)), *out = malloc(sizeof(*out));
  fg_browser_init(in, 0, n); fg_browser_init(out, 1, n);
  double energy = 0;
  unsigned solves = 0;
  uint64_t observed = 0;
  for (unsigned frame = 0; frame < 2400; ++frame) {
    if (frame == 600 || frame == 1200) {
      CHECK(fg_provider_resume(audio)); CHECK(fg_provider_resume(native));
    }
    controls[FG_CONTROL_RESET] = frame >= 1500 && frame < 1550;
    controls[FG_CONTROL_PITCH_RATIO] = frame < 1000 ? .25 : 4;
    fg_controls(a, controls); fg_controls(b, controls);
    double al, ar, bl, br; fg_sample(a, &al, &ar); fg_sample(b, &bl, &br);
    CHECK(al == bl && ar == br); energy += al*al + ar*ar;
    uint64_t sequence = in->acceptance[4] | ((uint64_t)in->acceptance[5] << 32);
    if (sequence != observed) {
      CHECK(in->acceptance[0] == frame && in->acceptance[1] == 0);
      CHECK((in->acceptance[2] | ((uint64_t)in->acceptance[3] << 32)) == fg_provider_stats(audio).epoch);
      CHECK(sequence > observed); observed = sequence;
    }
    /* Same service clock as the native producer: exact audio comparison. */
    CHECK(fg_provider_work(native) >= 0);
    fg_browser_service(in, audio);
    if (in->output_ready) {
      memcpy(out->input, in->output, FG_COMMAND_BYTES);
      out->input_ready = 1; in->output_ready = 0;
      fg_browser_service(out, solver);
      CHECK(out->result == 0 && out->output_ready); solves++;
      memcpy(in->input, out->output, in->packet_bytes);
      in->input_ready = 1; out->output_ready = 0;
      fg_browser_service(in, audio);
    }
  }
  CHECK(energy > .001 && solves > 10 && observed > 10);
  CHECK(fg_provider_stats(solver).processed == fg_provider_stats(native).processed);
  CHECK(fg_provider_stats(audio).applied == fg_provider_stats(native).applied);
  /* The last command is still in the owned input bytes. Replaying or corrupting
   * it must not advance the worker or publish a replacement field. */
  uint64_t processed = fg_provider_stats(solver).processed;
  out->input_ready = 1;
  fg_browser_service(out, solver);
  CHECK(out->result && !out->output_ready);
  CHECK(fg_provider_stats(solver).processed == processed);
  out->input[0] ^= 0xff;
  out->input_ready = 1;
  fg_browser_service(out, solver);
  CHECK(out->result && !out->output_ready);
  CHECK(fg_provider_stats(solver).processed == processed);
  free(a); free(b); free(audio); free(solver); free(native); free(in); free(out);
}
static void command_wire(void) {
  FGCommand c = {.epoch = UINT64_C(0xfedcba9876543210),
    .reset_epoch = 17, .sequence = UINT64_C(0x123456789abcdef0),
    .frame = UINT64_C(0xffffffffffffffff)}, copy = {0};
  double config[FG_CONFIG_COUNT]; fg_defaults(config, c.controls);
  c.controls[FG_CONTROL_PITCH_RATIO] = 0; /* internal log2 domain */
  unsigned char bytes[FG_COMMAND_BYTES];
  CHECK(fg_command_encode(bytes, &c, 7));
  CHECK(!memcmp(bytes, "FGCM\1\0\0\0", 8));
  CHECK(bytes[16] == 0x10 && bytes[23] == 0xfe && bytes[40] == 0xff);
  CHECK(fg_command_decode(bytes, &copy, 7));
  CHECK(!memcmp(&c, &copy, sizeof(c)));
  FGCommand before = copy;
  CHECK(!fg_command_decode(bytes, &copy, 8));
  bytes[48] = 1; CHECK(!fg_command_decode(bytes, &copy, 7)); bytes[48] = 0;
  bytes[4] = 2; CHECK(!fg_command_decode(bytes, &copy, 7)); bytes[4] = 1;
  /* +Inf in the gain field, exact little-endian double. */
  memset(bytes + 64, 0, 8); bytes[70] = 0xf0; bytes[71] = 0x7f;
  CHECK(!fg_command_decode(bytes, &copy, 7)); CHECK(!memcmp(&before, &copy, sizeof(copy)));
}
int main(void) {
  command_wire(); split(16); split(32); split(64);
  puts("browser mailbox: exact native/split audio on all grids; uint64 wire and rejection checks pass");
  return 0;
}
