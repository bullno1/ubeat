#ifndef UBEAT_BYTEBEAT_H
#define UBEAT_BYTEBEAT_H

#include <stdint.h>
#include <buxn/vm/vm.h>
#include <buxn/vm/jit.h>

#define BYTEBEAT_VECTOR 0xd0
#define BYTEBEAT_T 0xd2
#define BYTEBEAT_V 0xd4
#define BYTEBEAT_OPTIONS 0xd6

enum {
	BYTEBEAT_SYNC_VECTOR = 1 << 0,
	BYTEBEAT_SYNC_T      = 1 << 1,
	BYTEBEAT_SYNC_V      = 1 << 2,
};

enum {
	BYTEBEAT_OPTS_SHOW_WAVEFORM  = 1 << 0,
	BYTEBEAT_OPTS_SHOW_FFT       = 1 << 1,
};

typedef struct {
	uint16_t vector;
	uint16_t t;
	uint16_t v;

	uint8_t sync_bits;
} bytebeat_t;

uint8_t
bytebeat_dei(buxn_vm_t* vm, bytebeat_t* device, uint8_t address);

void
bytebeat_deo(buxn_vm_t* vm, bytebeat_t* device, uint8_t address);

static inline void
bytebeat_init(bytebeat_t* device) {
	*device = (bytebeat_t){ .v = 1 };
}

static inline uint8_t
bytebeat_options(buxn_vm_t* vm) {
	return buxn_vm_dev_load(vm, BYTEBEAT_OPTIONS);
}

static inline uint8_t
bytebeat_render(
	buxn_vm_t* vm,
	buxn_jit_t* jit,
	bytebeat_t* device,
	uint16_t t
) {
	vm->wsp = 2;
	vm->ws[0] = t >> 8;
	vm->ws[1] = t & 0xff;
	device->t = t;
	buxn_jit_execute(jit, device->vector);
	vm->wsp = 0;
	return vm->ws[0];
}

#endif
