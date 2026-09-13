/* Real Csound API lifecycle, using the shipped persistent-instrument example.
 * No device is opened: preparation and teardown belong to the stopped host.
 * This test proves state/ownership, not hardware callback deadlines. */
#include <csound.h>
#include "naviergrain_schema.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void messages(CSOUND *host, int print) {
  while (csoundGetMessageCnt(host)) {
    const char *message = csoundGetFirstMessage(host);
    if (print && message)
      fputs(message, stderr);
    csoundPopFirstMessage(host);
  }
}
static void check(CSOUND *host, int condition, const char *reason, int line) {
  if (!condition) {
    fprintf(stderr, "host test line %d: %s\n", line, reason);
    messages(host, 1);
    exit(1);
  }
}
#define CHECK(h, condition) check(h, (condition), #condition, __LINE__)

static double channel(CSOUND *host, const char *name) {
  int32_t error = 0;
  double value = csoundGetControlChannel(host, name, &error);
  CHECK(host, !error && isfinite(value));
  return value;
}
static void quiet_block(CSOUND *host) {
  CHECK(host, csoundPerformKsmps(host) == 0);
  const MYFLT *audio = csoundGetSpout(host);
  for (unsigned i = 0; i < 2 * csoundGetKsmps(host); ++i)
    CHECK(host, audio[i] == 0);
}
static void options(CSOUND *host, const char *module, unsigned block, unsigned sr) {
  char option[4096];
  int length = snprintf(option, sizeof(option), "--opcode-lib=%s", module);
  CHECK(host, length > 0 && (size_t)length < sizeof(option));
  CHECK(host, csoundSetOption(host, option) == 0);
  snprintf(option, sizeof(option), "--ksmps=%u", block);
  CHECK(host, csoundSetOption(host, option) == 0);
  snprintf(option, sizeof(option), "--sample-rate=%u", sr);
  CHECK(host, csoundSetOption(host, option) == 0);
}
static void prepare(CSOUND *host, const char *module, unsigned block,
                    unsigned sr, unsigned preroll) {
  options(host, module, block, sr);
  CHECK(host, csoundCompileCSD(host, "examples/prepared.csd", 0, 0) == 0);
  CHECK(host, csoundStart(host) == 0);
  CHECK(host, csoundGetKsmps(host) == block);
  for (unsigned i = 0; i < preroll; ++i) {
    quiet_block(host);
    CHECK(host, channel(host, "ng.status") == NG_STATUS_READY);
    CHECK(host, channel(host, "ng.source_length") == 997);
    CHECK(host, channel(host, "ng.live") == 0);
    CHECK(host, channel(host, "ng.sequence") == 0);
    CHECK(host, channel(host, "ng.age") == 0);
  }
  csoundSetControlChannel(host, "ng.rate", 80);
  csoundSetControlChannel(host, "ng.gain", .03);
  csoundSetControlChannel(host, "ng.pitch", 1);
  csoundSetControlChannel(host, "ng.run", 1);
}
static void automation(CSOUND *host, unsigned block) {
  csoundSetControlChannel(host, "ng.freeze", block >= 50 && block < 90);
  csoundSetControlChannel(host, "ng.reset", block >= 150 && block < 190);
  csoundSetControlChannel(host, "ng.pitch", block < 100 ? 1 : 1.5);
}
static void pause_transport(CSOUND *host) {
  const char *names[] = {"ng.status", "ng.live", "ng.sequence", "ng.age", "ng.epoch"};
  double before[5];
  for (unsigned i = 0; i < 5; ++i)
    before[i] = channel(host, names[i]);
  csoundSetControlChannel(host, "ng.run", 0);
  for (unsigned block = 0; block < 13; ++block) {
    quiet_block(host);
    for (unsigned i = 0; i < 5; ++i)
      CHECK(host, channel(host, names[i]) == before[i]);
  }
  csoundSetControlChannel(host, "ng.run", 1);
}
static void stopped_reset(CSOUND *host) {
  /* The caller has stopped invoking performance; no audio thread or worker
   * can hold an opcode pointer when reset reclaims its AUXCH storage. */
  messages(host, 0);
  csoundReset(host);
  /* This checkout retains the buffer but resets the message callback. */
  csoundCreateMessageBuffer(host, 0);
}
static void invalid_automation(CSOUND *reference, CSOUND *faulted,
                               const char *module) {
  prepare(reference, module, 37, 48000, 1);
  prepare(faulted, module, 37, 48000, 1);
  const char *names[] = {"ng.gain", "ng.rate", "ng.pitch", "ng.freeze", "ng.reset"};
  const double initial[] = {.03, 80, 1, 0, 0};
  const double minimum[] = {0, 0, .25, 0, 0};
  const double maximum[] = {2, 32000, 4, 1, 1};
  double energy = 0;
  for (unsigned channel_index = 0; channel_index < 5; ++channel_index) {
    const double faults[] = {NAN, INFINITY, -INFINITY,
                            minimum[channel_index] - 1,
                            maximum[channel_index] + 1, .5};
    double last = initial[channel_index];
    for (unsigned fault = 0; fault < 6; ++fault) {
      double value = faults[fault];
      if (isfinite(value) && channel_index < 3)
        last = fmin(maximum[channel_index], fmax(minimum[channel_index], value));
      csoundSetControlChannel(reference, names[channel_index], last);
      csoundSetControlChannel(faulted, names[channel_index], value);
      for (unsigned block = 0; block < 19; ++block) {
        CHECK(reference, csoundPerformKsmps(reference) == 0);
        CHECK(faulted, csoundPerformKsmps(faulted) == 0);
        const MYFLT *a = csoundGetSpout(reference), *b = csoundGetSpout(faulted);
        for (unsigned i = 0; i < 74; ++i) {
          CHECK(faulted, isfinite(b[i]) && a[i] == b[i]);
          energy += b[i] * b[i];
        }
      }
      CHECK(faulted, (unsigned)channel(faulted, "ng.status") &
                         NG_STATUS_CONTROLS_CLAMPED);
      CHECK(faulted, channel(faulted, "ng.numeric") == 0);
    }
    csoundSetControlChannel(reference, names[channel_index], initial[channel_index]);
    csoundSetControlChannel(faulted, names[channel_index], initial[channel_index]);
  }
  CHECK(faulted, energy > 0);
  CHECK(faulted, channel(faulted, "ng.epoch") == 0);
  stopped_reset(reference);
  stopped_reset(faulted);
  puts("prepared host: 30 invalid/endpoint automation cases match sanitized controls exactly");
}
static void failed_preparation(CSOUND *host, const char *module, int source_fault) {
  options(host, module, 64, 48000);
  const char *csd =
      "<CsoundSynthesizer>\n<CsOptions>\n-n -d -m0\n</CsOptions>\n"
      "<CsInstruments>\nsr=48000\nksmps=64\nnchnls=2\n0dbfs=1\n"
      "#include \"include/naviergrain.inc\"\n"
      "#include \"include/naviergrain-prepared.inc\"\n"
      "giSource ftgen 123,0,-997,10,1\n"
      "instr 1\niRate chnget \"failed.rate\"\n"
      "iConfig[] fillarray $NG_CONFIG_DEFAULTS\n"
      "kControl[] fillarray $NG_CONTROL_DEFAULTS\n"
      "aL,aR,kStats[] NaviergrainPrepared giSource,iRate,iConfig,kControl,0\n"
      "chnset kStats[$NG_STAT_STATUS],\"failed.status\"\n"
      "outs aL,aR\nendin\n</CsInstruments>\n"
      "<CsScore>\ni 1 0 -1\nf 0 z\n</CsScore>\n</CsoundSynthesizer>\n";
  CHECK(host, csoundCompileCSD(host, csd, 1, 0) == 0);
  CHECK(host, csoundStart(host) == 0);
  csoundSetControlChannel(host, "failed.rate", source_fault < 0 ? 0 : 48000);
  if (source_fault >= 0) {
    const double faults[] = {NAN, INFINITY, -INFINITY};
    MYFLT *table = NULL;
    CHECK(host, csoundGetTable(host, &table, 123) == 997 && table);
    table[17] = faults[source_fault];
  }
  /* A failed instrument init need not terminate the entire Csound score. */
  csoundPerformKsmps(host);
  CHECK(host, channel(host, "failed.status") == 0);
  int diagnosed = 0;
  while (csoundGetMessageCnt(host)) {
    const char *message = csoundGetFirstMessage(host);
    if (message && strstr(message, source_fault < 0
            ? "source sample rate must be finite and positive"
            : "source contains a non-finite sample"))
      diagnosed = 1;
    csoundPopFirstMessage(host);
  }
  CHECK(host, diagnosed);
  const MYFLT *audio = csoundGetSpout(host);
  for (unsigned i = 0; i < 128; ++i)
    CHECK(host, audio[i] == 0);
}
static void observation_host(CSOUND *host, const char *module, int observed, int size, int table) {
  char csd[4096];
  snprintf(csd, sizeof(csd),
    "<CsoundSynthesizer>\n<CsOptions>\n-n -d -m0\n</CsOptions>\n<CsInstruments>\n"
    "sr=48000\nksmps=64\nnchnls=2\n0dbfs=1\n"
    "#include \"include/naviergrain.inc\"\n#include \"include/naviergrain-prepared.inc\"\n"
    "giSource ftgen 1,0,-997,10,1\ngiView ftgen 2,0,-%d,-2,0\n"
    "instr 1\niCfg[] fillarray $NG_CONFIG_DEFAULTS\nkCtl[] fillarray $NG_CONTROL_DEFAULTS\n"
    "kRun chnget \"view.run\"\nkObserve chnget \"view.request\"\n"
    "kCtl[$NG_CONTROL_RESET] chnget \"view.reset\"\n"
    "aL,aR,kStats[] %s giSource,48000,iCfg,kCtl,kRun%s\n"
    "chnset kStats[$NG_STAT_STATUS],\"view.status\"\nouts aL,aR\nendin\n"
    "</CsInstruments>\n<CsScore>\ni 1 0 -1\nf 0 z\n</CsScore>\n</CsoundSynthesizer>\n",
    size, observed ? "NaviergrainVisualPrepared" : "NaviergrainPrepared",
    observed ? (table == 1 ? ",1,kObserve" : ",giView,kObserve") : "");
  options(host, module, 64, 48000);
  CHECK(host, csoundCompileCSD(host, csd, 1, 0) == 0);
  CHECK(host, csoundStart(host) == 0);
}
static void observation(CSOUND *a, CSOUND *b, const char *module) {
  observation_host(a, module, 0, 3344, 2);
  observation_host(b, module, 1, 3344, 2);
  quiet_block(a); quiet_block(b);
  csoundSetControlChannel(a, "view.run", 1); csoundSetControlChannel(b, "view.run", 1);
  double peak = 0;
  for (unsigned block = 0; block < 400; ++block) {
    csoundSetControlChannel(a, "view.reset", block == 120);
    csoundSetControlChannel(b, "view.reset", block == 120);
    csoundSetControlChannel(b, "view.request", block % 3 == 0);
    CHECK(a, csoundPerformKsmps(a) == 0); CHECK(b, csoundPerformKsmps(b) == 0);
    const MYFLT *left = csoundGetSpout(a), *right = csoundGetSpout(b);
    for (unsigned i = 0; i < 128; ++i) {
      CHECK(b, left[i] == right[i] && isfinite(left[i])); peak = fmax(peak, fabs(left[i]));
    }
    MYFLT *view; CHECK(b, csoundGetTable(b, &view, 2) == 3344);
    CHECK(b, view[0] == 1 && view[1] == 3344 && view[8] <= 128);
    for (unsigned i = 0; i < 3344; ++i) CHECK(b, isfinite(view[i]));
  }
  CHECK(b, peak > .001);
  stopped_reset(a); stopped_reset(b);
  for (int fault = 0; fault < 2; ++fault) {
    observation_host(b, module, 1, fault ? 3344 : 8, fault ? 1 : 2);
    csoundPerformKsmps(b);
    CHECK(b, channel(b, "view.status") == 0);
    const MYFLT *audio = csoundGetSpout(b);
    for (unsigned i = 0; i < 128; ++i) CHECK(b, audio[i] == 0);
    int diagnosed = 0;
    while (csoundGetMessageCnt(b)) {
      if (strstr(csoundGetFirstMessage(b), "snapshot table")) diagnosed = 1;
      csoundPopFirstMessage(b);
    }
    CHECK(b, diagnosed); stopped_reset(b);
  }
  puts("observation host: exact audio, reset, bounded table and failed-init recovery passed");
}
int main(int argc, char **argv) {
  if (argc != 2 || csoundGetSizeOfMYFLT() != sizeof(MYFLT))
    return 1;
  CSOUND *reference = csoundCreate(NULL, NULL), *paused = csoundCreate(NULL, NULL);
  if (!reference || !paused)
    return 1;
  csoundCreateMessageBuffer(reference, 0);
  csoundCreateMessageBuffer(paused, 0);
  unsigned blocks[] = {64, 37, 64}, rates[] = {48000, 44100, 96000};
  for (unsigned cycle = 0; cycle < 3; ++cycle) {
    prepare(reference, argv[1], blocks[cycle], rates[cycle], 1);
    prepare(paused, argv[1], blocks[cycle], rates[cycle], 31);
    double peak = 0;
    /* >0.3 seconds even at 96 kHz, including reset completion. */
    for (unsigned block = 0; block < 500; ++block) {
      automation(reference, block);
      automation(paused, block);
      if (block == 125 || block == 165)
        pause_transport(paused); /* Also stop partway through a reset. */
      CHECK(reference, csoundPerformKsmps(reference) == 0);
      CHECK(paused, csoundPerformKsmps(paused) == 0);
      const MYFLT *a = csoundGetSpout(reference), *b = csoundGetSpout(paused);
      for (unsigned i = 0; i < 2 * blocks[cycle]; ++i) {
        CHECK(paused, isfinite(a[i]) && a[i] == b[i] && fabs(a[i]) < 1);
        if (fabs(a[i]) > peak)
          peak = fabs(a[i]);
      }
    }
    CHECK(paused, peak > .001);
    CHECK(paused, channel(paused, "ng.epoch") == 1);
    CHECK(paused, channel(paused, "ng.numeric") == 0);
    CHECK(paused, channel(paused, "ng.drops") == 0);
    /* Turning off is tested while offline, never prescribed on a live thread. */
    MYFLT turnoff[] = {-1, 0, 0};
    csoundEvent(paused, CS_INSTR_EVENT, turnoff, 3, 0);
    quiet_block(paused);
    printf("prepared host: %u Hz / %u samples, exact gated/ungated audio, "
           "reset epoch 1, peak %.6f\n", rates[cycle], blocks[cycle], peak);
    stopped_reset(reference);
    stopped_reset(paused);
  }
  observation(reference, paused, argv[1]);
  invalid_automation(reference, paused, argv[1]);
  for (int fault = -1; fault < 3; ++fault) {
    failed_preparation(paused, argv[1], fault);
    stopped_reset(paused);
  }
  prepare(paused, argv[1], 64, 48000, 1);
  CHECK(paused, csoundPerformKsmps(paused) == 0);
  CHECK(paused, channel(paused, "ng.status") == NG_STATUS_READY);
  csoundDestroyMessageBuffer(reference);
  csoundDestroyMessageBuffer(paused);
  csoundDestroy(reference);
  csoundDestroy(paused);
  puts("prepared host: failed-init recovery and stopped reset/recompile passed");
  return 0;
}
