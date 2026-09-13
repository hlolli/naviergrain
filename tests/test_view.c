/* Inspect actual storage as well as the public read-only copy. */
#include "../src/naviergrain_core.c"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
int main(void) {
  /* A periodic crossing changes chart, not a flight through the middle. */
  NGEngine mapping_engine = {0};
  mapping_engine.smooth[NG_CONTROL_FLOW_SPEED] = 1;
  for (unsigned axis = 0; axis < 2; ++axis) {
    for (int direction = -1; direction <= 1; direction += 2) {
      NGVoice voice = {0};
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
    double cfg[NG_CONFIG_COUNT], ctl[NG_CONTROL_COUNT];
    ng_defaults(cfg, ctl); cfg[NG_CONFIG_GRID_SIZE] = n;
    cfg[NG_CONFIG_MAX_GRAINS] = 2048; cfg[NG_CONFIG_EMITTER_COUNT] = 4096;
    ctl[NG_CONTROL_GRAIN_RATE] = 2000; ctl[NG_CONTROL_GRAIN_MS] = 500;
    NGConfig c; CHECK(!ng_config_parse(&c, cfg, NG_CONFIG_COUNT));
    size_t bytes = ng_memory_size(&c, 997, 48000, 48000);
    void *memory = malloc(bytes), *before = malloc(bytes);
    NGEngine *e = ng_init(memory, bytes, &c, 997, 48000, 48000); CHECK(e);
    for (unsigned i = 0; i < 997; ++i) ng_source(e)[i] = sin(i * .02);
    ng_controls(e, ctl);
    double view[NG_VIEW_SIZE + 1]; view[NG_VIEW_SIZE] = 123;
    CHECK(!ng_view(e, view, NG_VIEW_SIZE - 1));
    CHECK(!ng_view(NULL, view, NG_VIEW_SIZE));
    CHECK(!ng_view(e, NULL, NG_VIEW_SIZE));
    for (unsigned frame = 0; frame < 12000; ++frame) {
      if (frame == 10000) { ctl[NG_CONTROL_RESET] = 1; ng_controls(e, ctl); }
      double l, r; ng_sample(e, &l, &r);
      if (frame % 64) continue;
      memcpy(before, memory, bytes);
      CHECK(ng_view(e, view, NG_VIEW_SIZE));
      CHECK(!memcmp(before, memory, bytes)); /* Includes RNG, peak, counters. */
      CHECK(view[NG_VIEW_SIZE] == 123);
      for (unsigned i = 0; i < NG_VIEW_SIZE; ++i) CHECK(isfinite(view[i]));
      CHECK(view[0] == 1 && view[1] == NG_VIEW_SIZE && view[7] == NG_VIEW_SIDE);
      CHECK(view[8] <= NG_VIEW_PARTICLES && view[9] <= NG_VIEW_PARTICLES);
      if (e->reset_stage == 2) { CHECK(view[8] == 0 && view[9] == 0 && view[11] == 0); continue; }
      if (view[11]) {
        NGFieldSample sample = ng_field_sample(&e->field, .5 / NG_VIEW_SIDE, .5 / NG_VIEW_SIDE);
        CHECK(view[16] == sample.u && view[17] == sample.v);
        CHECK(view[18] == sample.omega && view[19] == sample.strain);
      }
      for (unsigned i = 0; i < (unsigned)view[8]; ++i) {
        double *v = view + NG_VIEW_HEADER + 256 * NG_VIEW_FIELD_STRIDE + i * NG_VIEW_GRAIN_STRIDE;
        NGGrainState g; CHECK(ng_grain_state(e, (uint32_t)v[7], &g));
        CHECK(v[0] == (uint32_t)g.id && v[1] == (uint32_t)(g.id >> 32));
        CHECK(v[2] == g.x && v[3] == g.y && v[4] == g.path_x && v[5] == g.path_y);
      }
    }
    free(before); free(memory);
  }
  puts("view: actual field/grains, all grids, bounded pools, reset, no state mutation passed");
}
