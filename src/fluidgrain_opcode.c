#include "fluidgrain_core.h"
#include <csdl.h>
#include <arrays.h>
#include <math.h>
#include <string.h>
#include "fluidgrain_provider.h"
#include "fluidgrain_browser.h"
#include "fluidgrain_pack.h"
#define FG_REGISTRY_NAME "::fluidgrain.providers.v1"
typedef struct { FGProvider *providers[16]; } FGRegistry;
static int register_provider(void *host, FGProvider *provider) {
  CSOUND *csound = host;
  if (!csound || !provider || fg_provider_claimed(provider)) return 0;
  FGRegistry *registry = csound->QueryGlobalVariable(csound, FG_REGISTRY_NAME);
  if (!registry) {
    if (csound->CreateGlobalVariable(csound, FG_REGISTRY_NAME, sizeof(FGRegistry)))
      return 0;
    registry = csound->QueryGlobalVariable(csound, FG_REGISTRY_NAME);
    if (!registry) return 0;
    memset(registry, 0, sizeof(*registry));
  }
  for (unsigned i = 0; i < 16; ++i)
    if (fg_provider_key(registry->providers[i]) == fg_provider_key(provider))
      return 0;
  for (unsigned i = 0; i < 16; ++i)
    if (!registry->providers[i]) { registry->providers[i] = provider; return 1; }
  return 0;
}
static int unregister_provider(void *host, FGProvider *provider) {
  CSOUND *csound = host;
  if (!csound || !provider || fg_provider_claimed(provider)) return 0;
  FGRegistry *registry = csound->QueryGlobalVariable(csound, FG_REGISTRY_NAME);
  if (!registry) return 0;
  for (unsigned i = 0; i < 16; ++i)
    if (registry->providers[i] == provider) {
      registry->providers[i] = NULL; return 1;
    }
  return 0;
}
PUBLIC const FGNativeAPI *fluidgrain_native_api(void) {
  static const FGNativeAPI api = {1, fg_provider_size, fg_provider_init,
    register_provider, unregister_provider, fg_provider_work,
    fg_provider_resume, fg_provider_stats};
  return &api;
}

typedef struct {
  OPDS h;
  MYFLT *left, *right;
  ARRAYDAT *stats;
  MYFLT *table, *source_sr;
  ARRAYDAT *config, *control;
  MYFLT *view_table, *view_request; /* Only fluidgrain_visual binds these. */
  AUXCH memory;
  FUNC *view;
  AUXCH view_memory;
  double *view_copy;
  FGEngine *engine;
  FGProvider *provider;
} FLUIDGRAIN;

