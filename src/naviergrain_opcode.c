#include "naviergrain_core.h"
#include <csdl.h>
#include <arrays.h>
#include <math.h>
#include <string.h>
#include "naviergrain_provider.h"
#include "naviergrain_browser.h"
#include "naviergrain_pack.h"
#define NG_REGISTRY_NAME "::naviergrain.providers.v1"
typedef struct { NGProvider *providers[16]; } NGRegistry;
static int register_provider(void *host, NGProvider *provider) {
  CSOUND *csound = host;
  if (!csound || !provider || ng_provider_claimed(provider)) return 0;
  NGRegistry *registry = csound->QueryGlobalVariable(csound, NG_REGISTRY_NAME);
  if (!registry) {
    if (csound->CreateGlobalVariable(csound, NG_REGISTRY_NAME, sizeof(NGRegistry)))
      return 0;
    registry = csound->QueryGlobalVariable(csound, NG_REGISTRY_NAME);
    if (!registry) return 0;
    memset(registry, 0, sizeof(*registry));
  }
  for (unsigned i = 0; i < 16; ++i)
    if (ng_provider_key(registry->providers[i]) == ng_provider_key(provider))
      return 0;
  for (unsigned i = 0; i < 16; ++i)
    if (!registry->providers[i]) { registry->providers[i] = provider; return 1; }
  return 0;
}
static int unregister_provider(void *host, NGProvider *provider) {
  CSOUND *csound = host;
  if (!csound || !provider || ng_provider_claimed(provider)) return 0;
  NGRegistry *registry = csound->QueryGlobalVariable(csound, NG_REGISTRY_NAME);
  if (!registry) return 0;
  for (unsigned i = 0; i < 16; ++i)
    if (registry->providers[i] == provider) {
      registry->providers[i] = NULL; return 1;
    }
  return 0;
}
PUBLIC const NGNativeAPI *naviergrain_native_api(void) {
  static const NGNativeAPI api = {1, ng_provider_size, ng_provider_init,
    register_provider, unregister_provider, ng_provider_work,
    ng_provider_resume, ng_provider_stats};
  return &api;
}

typedef struct {
  OPDS h;
  MYFLT *left, *right;
  ARRAYDAT *stats;
  MYFLT *table, *source_sr;
  ARRAYDAT *config, *control;
  MYFLT *view_table, *view_request; /* Only naviergrain_visual binds these. */
  AUXCH memory;
  FUNC *view;
  AUXCH view_memory;
  double *view_copy;
  NGEngine *engine;
  NGProvider *provider;
} NAVIERGRAIN;

