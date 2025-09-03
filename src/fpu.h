#ifndef BUXN_FPU_H
#define BUXN_FPU_H

// Based on: https://benbridle.com/projects/bedrock/specification/math-device.html

#include <stdint.h>

#define BUXN_DEVICE_FPU 0xe0

struct buxn_vm_s;

typedef struct {
	uint16_t ix, iy;
	uint16_t ir, it;

	uint16_t ox, oy;
	uint16_t or, ot;

	float lhs;
	float rhs;
} buxn_fpu_t;

uint8_t
buxn_fpu_dei(struct buxn_vm_s* vm, buxn_fpu_t* device, uint8_t address);

void
buxn_fpu_deo(struct buxn_vm_s* vm, buxn_fpu_t* device, uint8_t address);

#endif