static int vector(const ARRAYDAT *a, int n) {
  return a && a->dimensions == 1 && a->sizes && a->sizes[0] == n && a->data &&
         a->arrayMemberSize == (int)sizeof(MYFLT);
}
static int32_t init(CSOUND *csound, FLUIDGRAIN *p) {
  /* Reinit must not attach a second engine to an existing live provider. */
  if (p->provider)
    return csound->InitError(csound, "fluidgrain: external reinit requires stopped host reprepare");
  p->engine = NULL;
  if (!vector(p->config, FG_CONFIG_COUNT) ||
      !vector(p->control, FG_CONTROL_COUNT))
    return csound->InitError(
        csound, "fluidgrain: expected 1D config[13] and control[24]");
  double values[FG_CONFIG_COUNT];
  for (int i = 0; i < FG_CONFIG_COUNT; ++i)
    values[i] = p->config->data[i];
  FGConfig config;
  const char *error = fg_config_parse(&config, values, FG_CONFIG_COUNT);
  if (error)
    return csound->InitError(csound, "fluidgrain: %s", error);
  if (config.value[FG_CONFIG_BACKEND] == 1) {
    FGRegistry *registry = csound->QueryGlobalVariable(csound, FG_REGISTRY_NAME);
    FGProvider *provider = NULL;
    if (registry)
      for (unsigned i = 0; i < 16; ++i)
        if (fg_provider_key(registry->providers[i]) ==
            (uint64_t)config.value[FG_CONFIG_INSTANCE_ID])
          provider = registry->providers[i];
    if (!provider || fg_provider_claimed(provider))
      return csound->InitError(csound, "fluidgrain: external backend needs an unclaimed prepared provider");
  }
  if (!isfinite(*p->source_sr) || *p->source_sr <= 0)
    return csound->InitError(
        csound, "fluidgrain: source sample rate must be finite and positive");
  if (!isfinite(*p->table) || *p->table < 1 || *p->table > INT32_MAX ||
      *p->table != floor(*p->table))
    return csound->InitError(
        csound, "fluidgrain: source table must be a positive integer");
  FUNC *source = csound->FTFind(csound, p->table);
  if (!source || !source->ftable || !source->flen)
    return csound->InitError(csound,
                             "fluidgrain: source table is missing or empty");
  /* GEN01 records channels; synthetic GEN tables have 0 or 1 channels. */
  if (source->nchanls < 0 || source->nchanls > 1)
    return csound->InitError(csound, "fluidgrain: source table must be mono");
  for (uint32_t i = 0; i < source->flen; ++i)
    if (!isfinite(source->ftable[i]))
      return csound->InitError(
          csound, "fluidgrain: source contains a non-finite sample");
  size_t bytes = fg_memory_size(&config, source->flen, *p->source_sr, CS_ESR);
  if (!bytes)
    return csound->InitError(csound, "fluidgrain: invalid allocation size");
  /* Offline preview: AUXCH belongs to the opcode/engine; no custom teardown
   * required. */
  csound->AuxAlloc(csound, bytes, &p->memory);
  if (!p->memory.auxp)
    return csound->InitError(csound, "fluidgrain: allocation failed");
  p->engine = fg_init(p->memory.auxp, bytes, &config, source->flen,
                      *p->source_sr, CS_ESR);
  if (!p->engine)
    return csound->InitError(csound,
                             "fluidgrain: unsupported sample rate or storage");
  double *copy = fg_source(p->engine);
  for (uint32_t i = 0; i < source->flen; ++i)
    copy[i] = source->ftable[i];
  /* The pinned WASI SDK (wasm-bin beta21) has the legacy void tabinit.
   * Keep native failure propagation; validate the resulting array on both. */
#if defined(__wasi__)
  tabinit(csound, p->stats, FG_STAT_COUNT, p->h.insdshead);
#else
  if (tabinit(csound, p->stats, FG_STAT_COUNT, p->h.insdshead) != OK)
    return csound->InitError(csound,
                             "fluidgrain: could not initialize stats[20]");
#endif
  if (!vector(p->stats, FG_STAT_COUNT))
    return csound->InitError(csound,
                             "fluidgrain: could not initialize stats[20]");
  double controls[FG_CONTROL_COUNT];
  for (int i = 0; i < FG_CONTROL_COUNT; ++i)
    controls[i] = p->control->data[i];
  fg_controls(p->engine, controls);
  if (config.value[FG_CONFIG_BACKEND] == 1) {
    FGRegistry *registry = csound->QueryGlobalVariable(csound, FG_REGISTRY_NAME);
    for (unsigned i = 0; i < 16; ++i) {
      FGProvider *provider = registry->providers[i];
      if (fg_provider_key(provider) == (uint64_t)config.value[FG_CONFIG_INSTANCE_ID]) {
        if (!fg_provider_attach(provider, p->engine))
          return csound->InitError(csound, "fluidgrain: external provider configuration mismatch");
        p->provider = provider;
        break;
      }
    }
  }
  /* Publish readiness after successful preparation, even when a host keeps
   * this opcode behind a k-rate gate before its first performance call. */
  double stats[FG_STAT_COUNT];
  fg_stats(p->engine, stats);
  for (int i = 0; i < FG_STAT_COUNT; ++i)
    p->stats->data[i] = (MYFLT)stats[i];
  return OK;
}
static int32_t perf(CSOUND *csound, FLUIDGRAIN *p) {
  uint32_t n = CS_KSMPS, start = p->h.insdshead->ksmps_offset;
  uint32_t end = n - p->h.insdshead->ksmps_no_end;
  memset(p->left, 0, n * sizeof(MYFLT));
  memset(p->right, 0, n * sizeof(MYFLT));
  if (!p->engine || !vector(p->control, FG_CONTROL_COUNT) ||
      !vector(p->stats, FG_STAT_COUNT))
    return csound->PerfError(
        csound, &p->h, "fluidgrain: array shape changed during performance");
  double controls[FG_CONTROL_COUNT];
  for (int i = 0; i < FG_CONTROL_COUNT; ++i)
    controls[i] = p->control->data[i];
  fg_controls(p->engine, controls);
  for (uint32_t i = start; i < end; ++i) {
    double left, right;
    fg_sample(p->engine, &left, &right);
    p->left[i] = (MYFLT)left;
    p->right[i] = (MYFLT)right;
  }
  double stats[FG_STAT_COUNT];
  fg_stats(p->engine, stats);
  for (int i = 0; i < FG_STAT_COUNT; ++i)
    p->stats->data[i] = (MYFLT)stats[i];
  return OK;
}
/* An explicit optional opcode keeps the original public signature unchanged.
 * The host owns a dedicated fixed-size table; source and snapshot cannot alias. */
