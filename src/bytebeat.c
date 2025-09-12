#include "bytebeat.h"

uint8_t
bytebeat_dei(buxn_vm_t* vm, bytebeat_t* device, uint8_t address) {
	switch (address) {
		case BYTEBEAT_VECTOR:
			return (uint8_t)(device->vector >> 8);
		case BYTEBEAT_VECTOR + 1:
			return (uint8_t)(device->vector & 0xff);
		case BYTEBEAT_T:
			return (uint8_t)(device->t >> 8);
		case BYTEBEAT_T + 1:
			return (uint8_t)(device->t & 0xff);
		case BYTEBEAT_V:
			return (uint8_t)(device->v >> 8);
		case BYTEBEAT_V + 1:
			return (uint8_t)(device->v & 0xff);
		default:
			return vm->device[address];
	}
}

void
bytebeat_deo(buxn_vm_t* vm, bytebeat_t* device, uint8_t address) {
	switch (address) {
		case BYTEBEAT_VECTOR:
			device->vector = buxn_vm_dev_load2(vm, BYTEBEAT_VECTOR);
			device->sync_bits |= BYTEBEAT_SYNC_VECTOR;
			break;
		case BYTEBEAT_T:
			device->t = buxn_vm_dev_load2(vm, BYTEBEAT_T);
			device->sync_bits |= BYTEBEAT_SYNC_T;
			break;
		case BYTEBEAT_V:
			device->v = buxn_vm_dev_load2(vm, BYTEBEAT_V);
			device->sync_bits |= BYTEBEAT_SYNC_V;
			break;
	}
}
