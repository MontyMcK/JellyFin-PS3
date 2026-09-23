#ifndef BENCH_PPU_H
#define BENCH_PPU_H

#include <ppu-types.h>
#include "spu_job.h"

void  ppu_step_aos(float *p, u32 n, float dt, float phase);
void  ppu_step_soa_scalar(float *const f[SPUB_NFIELD], u32 first, u32 n,
                          float dt, float phase);
void  ppu_step_soa_vmx(float *const f[SPUB_NFIELD], u32 first, u32 n,
                       float dt, float phase);
float ppu_checksum(float *const f[SPUB_NFIELD], u32 n);

#endif