static int32_t init_visual(CSOUND *csound, FLUIDGRAIN *p) {
  p->view = NULL;
  if (!isfinite(*p->view_table) || *p->view_table < 1 ||
      *p->view_table > INT32_MAX || *p->view_table != floor(*p->view_table) ||
      *p->view_table == *p->table)
    return csound->InitError(csound, "fluidgrain_visual: invalid snapshot table");
  FUNC *view = csound->FTFind(csound, p->view_table);
  if (!view || !view->ftable || view->flen != FG_VIEW_SIZE)
    return csound->InitError(csound, "fluidgrain_visual: wrong snapshot table size");
  csound->AuxAlloc(csound, FG_VIEW_SIZE * sizeof(double), &p->view_memory);
  if (!p->view_memory.auxp)
    return csound->InitError(csound, "fluidgrain_visual: snapshot allocation failed");
  int32_t result = init(csound, p);
  if (result != OK) return result;
  p->view_copy = p->view_memory.auxp;
  p->view = view;
  fg_view(p->engine, p->view_copy, FG_VIEW_SIZE);
  for (unsigned i = 0; i < FG_VIEW_SIZE; ++i)
    view->ftable[i] = (MYFLT)p->view_copy[i];
  return OK;
}
static int32_t perf_visual(CSOUND *csound, FLUIDGRAIN *p) {
  int32_t result = perf(csound, p);
  if (result != OK || *p->view_request != 1) return result;
  FUNC *view = csound->FTFind(csound, p->view_table);
  if (!view || !view->ftable || view->flen != FG_VIEW_SIZE)
    return csound->PerfError(csound, &p->h, "fluidgrain_visual: snapshot table changed");
  fg_view(p->engine, p->view_copy, FG_VIEW_SIZE);
  for (unsigned i = 0; i < FG_VIEW_SIZE; ++i)
    view->ftable[i] = (MYFLT)p->view_copy[i];
  return OK;
}
static int32_t deinit(CSOUND *csound, FLUIDGRAIN *p) {
  (void)csound;
  if (p->provider) { fg_provider_release(p->provider); p->provider = NULL; }
  p->engine = NULL;
  return OK;
}
#if defined(__wasi__)
/* Private host binding: address is local to this WASM instance, never an engine
 * ID or worker message. Global storage survives opcode deinit ordering and is
 * reclaimed by Csound reset/destroy. Each browser render uses a fresh host. */
