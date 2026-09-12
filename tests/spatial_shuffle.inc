/* Included only by the diagnostic core, never by the installed plugin.
 * Permute mapping values, not particle/field state. Reusing the seed keeps
 * assignments stable while the ordered population is unchanged. Birth/death
 * changes can reassign live targets; existing pitch/pan smoothing still applies.
 */
static void shuffle_column(void *rows, size_t stride, size_t offset,
                           const uint32_t *indices, uint32_t count,
                           uint32_t seed) {
  for (uint32_t i = count; i > 1; --i) {
    uint32_t j = random_u32(&seed) % i;
    uint32_t a = indices ? indices[i - 1] : i - 1;
    uint32_t b = indices ? indices[j] : j;
    double *left = (double *)((char *)rows + a * stride + offset);
    double *right = (double *)((char *)rows + b * stride + offset);
    double value = *left;
    *left = *right;
    *right = value;
  }
}

static void shuffle_mappings(FGEngine *e) {
  const uint32_t seed = (uint32_t)e->config.value[FG_CONFIG_SEED];
  const size_t emitter_columns[] = {
      offsetof(FGParticle, x), offsetof(FGParticle, y),
      offsetof(FGParticle, omega), offsetof(FGParticle, strain)};
  memcpy(e->mapping_emitters, e->emitters,
         e->emitter_count * sizeof(FGEmitter));
  for (uint32_t i = 0; i < 4; ++i)
    shuffle_column(e->mapping_emitters, sizeof(FGEmitter), emitter_columns[i],
                   NULL, e->emitter_count,
                   seed ^ (UINT32_C(0x3c6ef372) + i * UINT32_C(0x9e3779b9)));

  uint32_t live = 0;
  for (uint32_t i = 0; i < e->capacity; ++i) {
    if (!e->voices[i].active)
      continue;
    e->voices[i].mapping = e->voices[i].particle;
    e->mapping_indices[live++] = i;
  }
  const size_t voice_columns[] = {offsetof(FGParticle, y),
                                  offsetof(FGParticle, omega)};
  for (uint32_t i = 0; i < 2; ++i)
    shuffle_column(e->voices, sizeof(FGVoice),
                   offsetof(FGVoice, mapping) + voice_columns[i],
                   e->mapping_indices, live,
                   seed ^ (UINT32_C(0xa54ff53a) + i * UINT32_C(0x9e3779b9)));
}