static int vector(const ARRAYDAT *a, int n) {
  return a && a->dimensions == 1 && a->sizes && a->sizes[0] == n && a->data &&
         a->arrayMemberSize == (int)sizeof(MYFLT);
}
static int32_t init(CSOUND *csound, NAVIERGRAIN *p) {
  /* Reinit must not attach a second engine to an existing live provider. */
  if (p->provider)
    return csound->InitError(csound, "naviergrain: external reinit requires stopped host reprepare");
  p->engine = NULL;
  if (!vector(p->config, NG_CONFIG_COUNT) ||
      !vector(p->control, NG_CONTROL_COUNT))
    return csound->InitError(
        csound, "naviergrain: expected 1D config[13] and control[24]");
  double values[NG_CONFIG_COUNT];
  for (int i = 0; i < NG_CONFIG_COUNT; ++i)
    values[i] = p->config->data[i];
  NGConfig config;
  const char *error = ng_config_parse(&config, values, NG_CONFIG_COUNT);
  if (error)
    return csound->InitError(csound, "naviergrain: %s", error);
  if (config.value[NG_CONFIG_BACKEND] == 1) {
    NGRegistry *registry = csound->QueryGlobalVariable(csound, NG_REGISTRY_NAME);
    NGProvider *provider = NULL;
    if (registry)
      for (unsigned i = 0; i < 16; ++i)
        if (ng_provider_key(registry->providers[i]) ==
            (uint64_t)config.value[NG_CONFIG_INSTANCE_ID])
          provider = registry->providers[i];
    if (!provider || ng_provider_claimed(provider))
      return csound->InitError(csound, "naviergrain: external backend needs an unclaimed prepared provider");
  }
  if (!isfinite(*p->source_sr) || *p->source_sr <= 0)
    return csound->InitError(
        csound, "naviergrain: source sample rate must be finite and positive");
  if (!isfinite(*p->table) || *p->table < 1 || *p->table > INT32_MAX ||
      *p->table != floor(*p->table))
    return csound->InitError(
        csound, "naviergrain: source table must be a positive integer");
  FUNC *source = csound->FTFind(csound, p->table);
  if (!source || !source->ftable || !source->flen)
    return csound->InitError(csound,
                             "naviergrain: source table is missing or empty");
  /* GEN01 records channels; synthetic GEN tables have 0 or 1 channels. */
  if (source->nchanls < 0 || source->nchanls > 1)
    return csound->InitError(csound, "naviergrain: source table must be mono");
  for (uint32_t i = 0; i < source->flen; ++i)
    if (!isfinite(source->ftable[i]))
      return csound->InitError(
          csound, "naviergrain: source contains a non-finite sample");
  size_t bytes = ng_memory_size(&config, source->flen, *p->source_sr, CS_ESR);
  if (!bytes)
    return csound->InitError(csound, "naviergrain: invalid allocation size");
  /* Offline preview: AUXCH belongs to the opcode/engine; no custom teardown
   * required. */
  csound->AuxAlloc(csound, bytes, &p->memory);
  if (!p->memory.auxp)
    return csound->InitError(csound, "naviergrain: allocation failed");
  p->engine = ng_init(p->memory.auxp, bytes, &config, source->flen,
                      *p->source_sr, CS_ESR);
  if (!p->engine)
    return csound->InitError(csound,
                             "naviergrain: unsupported sample rate or storage");
  double *copy = ng_source(p->engine);
  for (uint32_t i = 0; i < source->flen; ++i)
    copy[i] = source->ftable[i];
  /* The pinned WASI SDK (wasm-bin beta21) has the legacy void tabinit.
   * Keep native failure propagation; validate the resulting array on both. */
#if defined(__wasi__)
  tabinit(csound, p->stats, NG_STAT_COUNT, p->h.insdshead);
#else
  if (tabinit(csound, p->stats, NG_STAT_COUNT, p->h.insdshead) != OK)
    return csound->InitError(csound,
                             "naviergrain: could not initialize stats[20]");
#endif
  if (!vector(p->stats, NG_STAT_COUNT))
    return csound->InitError(csound,
                             "naviergrain: could not initialize stats[20]");
  double controls[NG_CONTROL_COUNT];
  for (int i = 0; i < NG_CONTROL_COUNT; ++i)
    controls[i] = p->control->data[i];
  ng_controls(p->engine, controls);
  if (config.value[NG_CONFIG_BACKEND] == 1) {
    NGRegistry *registry = csound->QueryGlobalVariable(csound, NG_REGISTRY_NAME);
    for (unsigned i = 0; i < 16; ++i) {
      NGProvider *provider = registry->providers[i];
      if (ng_provider_key(provider) == (uint64_t)config.value[NG_CONFIG_INSTANCE_ID]) {
        if (!ng_provider_attach(provider, p->engine))
          return csound->InitError(csound, "naviergrain: external provider configuration mismatch");
        p->provider = provider;
        break;
      }
    }
  }
  /* Publish readiness after successful preparation, even when a host keeps
   * this opcode behind a k-rate gate before its first performance call. */
  double stats[NG_STAT_COUNT];
  ng_stats(p->engine, stats);
  for (int i = 0; i < NG_STAT_COUNT; ++i)
    p->stats->data[i] = (MYFLT)stats[i];
  return OK;
}
static int32_t perf(CSOUND *csound, NAVIERGRAIN *p) {
  uint32_t n = CS_KSMPS, start = p->h.insdshead->ksmps_offset;
  uint32_t end = n - p->h.insdshead->ksmps_no_end;
  memset(p->left, 0, n * sizeof(MYFLT));
  memset(p->right, 0, n * sizeof(MYFLT));
  if (!p->engine || !vector(p->control, NG_CONTROL_COUNT) ||
      !vector(p->stats, NG_STAT_COUNT))
    return csound->PerfError(
        csound, &p->h, "naviergrain: array shape changed during performance");
  double controls[NG_CONTROL_COUNT];
  for (int i = 0; i < NG_CONTROL_COUNT; ++i)
    controls[i] = p->control->data[i];
  ng_controls(p->engine, controls);
  for (uint32_t i = start; i < end; ++i) {
    double left, right;
    ng_sample(p->engine, &left, &right);
    p->left[i] = (MYFLT)left;
    p->right[i] = (MYFLT)right;
  }
  double stats[NG_STAT_COUNT];
  ng_stats(p->engine, stats);
  for (int i = 0; i < NG_STAT_COUNT; ++i)
    p->stats->data[i] = (MYFLT)stats[i];
  return OK;
}
/* An explicit optional opcode keeps the original public signature unchanged.
 * The host owns a dedicated fixed-size table; source and snapshot cannot alias. */