typedef struct {
  OPDS h;
  MYFLT *address;
  ARRAYDAT *config;
  MYFLT *mode;
  FGProvider *provider;
  FGBrowserMailbox *mailbox;
} FGBROWSER;
static int32_t browser_init(CSOUND *csound, FGBROWSER *p) {
  if (!vector(p->config, FG_CONFIG_COUNT) ||
      (*p->mode != 0 && *p->mode != 1))
    return csound->InitError(csound, "fluidgrain_browser: invalid setup");
  double values[FG_CONFIG_COUNT];
  for (unsigned i = 0; i < FG_CONFIG_COUNT; ++i) values[i] = p->config->data[i];
  FGConfig config;
  if (fg_config_parse(&config, values, FG_CONFIG_COUNT) ||
      config.value[FG_CONFIG_BACKEND] != 1)
    return csound->InitError(csound, "fluidgrain_browser: external config required");
  char name[80];
  snprintf(name, sizeof(name), "::fluidgrain.browser.v1.%u.%u",
    (unsigned)config.value[FG_CONFIG_INSTANCE_ID], (unsigned)*p->mode);
  size_t bytes = fg_provider_size(&config);
  if (!bytes || csound->QueryGlobalVariable(csound, name) ||
      csound->CreateGlobalVariable(csound, name, bytes + sizeof(FGBrowserMailbox)))
    return csound->InitError(csound, "fluidgrain_browser: fresh host storage required");
  void *memory = csound->QueryGlobalVariable(csound, name);
  p->provider = fg_provider_init(memory, bytes, &config, CS_ESR, 1);
  if (!p->provider || (*p->mode == 0 && !register_provider(csound, p->provider)))
    return csound->InitError(csound, "fluidgrain_browser: provider preparation failed");
  p->mailbox = (FGBrowserMailbox *)((unsigned char *)memory + bytes);
  fg_browser_init(p->mailbox, (unsigned)*p->mode,
                  (unsigned)config.value[FG_CONFIG_GRID_SIZE]);
  *p->address = (MYFLT)(uintptr_t)p->mailbox;
  return OK;
}
static int32_t browser_perf(CSOUND *csound, FGBROWSER *p) {
  (void)csound;
  fg_browser_service(p->mailbox, p->provider);
  *p->address = (MYFLT)(uintptr_t)p->mailbox;
  return OK;
}
#endif
#include "fluidgrain_plan_opcode.inc"
#if defined(FG_HAVE_CUDA)
#include "fluidgrain_gpu_opcode.inc"
#endif
#if defined(FG_HAVE_NATIVE_WORKER)
#include "fluidgrain_buffered_opcode.inc"
#endif
static OENTRY localops[] = {
#if defined(FG_HAVE_NATIVE_WORKER)
  {"naviergrain_buffered", sizeof(FGBUFFEROP), 0, "aak[]kkkk", "ik[]kk",
    (SUBR)buffered_init, (SUBR)buffered_perf, (SUBR)buffered_deinit, NULL, 0},
#endif
#if defined(FG_HAVE_CUDA)
  {"naviergrain_gpu", sizeof(FGGPUOP), 0, "aak[]k", "iii[]k[]ik",
    (SUBR)gpu_opcode_init, (SUBR)gpu_opcode_perf, (SUBR)gpu_opcode_deinit, NULL, 0},
#endif
  {"naviergrain_plan", sizeof(FGPLANOP), 0, "kk[]", "iii[]k[]o",
    (SUBR)plan_opcode_init, (SUBR)plan_opcode_perf, (SUBR)plan_opcode_deinit, NULL, 0},
#if defined(__wasi__)
  {"naviergrain_browser", sizeof(FGBROWSER), 0, "k", "i[]i",
    (SUBR)browser_init, (SUBR)browser_perf, NULL, NULL, 0},
#endif
  {"naviergrain", sizeof(FLUIDGRAIN), 0, "aak[]", "iii[]k[]",
    (SUBR)init, (SUBR)perf, (SUBR)deinit, NULL, 0},
  {"naviergrain_visual", sizeof(FLUIDGRAIN), 0, "aak[]", "iii[]k[]ik",
    (SUBR)init_visual, (SUBR)perf_visual, (SUBR)deinit, NULL, 0}
,
/* Legacy score compatibility. */
#if defined(FG_HAVE_NATIVE_WORKER)
  {"fluidgrain_buffered", sizeof(FGBUFFEROP), 0, "aak[]kkkk", "ik[]kk",
    (SUBR)buffered_init, (SUBR)buffered_perf, (SUBR)buffered_deinit, NULL, 0},
#endif
#if defined(FG_HAVE_CUDA)
  {"fluidgrain_gpu", sizeof(FGGPUOP), 0, "aak[]k", "iii[]k[]ik",
    (SUBR)gpu_opcode_init, (SUBR)gpu_opcode_perf, (SUBR)gpu_opcode_deinit, NULL, 0},
#endif
  {"fluidgrain_plan", sizeof(FGPLANOP), 0, "kk[]", "iii[]k[]o",
    (SUBR)plan_opcode_init, (SUBR)plan_opcode_perf, (SUBR)plan_opcode_deinit, NULL, 0},
#if defined(__wasi__)
  {"fluidgrain_browser", sizeof(FGBROWSER), 0, "k", "i[]i",
    (SUBR)browser_init, (SUBR)browser_perf, NULL, NULL, 0},
#endif
  {"fluidgrain", sizeof(FLUIDGRAIN), 0, "aak[]", "iii[]k[]",
    (SUBR)init, (SUBR)perf, (SUBR)deinit, NULL, 0},
  {"fluidgrain_visual", sizeof(FLUIDGRAIN), 0, "aak[]", "iii[]k[]ik",
    (SUBR)init_visual, (SUBR)perf_visual, (SUBR)deinit, NULL, 0}
};
LINKAGE
