#ifndef UBEAT_BYTEBEAT_H
#define UBEAT_BYTEBEAT_H

#include <stdint.h>
#include <buxn/vm/vm.h>

#define BYTEBEAT_VECTOR 0xd0
#define BYTEBEAT_T 0xd2
#define BYTEBEAT_V 0xd4
#define BYTEBEAT_B 0xd6

enum {
	BYTEBEAT_SYNC_VECTOR = 1 << 0,
	BYTEBEAT_SYNC_T      = 1 << 1,
	BYTEBEAT_SYNC_V      = 1 << 2,
};

typedef struct {
	uint16_t vector;
	uint16_t t;
	uint16_t v;
	uint8_t b;

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

#endif
