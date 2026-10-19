// See dd_voice.h.
#include "dd_voice.h"

#include "include/dd_efm.h"
#include "include/dd_trx_family.h"
#include "include/dd_trx_md.h"

#define DDV_EFM 8   // engines 0..7 EFM, 8.. TRX RS CB CL (trx_family kinds 5..7)

struct ddv {
  int engine;
  union {
    dd_trx_family_voice fam;
    dd_efm_voice efm;
  } u;
};

static const char *const names[DDV_ENGINES] = {
    "EFM-BD", "EFM-SD", "EFM-XT", "EFM-CP", "EFM-RS", "EFM-CB", "EFM-HH", "EFM-CY",
    "TRX-RS", "TRX-CB", "TRX-CL"};
// website/script.js's labels
static const char *const labels[DDV_ENGINES][8] = {
    {"PTCH", "DEC", "RAMP", "RDEC", "MOD", "MFRQ", "MDEC", "MFB"},
    {"PTCH", "DEC", "NOIS", "NDEC", "MOD", "MFRQ", "MDEC", "HPF"},
    {"PTCH", "DEC", "RAMP", "RDEC", "MOD", "MFRQ", "MDEC", "CLIC"},
    {"PTCH", "DEC", "CLPS", "CDEC", "MOD", "MFRQ", "MDEC", "HPF"},
    {"PTCH", "DEC", "MOD", "HPF", "SNAR", "SPTC", "SDEC", "SMOD"},
    {"PTCH", "DEC", "SNAP", "FB", "MOD", "MFRQ", "MDEC", ""},
    {"PTCH", "DEC", "TREM", "TFRQ", "MOD", "MFRQ", "MDEC", "FB"},
    {"PTCH", "DEC", "FB", "HPF", "MOD", "MFRQ", "MDEC", ""},
    {"PTCH", "DEC", "DIST", "", "", "", "", ""},
    {"PTCH", "DEC", "ENH", "TONE", "BUMP", "", "", "DIST"},
    {"PTCH", "DEC", "DUAL", "ENH", "TUNE", "CLIC", "", ""}};

uint32_t ddv_size(void) { return sizeof(ddv); }

static int clampe(int e) { return e < 0 ? 0 : e >= DDV_ENGINES ? DDV_ENGINES - 1 : e; }

void ddv_init(ddv *v, int engine) {
  engine = clampe(engine);
  v->engine = engine;
  if (engine < DDV_EFM) dd_efm_init(&v->u.efm, (dd_efm_kind)engine);
  else dd_trx_family_init(&v->u.fam, (dd_trx_family_kind)(DD_TRXF_RS + engine - DDV_EFM));
}

void ddv_render(ddv *v, int engine, const uint8_t control[8], float level, int trig,
                float *out, uint32_t n) {
  dd_trx_params p;
  int32_t b[DD_BLOCK_SIZE];
  uint32_t i;
  engine = clampe(engine);
  if (engine != v->engine) ddv_init(v, engine);
  if (n > DD_BLOCK_SIZE) n = DD_BLOCK_SIZE;
  for (i = 0; i < 8; ++i) {
    uint32_t c = control[i] > 127 ? 127 : control[i];
    p.control[i] = (uint16_t)((c * 32767u + 63u) / 127u);
  }
  level = level < 0 ? 0 : level > 1 ? 1 : level;
  p.level = (uint16_t)(level * 32767.0f);
  if (engine < DDV_EFM) dd_efm_render(&v->u.efm, &p, trig, b, n);
  else dd_trx_family_render(&v->u.fam, &p, trig, b, n);
  for (i = 0; i < n; ++i) out[i] = b[i] * (1.0f / 2147483648.0f);
}

const char *ddv_name(int engine) { return names[clampe(engine)]; }
const char *ddv_label(int engine, int i) { return (i < 0 || i > 7) ? "" : labels[clampe(engine)][i]; }

int ddv_default(int engine, int i) {
  engine = clampe(engine);
  if (i < 0 || i > 7) return 0;
  return engine < DDV_EFM ? dd_efm_defaults_u7[engine][i] : dd_trx_defaults_u7[DD_TRXF_RS + engine - DDV_EFM][i];
}
