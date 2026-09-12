/* Inspect actual storage as well as the public read-only copy. */
#include "../src/fluidgrain_core.c"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
int main(void) {
  /* A periodic crossing changes chart, not a flight through the middle. */
  FGEngine mapping_engine = {0};
  mapping_engine.smooth[FG_CONTROL_FLOW_SPEED] = 1;
  for (unsigned axis = 0; axis < 2; ++axis) {
    for (int direction = -1; direction <= 1; direction += 2) {
      FGVoice voice = {0};
      voice.particle.x = voice.particle.y = .4;
      if (axis == 0) voice.particle.x = direction > 0 ? .999 : .001;
      else voice.particle.y = direction > 0 ? .999 : .001;
      voice.particle.path_x = voice.particle.x;
      voice.particle.path_y = voice.particle.y;
      voice.particle.motion_speed = .5;
      sound_target(&mapping_engine, &voice);
      memcpy(voice.space, voice.space_target, sizeof(voice.space));
      if (axis == 0) {
        voice.particle.path_x += direction * .002;
        voice.particle.x = voice.particle.path_x - floor(voice.particle.path_x);
      } else {
        voice.particle.path_y += direction * .002;
        voice.particle.y = voice.particle.path_y - floor(voice.particle.path_y);
      }
      sound_target(&mapping_engine, &voice);
      for (unsigned a = 0; a < 3; ++a) CHECK(voice.space[a] == voice.space_target[a]);
    }
  }
  for (unsigned n = 16; n <= 64; n *= 2) {
    double cfg[FG_CONFIG_COUNT], ctl[FG_CONTROL_COUNT];
    fg_defaults(cfg, ctl); cfg[FG_CONFIG_GRID_SIZE] = n;
    cfg[FG_CONFIG_MAX_GRAINS] = 2048; cfg[FG_CONFIG_EMITTER_COUNT] = 4096;
    ctl[FG_CONTROL_GRAIN_RATE] = 2000; ctl[FG_CONTROL_GRAIN_MS] = 500;
    FGConfig c; CHECK(!fg_config_parse(&c, cfg, FG_CONFIG_COUNT));
    size_t bytes = fg_memory_size(&c, 997, 48000, 48000);
    void *memory = malloc(bytes), *before = malloc(bytes);
    FGEngine *e = fg_init(memory, bytes, &c, 997, 48000, 48000); CHECK(e);
    for (unsigned i = 0; i < 997; ++i) fg_source(e)[i] = sin(i * .02);
    fg_controls(e, ctl);
    double view[FG_VIEW_SIZE + 1]; view[FG_VIEW_SIZE] = 123;
    CHECK(!fg_view(e, view, FG_VIEW_SIZE - 1));
    CHECK(!fg_view(NULL, view, FG_VIEW_SIZE));
    CHECK(!fg_view(e, NULL, FG_VIEW_SIZE));
    for (unsigned frame = 0; frame < 12000; ++frame) {
      if (frame == 10000) { ctl[FG_CONTROL_RESET] = 1; fg_controls(e, ctl); }
      double l, r; fg_sample(e, &l, &r);
      if (frame % 64) continue;
      memcpy(before, memory, bytes);
      CHECK(fg_view(e, view, FG_VIEW_SIZE));
      CHECK(!memcmp(before, memory, bytes)); /* Includes RNG, peak, counters. */
      CHECK(view[FG_VIEW_SIZE] == 123);
      for (unsigned i = 0; i < FG_VIEW_SIZE; ++i) CHECK(isfinite(view[i]));
      CHECK(view[0] == 1 && view[1] == FG_VIEW_SIZE && view[7] == FG_VIEW_SIDE);
      CHECK(view[8] <= FG_VIEW_PARTICLES && view[9] <= FG_VIEW_PARTICLES);
      if (e->reset_stage == 2) { CHECK(view[8] == 0 && view[9] == 0 && view[11] == 0); continue; }
      if (view[11]) {
        FGFieldSample sample = fg_field_sample(&e->field, .5 / FG_VIEW_SIDE, .5 / FG_VIEW_SIDE);
        CHECK(view[16] == sample.u && view[17] == sample.v);
        CHECK(view[18] == sample.omega && view[19] == sample.strain);
      }
      for (unsigned i = 0; i < (unsigned)view[8]; ++i) {
        double *v = view + FG_VIEW_HEADER + 256 * FG_VIEW_FIELD_STRIDE + i * FG_VIEW_GRAIN_STRIDE;
        FGGrainState g; CHECK(fg_grain_state(e, (uint32_t)v[7], &g));
        CHECK(v[0] == (uint32_t)g.id && v[1] == (uint32_t)(g.id >> 32));
        CHECK(v[2] == g.x && v[3] == g.y && v[4] == g.path_x && v[5] == g.path_y);
      }
    }
    free(before); free(memory);
  }
  puts("view: actual field/grains, all grids, bounded pools, reset, no state mutation passed");
}
