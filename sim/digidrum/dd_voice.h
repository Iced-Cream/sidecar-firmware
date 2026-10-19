// One DIGIDRUM voice: digidrum's Machinedrum-style machines (soejrd/digidrum,
// MIT, pinned in build.sh) behind one engine number, as their web demo
// (website/trx_web.c) maps them, curated to the DIGIDRUM machine's ENG list
// (rytm-mods 0021, 2026-10-09; its 11 zones are these numbers):
//   0..7 EFM BD SD XT CP RS CB HH CY   8..10 TRX RS CB CL
// Left out: TRX-B2 (trx_md.c's voice: not referenced, so not linked) and TRX
// SD CH OH CY (inside trx_family.c, which RS CB CL share: their code stays).
// Controls are 0..127 (the Rytm's knobs), out is float -1..1 at 48 kHz.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DDV_ENGINES 11
typedef struct ddv ddv;
uint32_t ddv_size(void);
void ddv_init(ddv *v, int engine);
// Renders n (<= 32) samples; trig starts a hit; level 0..1. Re-inits on an
// engine change.
void ddv_render(ddv *v, int engine, const uint8_t control[8], float level, int trig,
                float *out, uint32_t n);
// The engine's name ("EFM-BD") and control i's label ("PTCH"), "" if none.
const char *ddv_name(int engine);
const char *ddv_label(int engine, int i);
// Control i's default, 0..127 (digidrum's per-machine defaults).
int ddv_default(int engine, int i);

#ifdef __cplusplus
}
#endif