static int32_t init_visual(CSOUND *csound, NAVIERGRAIN *p) {
  p->view = NULL;
  if (!isfinite(*p->view_table) || *p->view_table < 1 ||
      *p->view_table > INT32_MAX || *p->view_table != floor(*p->view_table) ||
      *p->view_table == *p->table)
    return csound->InitError(csound, "naviergrain_visual: invalid snapshot table");
  FUNC *view = csound->FTFind(csound, p->view_table);
  if (!view || !view->ftable || view->flen != NG_VIEW_SIZE)
    return csound->InitError(csound, "naviergrain_visual: wrong snapshot table size");
  csound->AuxAlloc(csound, NG_VIEW_SIZE * sizeof(double), &p->view_memory);
  if (!p->view_memory.auxp)
    return csound->InitError(csound, "naviergrain_visual: snapshot allocation failed");
  int32_t result = init(csound, p);
  if (result != OK) return result;
  p->view_copy = p->view_memory.auxp;
  p->view = view;
  ng_view(p->engine, p->view_copy, NG_VIEW_SIZE);
  for (unsigned i = 0; i < NG_VIEW_SIZE; ++i)
    view->ftable[i] = (MYFLT)p->view_copy[i];
  return OK;
}
static int32_t perf_visual(CSOUND *csound, NAVIERGRAIN *p) {
  int32_t result = perf(csound, p);
  if (result != OK || *p->view_request != 1) return result;
  FUNC *view = csound->FTFind(csound, p->view_table);
  if (!view || !view->ftable || view->flen != NG_VIEW_SIZE)
    return csound->PerfError(csound, &p->h, "naviergrain_visual: snapshot table changed");
  ng_view(p->engine, p->view_copy, NG_VIEW_SIZE);
  for (unsigned i = 0; i < NG_VIEW_SIZE; ++i)
    view->ftable[i] = (MYFLT)p->view_copy[i];
  return OK;
}
static int32_t deinit(CSOUND *csound, NAVIERGRAIN *p) {
  (void)csound;
  if (p->provider) { ng_provider_release(p->provider); p->provider = NULL; }
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
  NGProvider *provider;
  NGBrowserMailbox *mailbox;
} NGBROWSER;
static int32_t browser_init(CSOUND *csound, NGBROWSER *p) {
  if (!vector(p->config, NG_CONFIG_COUNT) ||
      (*p->mode != 0 && *p->mode != 1))
    return csound->InitError(csound, "naviergrain_browser: invalid setup");
  double values[NG_CONFIG_COUNT];
  for (unsigned i = 0; i < NG_CONFIG_COUNT; ++i) values[i] = p->config->data[i];
  NGConfig config;
  if (ng_config_parse(&config, values, NG_CONFIG_COUNT) ||
      config.value[NG_CONFIG_BACKEND] != 1)
    return csound->InitError(csound, "naviergrain_browser: external config required");
  char name[80];
  snprintf(name, sizeof(name), "::naviergrain.browser.v1.%u.%u",
    (unsigned)config.value[NG_CONFIG_INSTANCE_ID], (unsigned)*p->mode);
  size_t bytes = ng_provider_size(&config);
  if (!bytes || csound->QueryGlobalVariable(csound, name) ||
      csound->CreateGlobalVariable(csound, name, bytes + sizeof(NGBrowserMailbox)))
    return csound->InitError(csound, "naviergrain_browser: fresh host storage required");
  void *memory = csound->QueryGlobalVariable(csound, name);
  p->provider = ng_provider_init(memory, bytes, &config, CS_ESR, 1);
  if (!p->provider || (*p->mode == 0 && !register_provider(csound, p->provider)))
    return csound->InitError(csound, "naviergrain_browser: provider preparation failed");
  p->mailbox = (NGBrowserMailbox *)((unsigned char *)memory + bytes);
  ng_browser_init(p->mailbox, (unsigned)*p->mode,
                  (unsigned)config.value[NG_CONFIG_GRID_SIZE]);
  *p->address = (MYFLT)(uintptr_t)p->mailbox;
  return OK;
}
static int32_t browser_perf(CSOUND *csound, NGBROWSER *p) {
  (void)csound;
  ng_browser_service(p->mailbox, p->provider);
  *p->address = (MYFLT)(uintptr_t)p->mailbox;
  return OK;
}
#endif
#include "naviergrain_plan_opcode.inc"
#if defined(NG_HAVE_CUDA)
#include "naviergrain_gpu_opcode.inc"
#endif
#if defined(NG_HAVE_NATIVE_WORKER)
#include "naviergrain_buffered_opcode.inc"
#endif
static OENTRY localops[] = {
#if defined(NG_HAVE_NATIVE_WORKER)
  {"naviergrain_buffered", sizeof(NGBUFFEROP), 0, "aak[]kkkk", "ik[]kk",
    (SUBR)buffered_init, (SUBR)buffered_perf, (SUBR)buffered_deinit, NULL, 0},
#endif
#if defined(NG_HAVE_CUDA)
  {"naviergrain_gpu", sizeof(NGGPUOP), 0, "aak[]k", "iii[]k[]ik",
    (SUBR)gpu_opcode_init, (SUBR)gpu_opcode_perf, (SUBR)gpu_opcode_deinit, NULL, 0},
#endif
  {"naviergrain_plan", sizeof(NGPLANOP), 0, "kk[]", "iii[]k[]o",
    (SUBR)plan_opcode_init, (SUBR)plan_opcode_perf, (SUBR)plan_opcode_deinit, NULL, 0},
#if defined(__wasi__)
  {"naviergrain_browser", sizeof(NGBROWSER), 0, "k", "i[]i",
    (SUBR)browser_init, (SUBR)browser_perf, NULL, NULL, 0},
#endif
  {"naviergrain", sizeof(NAVIERGRAIN), 0, "aak[]", "iii[]k[]",
    (SUBR)init, (SUBR)perf, (SUBR)deinit, NULL, 0},
  {"naviergrain_visual", sizeof(NAVIERGRAIN), 0, "aak[]", "iii[]k[]ik",
    (SUBR)init_visual, (SUBR)perf_visual, (SUBR)deinit, NULL, 0}
};
LINKAGE
