#include "fluidgrain_browser.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

_Static_assert(offsetof(FGBrowserMailbox, input) == FG_BROWSER_HEADER, "input offset");
_Static_assert(offsetof(FGBrowserMailbox, output) ==
               FG_BROWSER_HEADER + FG_PACKET_MAX_BYTES, "output offset");
static void browser_put64(unsigned char *p, uint64_t word) {
  for (unsigned i = 0; i < 8; ++i) p[i] = (unsigned char)(word >> (8 * i));
}
static uint64_t browser_get64(const unsigned char *p) {
  uint64_t word = 0;
  for (unsigned i = 0; i < 8; ++i) word |= (uint64_t)p[i] << (8 * i);
  return word;
}
int fg_command_encode(void *output, const FGCommand *c, uint64_t key) {
  if (!output || !c || !key || !c->epoch || !c->sequence) return 0;
  for (unsigned i = 0; i < FG_CONTROL_COUNT; ++i)
    if (!isfinite(c->controls[i])) return 0;
  unsigned char *p = output;
  memset(p, 0, FG_COMMAND_BYTES);
  memcpy(p, "FGCM", 4); p[4] = 1;
  browser_put64(p + 8, key);
  browser_put64(p + 16, c->epoch); browser_put64(p + 24, c->reset_epoch);
  browser_put64(p + 32, c->sequence); browser_put64(p + 40, c->frame);
  for (unsigned i = 0; i < FG_CONTROL_COUNT; ++i) {
    uint64_t bits; memcpy(&bits, &c->controls[i], 8);
    browser_put64(p + 64 + 8 * i, bits);
  }
  return 1;
}
int fg_command_decode(const void *input, FGCommand *output, uint64_t key) {
  if (!input || !output || !key) return 0;
  const unsigned char *p = input;
  if (memcmp(p, "FGCM", 4) || p[4] != 1 || p[5] || p[6] || p[7] ||
      browser_get64(p + 8) != key) return 0;
  for (unsigned i = 48; i < 64; ++i) if (p[i]) return 0;
  FGCommand c = {.epoch = browser_get64(p + 16),
    .reset_epoch = browser_get64(p + 24), .sequence = browser_get64(p + 32),
    .frame = browser_get64(p + 40)};
  if (!c.epoch || !c.sequence) return 0;
  for (unsigned i = 0; i < FG_CONTROL_COUNT; ++i) {
    uint64_t bits = browser_get64(p + 64 + 8 * i);
    memcpy(&c.controls[i], &bits, 8);
    if (!isfinite(c.controls[i]) ||
        c.controls[i] < (i == FG_CONTROL_PITCH_RATIO ?
          log2(fg_control_parameters[i].minimum) : fg_control_parameters[i].minimum) ||
        c.controls[i] > (i == FG_CONTROL_PITCH_RATIO ?
          log2(fg_control_parameters[i].maximum) : fg_control_parameters[i].maximum)) return 0;
  }
  *output = c;
  return 1;
}
void fg_browser_init(FGBrowserMailbox *m, unsigned mode, unsigned n) {
  memset(m, 0, sizeof(*m));
  m->magic = FG_BROWSER_MAGIC; m->version = 2; m->bytes = FG_BROWSER_BYTES;
  m->mode = mode; m->packet_bytes = (uint32_t)fg_packet_size(n);
  m->command_bytes = FG_COMMAND_BYTES;
}
void fg_browser_service(FGBrowserMailbox *m, FGProvider *p) {
  if (m->mode == 0) {
    fg_provider_observe_acceptance(p, m->acceptance);
    if (m->resume) {
      m->resume = 0;
      if (!fg_provider_resume(p)) m->result = 1;
    }
    if (m->input_ready) {
      m->result = (uint32_t)fluidgrain_push_field(p, m->input, m->packet_bytes);
      m->input_ready = 0;
    }
    if (!m->output_ready) {
      FGCommand command;
      /* Consume at most one slot, including obsolete commands. */
      if (fg_provider_take_command(p, &command) == 1) {
        m->output_ready = (uint32_t)fg_command_encode(m->output, &command,
                                                    fg_provider_key(p));
      }
    }
  } else if (m->mode == 1 && m->input_ready && !m->output_ready) {
    FGCommand command;
    m->result = 1;
    if (fg_command_decode(m->input, &command, fg_provider_key(p)) &&
        fg_provider_solve_command(p, &command, m->output, m->packet_bytes)) {
      m->result = 0; m->output_ready = 1;
    }
    m->input_ready = 0;
  }
}
