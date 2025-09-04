#ifndef UBEAT_ASM_H
#define UBEAT_ASM_H

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

typedef struct {
	uint16_t size;
	uint8_t content[UINT16_MAX + 1 - 256];
} rom_t;

void
ubeat_asm_init(void);

void
ubeat_asm_set_entry_file(const char* filename);

bool
ubeat_asm_should_reload(void);

bool
ubeat_asm_reload(rom_t* rom);

void
ubeat_asm_cleanup(void);

#endif
