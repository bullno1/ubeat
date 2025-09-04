#include "asm.h"
#include <bresmon.h>
#include <barena.h>
#include <bhash.h>
#include <buxn/asm/asm.h>
#include <blog.h>
#include <string.h>

struct buxn_asm_ctx_s {
	rom_t* rom;
	barena_t* arena;
};

typedef BHASH_TABLE(char*, bresmon_watch_t*) watch_table_t;

static const char* entry_file = NULL;
static bresmon_t* monitor = NULL;
static barena_pool_t arena_pool = { 0 };
static watch_table_t watch_tables[2];
static watch_table_t* current_watch_table = &watch_tables[0];
static barena_t arenas[2];
static barena_t* current_arena = &arenas[0];
static int loaded_version = 0;
static int current_version = 0;

static bhash_hash_t
str_hash(const void* key, size_t size) {
	const char* str = *(const char**)key;
	return bhash_hash(str, strlen(str));
}

static bool
str_eq(const void* lhs, const void* rhs, size_t size) {
	return strcmp(*(char**)lhs, *(char**)rhs) == 0;
}

static char*
arena_strdup(barena_t* arena, const char* str) {
	size_t len = strlen(str);
	char* copy = barena_memalign(arena, len + 1, _Alignof(char));
	memcpy(copy, str, len);
	copy[len] = '\0';
	return copy;
}

static void
ubeat_file_changed(const char* filename, void* userdata) {
	BLOG_DEBUG("%s updated", (char*)userdata);  // userdata is the latest copy
	++current_version;
}

void
ubeat_asm_init(void) {
	barena_pool_init(&arena_pool, 1);
	barena_init(&arenas[0], &arena_pool);
	barena_init(&arenas[1], &arena_pool);

	monitor = bresmon_create(NULL);

	bhash_config_t config = bhash_config_default();
	config.eq = str_eq;
	config.hash = str_hash;
	config.removable = false;

	bhash_init(&watch_tables[0], config);
	bhash_init(&watch_tables[1], config);
}

void
ubeat_asm_set_entry_file(const char* filename) {
	++current_version;
	entry_file = filename;
}

bool
ubeat_asm_should_reload(void) {
	bresmon_check(monitor, false);

	return entry_file != NULL && loaded_version != current_version;
}

bool
ubeat_asm_reload(rom_t* rom) {
	if (entry_file == NULL) { return false; }

	buxn_asm_ctx_t basm = {
		.rom = rom,
		.arena = current_arena,
	};

	bool success = buxn_asm(&basm, entry_file);

	watch_table_t* previous_watch_table = current_watch_table == &watch_tables[0] ? &watch_tables[1] : &watch_tables[0];
	for (bhash_index_t i = 0; i < bhash_len(previous_watch_table); ++i) {
		char* filename = previous_watch_table->keys[i];
		// Previously watched file not found currently
		if (!bhash_is_valid(bhash_find(current_watch_table, filename))) {
			if (success) {  // If successfully compiled, forget about it
				bresmon_unwatch(previous_watch_table->values[i]);
				BLOG_DEBUG("Unwatching %s", filename);
			} else { // If not, continue watching
				char* filename_copy = arena_strdup(current_arena, filename);
				bresmon_watch_t* watch = previous_watch_table->values[i];
				bresmon_set_watch_callback(watch, ubeat_file_changed, filename_copy);
				bhash_put(
					current_watch_table,
					filename_copy,
					previous_watch_table->values[i]
				);
			}
		}
	}
	current_watch_table = previous_watch_table;
	current_arena = current_arena == &arenas[0] ? &arenas[1] : &arenas[0];
	barena_reset(current_arena);
	bhash_clear(current_watch_table);

	loaded_version = current_version;

	return success;
}

void
ubeat_asm_cleanup(void) {
	bhash_cleanup(&watch_tables[1]);
	bhash_cleanup(&watch_tables[0]);

	bresmon_destroy(monitor);

	barena_reset(&arenas[1]);
	barena_reset(&arenas[0]);
	barena_pool_cleanup(&arena_pool);
}

void*
buxn_asm_alloc(buxn_asm_ctx_t* ctx, size_t size, size_t alignment) {
	return barena_memalign(ctx->arena, size, alignment);
}

void
buxn_asm_report(buxn_asm_ctx_t* ctx, buxn_asm_report_type_t type, const buxn_asm_report_t* report) {
	blog_level_t level = BLOG_LEVEL_INFO;
	switch (type) {
		case BUXN_ASM_REPORT_ERROR: level = BLOG_LEVEL_ERROR; break;
		case BUXN_ASM_REPORT_WARNING: level = BLOG_LEVEL_WARN; break;
	}

	if (report->token == NULL) {
		blog_write(
			level,
			report->region->filename, report->region->range.start.line,
			"%s", report->message
		);
	} else {
		blog_write(
			level,
			report->region->filename, report->region->range.start.line,
			"%s (`%s`)", report->message, report->token
		);
	}

	if (report->related_message != NULL) {
		blog_write(
			BLOG_LEVEL_INFO,
			report->related_region->filename, report->related_region->range.start.line,
			"%s:", report->related_message
		);
	}
}

void
buxn_asm_put_rom(buxn_asm_ctx_t* ctx, uint16_t addr, uint8_t value) {
	uint16_t offset = addr - 256;
	ctx->rom->content[offset] = value;
	ctx->rom->size = offset + 1 > ctx->rom->size ? offset + 1 : ctx->rom->size;
}

void
buxn_asm_put_symbol(buxn_asm_ctx_t* ctx, uint16_t addr, const buxn_asm_sym_t* sym) {
}

buxn_asm_file_t*
buxn_asm_fopen(buxn_asm_ctx_t* ctx, const char* filename) {
	FILE* file = fopen(filename, "rb");
	if (file != NULL) {
		bhash_alloc_result_t result = bhash_alloc(current_watch_table, (char*){ (char*)filename });
		if (result.is_new) {
			char* name_copy = arena_strdup(current_arena, filename);
			current_watch_table->keys[result.index] = name_copy;

			// Copy watch from the previous table or create a new one
			watch_table_t* previous_watch_table = current_watch_table == &watch_tables[0] ? &watch_tables[1] : &watch_tables[0];
			bhash_index_t previous_index = bhash_find(previous_watch_table, (char*){ (char*)filename });
			if (bhash_is_valid(previous_index)) {
				bresmon_watch_t* watch = previous_watch_table->values[previous_index];
				bresmon_set_watch_callback(watch, ubeat_file_changed, name_copy);
				current_watch_table->values[result.index] = watch;
			} else {
				BLOG_DEBUG("Watching %s", filename);
				current_watch_table->values[result.index] = bresmon_watch(
					monitor, filename, ubeat_file_changed, name_copy
				);
			}
		}
	}

	return (void*)file;
}

void
buxn_asm_fclose(buxn_asm_ctx_t* ctx, buxn_asm_file_t* file) {
	fclose((void*)file);
}

int
buxn_asm_fgetc(buxn_asm_ctx_t* ctx, buxn_asm_file_t* file) {
	int result = fgetc((void*)file);
	if (result == EOF) {
		return BUXN_ASM_IO_EOF;
	} else if (result < 0) {
		return BUXN_ASM_IO_ERROR;
	} else {
		return result;
	}
}
