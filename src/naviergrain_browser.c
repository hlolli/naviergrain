#include "naviergrain_browser.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

_Static_assert(offsetof(NGBrowserMailbox, input) == NG_BROWSER_HEADER, "input offset");
_Static_assert(offsetof(NGBrowserMailbox, output) ==
               NG_BROWSER_HEADER + NG_PACKET_MAX_BYTES, "output offset");
static void browser_put64(unsigned char *p, uint64_t word) {
  for (unsigned i = 0; i < 8; ++i) p[i] = (unsigned char)(word >> (8 * i));
}
static uint64_t browser_get64(const unsigned char *p) {
  uint64_t word = 0;
  for (unsigned i = 0; i < 8; ++i) word |= (uint64_t)p[i] << (8 * i);
  return word;
}
int ng_command_encode(void *output, const NGCommand *c, uint64_t key) {
  if (!output || !c || !key || !c->epoch || !c->sequence) return 0;
  for (unsigned i = 0; i < NG_CONTROL_COUNT; ++i)
    if (!isfinite(c->controls[i])) return 0;
  unsigned char *p = output;
  memset(p, 0, NG_COMMAND_BYTES);
  memcpy(p, "NGCM", 4); p[4] = 1;
  browser_put64(p + 8, key);
  browser_put64(p + 16, c->epoch); browser_put64(p + 24, c->reset_epoch);
  browser_put64(p + 32, c->sequence); browser_put64(p + 40, c->frame);
  for (unsigned i = 0; i < NG_CONTROL_COUNT; ++i) {
    uint64_t bits; memcpy(&bits, &c->controls[i], 8);
    browser_put64(p + 64 + 8 * i, bits);
  }
  return 1;
}
int ng_command_decode(const void *input, NGCommand *output, uint64_t key) {
  if (!input || !output || !key) return 0;
  const unsigned char *p = input;
  if (memcmp(p, "NGCM", 4) || p[4] != 1 || p[5] || p[6] || p[7] ||
      browser_get64(p + 8) != key) return 0;
  for (unsigned i = 48; i < 64; ++i) if (p[i]) return 0;
  NGCommand c = {.epoch = browser_get64(p + 16),
    .reset_epoch = browser_get64(p + 24), .sequence = browser_get64(p + 32),
    .frame = browser_get64(p + 40)};
  if (!c.epoch || !c.sequence) return 0;
  for (unsigned i = 0; i < NG_CONTROL_COUNT; ++i) {
    uint64_t bits = browser_get64(p + 64 + 8 * i);
    memcpy(&c.controls[i], &bits, 8);
    if (!isfinite(c.controls[i]) ||
        c.controls[i] < (i == NG_CONTROL_PITCH_RATIO ?
          log2(ng_control_parameters[i].minimum) : ng_control_parameters[i].minimum) ||
        c.controls[i] > (i == NG_CONTROL_PITCH_RATIO ?
          log2(ng_control_parameters[i].maximum) : ng_control_parameters[i].maximum)) return 0;
  }
  *output = c;
  return 1;
}
void ng_browser_init(NGBrowserMailbox *m, unsigned mode, unsigned n) {
  memset(m, 0, sizeof(*m));
  m->magic = NG_BROWSER_MAGIC; m->version = 2; m->bytes = NG_BROWSER_BYTES;
  m->mode = mode; m->packet_bytes = (uint32_t)ng_packet_size(n);
  m->command_bytes = NG_COMMAND_BYTES;
}
void ng_browser_service(NGBrowserMailbox *m, NGProvider *p) {
  if (m->mode == 0) {
    ng_provider_observe_acceptance(p, m->acceptance);
    if (m->resume) {
      m->resume = 0;
      if (!ng_provider_resume(p)) m->result = 1;
    }
    if (m->input_ready) {
      m->result = (uint32_t)naviergrain_push_field(p, m->input, m->packet_bytes);
      m->input_ready = 0;
    }
    if (!m->output_ready) {
      NGCommand command;
      /* Consume at most one slot, including obsolete commands. */
      if (ng_provider_take_command(p, &command) == 1) {
        m->output_ready = (uint32_t)ng_command_encode(m->output, &command,
                                                    ng_provider_key(p));
      }
    }
  } else if (m->mode == 1 && m->input_ready && !m->output_ready) {
    NGCommand command;
    m->result = 1;
    if (ng_command_decode(m->input, &command, ng_provider_key(p)) &&
        ng_provider_solve_command(p, &command, m->output, m->packet_bytes)) {
      m->result = 0; m->output_ready = 1;
    }
    m->input_ready = 0;
  }
}
