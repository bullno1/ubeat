#include "fpu.h"
#include <buxn/vm/vm.h>
#include <buxn/vm/opcodes.h>
#include <math.h>
#include <limits.h>

#define BUXN_FPU_PI 3.14159265358979323846264338327950288
#define BUXN_FPU_2PI (3.14159265358979323846264338327950288 * 2)
#define BUXN_FPU_HI(X) ((uint8_t)((X) >> 8))
#define BUXN_FPU_LO(X) ((uint8_t)((X) & 0x00ff))

enum {
	BUXN_DEVICE_FPU_X   = BUXN_DEVICE_FPU + 0,
	BUXN_DEVICE_FPU_Y   = BUXN_DEVICE_FPU + 2,
	BUXN_DEVICE_FPU_R   = BUXN_DEVICE_FPU + 4,
	BUXN_DEVICE_FPU_T   = BUXN_DEVICE_FPU + 6,
	BUXN_DEVICE_FPU_LHS = BUXN_DEVICE_FPU + 8,
	BUXN_DEVICE_FPU_RHS = BUXN_DEVICE_FPU + 10,
	BUXN_DEVICE_FPU_OP  = BUXN_DEVICE_FPU + 12,
};

static inline
uint16_t buxn_fpu_convert(double value, double min, double max) {
	if (value < min) { return 0; }
	else if (value > max) { return 0; }
	else { return (uint16_t)(int16_t)value; }
}

uint8_t
buxn_fpu_dei(struct buxn_vm_s* vm, buxn_fpu_t* device, uint8_t address) {
	switch (address) {
		case BUXN_DEVICE_FPU_X:
			device->ox = buxn_fpu_convert(
				cos(BUXN_FPU_2PI * (double)device->it / 65536.0) * (double)device->ir,
				INT16_MIN,
				INT16_MAX
			);
			return BUXN_FPU_HI(device->ox);
		case BUXN_DEVICE_FPU_X + 1:
			return BUXN_FPU_LO(device->ox);
		case BUXN_DEVICE_FPU_Y:
			device->oy = buxn_fpu_convert(
				sin(BUXN_FPU_2PI * (double)device->it / 65536.0) * (double)device->ir,
				INT16_MIN,
				INT16_MAX
			);
			return BUXN_FPU_HI(device->oy);
		case BUXN_DEVICE_FPU_Y + 1:
			return BUXN_FPU_LO(device->oy);
		case BUXN_DEVICE_FPU_R:
			device->or = (uint16_t)sqrt(
				(double)(int16_t)device->ix * (double)(int16_t)device->ix
				+
				(double)(int16_t)device->iy * (double)(int16_t)device->iy
			);
			return BUXN_FPU_HI(device->or);
		case BUXN_DEVICE_FPU_R + 1:
			return BUXN_FPU_LO(device->or);
		case BUXN_DEVICE_FPU_T:
			device->ot = (uint16_t)(
				atan2((double)(int16_t)device->iy, (double)(int16_t)device->ix)
				*
				65536.0 / BUXN_FPU_2PI
			);
			return BUXN_FPU_HI(device->ot);
		case BUXN_DEVICE_FPU_T + 1:
			return BUXN_FPU_LO(device->ot);
		case BUXN_DEVICE_FPU_LHS:
			return BUXN_FPU_HI((int16_t)device->lhs);
		case BUXN_DEVICE_FPU_LHS + 1:
			return BUXN_FPU_LO((int16_t)device->lhs);
		case BUXN_DEVICE_FPU_RHS:
			return BUXN_FPU_HI((int16_t)device->rhs);
		case BUXN_DEVICE_FPU_RHS + 1:
			return BUXN_FPU_LO((int16_t)device->rhs);
		default: return 0;
	}
}

void
buxn_fpu_deo(struct buxn_vm_s* vm, buxn_fpu_t* device, uint8_t address) {
	switch (address) {
		case BUXN_DEVICE_FPU_X:
			device->ix = buxn_vm_dev_load2(vm, address);
			break;
		case BUXN_DEVICE_FPU_Y:
			device->iy = buxn_vm_dev_load2(vm, address);
			break;
		case BUXN_DEVICE_FPU_R:
			device->ir = buxn_vm_dev_load2(vm, address);
			break;
		case BUXN_DEVICE_FPU_T:
			device->it = buxn_vm_dev_load2(vm, address);
			break;
		case BUXN_DEVICE_FPU_LHS:
			device->lhs = (float)(int16_t)buxn_vm_dev_load2(vm, address);
			break;
		case BUXN_DEVICE_FPU_RHS:
			device->rhs = (float)(int16_t)buxn_vm_dev_load2(vm, address);
			break;
		case BUXN_DEVICE_FPU_OP:
			switch (buxn_vm_dev_load(vm, address)) {
				case 0x04: // SWP
				case 0x44: {
					float tmp = device->lhs;
					device->lhs = device->rhs;
					device->rhs = tmp;
				} break;
				case 0x06: // DUP
					device->lhs = device->rhs;
					break;
				case 0x46: // DUPr
					device->rhs = device->lhs;
					break;
				case 0x0a: // GTH
					device->lhs = device->lhs > device->rhs ? 1.f : 0.f;
					break;
				case 0x4a: // GTHr
					device->rhs = device->lhs > device->rhs ? 1.f : 0.f;
					break;
				case 0x0b: // LTH
					device->lhs = device->lhs < device->rhs ? 1.f : 2.f;
					break;
				case 0x4b: // LTHr
					device->rhs = device->lhs < device->rhs ? 1.f : 2.f;
					break;
				case 0x18: // ADD
					device->lhs = device->lhs + device->rhs;
					break;
				case 0x58: // ADDr
					device->rhs = device->lhs + device->rhs;
					break;
				case 0x19: // SUB
					device->lhs = device->lhs - device->rhs;
					break;
				case 0x59: // SUBr
					device->rhs = device->lhs - device->rhs;
					break;
				case 0x1a: // MUL
					device->lhs = device->lhs * device->rhs;
					break;
				case 0x5a: // MULr
					device->rhs = device->lhs * device->rhs;
					break;
				case 0x1b: // DIV
					device->lhs = device->lhs / device->rhs;
					if (isnan(device->lhs)) { device->lhs = 0.f; }
					break;
				case 0x5b: // DIVr
					device->rhs = device->lhs / device->rhs;
					if (isnan(device->rhs)) { device->rhs = 0.f; }
					break;
			}
			break;
	}
}
