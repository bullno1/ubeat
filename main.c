#include <sokol_app.h>
#include <sokol_gfx.h>
#include <sokol_glue.h>
#include <sokol_gl.h>
#include <sokol_audio.h>
#include <sokol_time.h>
#include <blog.h>
#include <bresmon.h>
#include <barena.h>
#include <buxn/asm/asm.h>
#include <buxn/vm/vm.h>
#include <stdlib.h>
#include <string.h>
#include "tribuf.h"

#define SAMPLING_RATE 8000
#define BYTEBEAT_VECTOR 0xd0
#define BYTEBEAT_T 0xd2
#define BYTEBEAT_V 0xd4
#define BYTEBEAT_B 0xd6

typedef struct {
	uint16_t size;
	uint8_t content[UINT16_MAX];
} rom_t;

struct buxn_asm_ctx_s {
	rom_t* rom;
	barena_t arena;
};

typedef struct {
	uint16_t vector;
	uint16_t t;
	uint16_t v;
	uint8_t b;
} bytebeat_t;

typedef struct {
	bytebeat_t bytebeat;
} devices_t;

enum {
	AUDIO_CMD_LOAD_ROM            = 1 << 0,
	AUDIO_CMD_SYNC_ZERO_PAGE      = 1 << 1,
	AUDIO_CMD_SYNC_BYTEBEAT       = 1 << 2,
};

typedef struct {
	int cmds;

	rom_t rom;
	uint8_t zero_page[UINT8_MAX];
	bytebeat_t bytebeat;
} audio_cmd_t;

static const char* input_file = NULL;
static bresmon_t* monitor = NULL;
static bresmon_watch_t* watch = NULL;
static barena_pool_t arena_pool = { 0 };

static audio_cmd_t audio_cmds[3] = { 0 };
static tribuf_t audio_cmd_buf;

static buxn_vm_t* main_thread_vm = NULL;
static devices_t main_thread_devices = {
	.bytebeat = { .v = 1 }
};
static buxn_vm_t* audio_thread_vm = NULL;
static devices_t audio_thread_devices = {
	.bytebeat = { .v = 1 }
};

static void
audio(float* buffer, int num_frames, int num_channels);

static void
reload_formula(const char* filename, void* userdata);

static void
slog(
	const char* tag,
	uint32_t log_level,
	uint32_t log_item_id,
	const char* message_or_null,
	uint32_t line_nr,
	const char* filename_or_null,
	void* user_data
);

static void
init(void) {
	stm_setup();
	sg_setup(&(sg_desc){
		.environment = sglue_environment(),
		.logger.func = slog,
	});
	sgl_setup(&(sgl_desc_t){
		.logger = {
			.func = slog,
		},
	});

	tribuf_init(&audio_cmd_buf, &audio_cmds, sizeof(audio_cmds[0]));
	barena_pool_init(&arena_pool, 1);
	monitor = bresmon_create(NULL);

	main_thread_vm = malloc(sizeof(buxn_vm_t) + BUXN_MEMORY_BANK_SIZE);
	main_thread_vm->config = (buxn_vm_config_t){
		.memory_size = BUXN_MEMORY_BANK_SIZE,
		.userdata = &main_thread_devices,
	};
	buxn_vm_reset(main_thread_vm, BUXN_VM_RESET_ALL);

	audio_thread_vm = malloc(sizeof(buxn_vm_t) + BUXN_MEMORY_BANK_SIZE);
	audio_thread_vm->config = (buxn_vm_config_t){
		.memory_size = BUXN_MEMORY_BANK_SIZE,
		.userdata = &audio_thread_devices,
	};
	buxn_vm_reset(audio_thread_vm, BUXN_VM_RESET_ALL);

	if (input_file != NULL) {
		watch = bresmon_watch(monitor, input_file, reload_formula, NULL);

		reload_formula(input_file, NULL);
	}

	saudio_setup(&(saudio_desc){
		.sample_rate = SAMPLING_RATE,
		.num_channels = 1,
		.stream_cb = audio,
		.logger = {
			.func = slog,
		},
	});
}

static void
cleanup(void) {
	saudio_shutdown();

	free(audio_thread_vm);
	free(main_thread_vm);
	bresmon_destroy(monitor);
	barena_pool_cleanup(&arena_pool);

	sgl_shutdown();
	sg_shutdown();
}

static void
frame(void) {
	bresmon_check(monitor, false);
	tribuf_try_swap(&audio_cmd_buf);

	sg_begin_pass(&(sg_pass){
		.swapchain = sglue_swapchain(),
		.action.colors[0] = {
			.load_action = SG_LOADACTION_CLEAR,
		},
	});
	{
		sgl_defaults();
		sgl_viewport(0, 0, sapp_width(), sapp_height(), true);
		sgl_ortho(0.f, sapp_widthf(), sapp_heightf(), 0.f, -1.f, 1.f);

		bytebeat_t* bytebeat = &main_thread_devices.bytebeat;

		sgl_begin_points();
		sgl_point_size(2.f);
		sgl_c4b(0, 0, 255, 255);
		float width = sapp_widthf();
		float height = sapp_heightf();
		for (float i = 0.f; i < SAMPLING_RATE; i += 1.f) {
			bytebeat->t += bytebeat->v;
			buxn_vm_execute(main_thread_vm, bytebeat->vector);
			sgl_v2f((float)i / (float)SAMPLING_RATE * width, height - height * (float)bytebeat->b / 255.f);
		}

		sgl_end();

		sgl_draw();
	}
	sg_end_pass();
	sg_commit();
}

static void
audio(float* buffer, int num_frames, int num_channels) {
	// Process commands
	audio_cmd_t* cmd = tribuf_begin_recv(&audio_cmd_buf);
	if (cmd != NULL) {
		if (cmd->cmds & AUDIO_CMD_LOAD_ROM) {
			buxn_vm_reset(audio_thread_vm, BUXN_VM_RESET_SOFT);
			memcpy(
				audio_thread_vm->memory + BUXN_RESET_VECTOR,
				cmd->rom.content,
				cmd->rom.size
			);
		}

		if (cmd->cmds & AUDIO_CMD_SYNC_ZERO_PAGE) {
			memcpy(
				audio_thread_vm->memory,
				cmd->zero_page,
				sizeof(cmd->zero_page)
			);
		}

		if (cmd->cmds & AUDIO_CMD_SYNC_BYTEBEAT) {
			audio_thread_devices.bytebeat = cmd->bytebeat;
		}

		cmd->cmds = 0;
		tribuf_end_recv(&audio_cmd_buf);
	}

	// Render audio
	bytebeat_t* bytebeat = &audio_thread_devices.bytebeat;
	for (int i = 0; i < num_frames; ++i, bytebeat->t += bytebeat->v) {
		buxn_vm_execute(audio_thread_vm, bytebeat->vector);
		buffer[i] = (float)bytebeat->b / 255.f * 2.f - 1.f;
	}
}

static void
reload_formula(const char* filename, void* userdata) {
	BLOG_INFO("Compiling %s", filename);

	audio_cmd_t* cmd = tribuf_begin_send(&audio_cmd_buf);
	buxn_asm_ctx_t basm = {
		.rom = &cmd->rom,
	};
	barena_init(&basm.arena, &arena_pool);
	bool success = buxn_asm(&basm, filename);
	barena_reset(&basm.arena);
	if (!success) { return; }

	BLOG_INFO("Executing %s (%d bytes)", filename, cmd->rom.size);
	buxn_vm_reset(main_thread_vm, BUXN_VM_RESET_SOFT);
	memcpy(
		main_thread_vm->memory + BUXN_RESET_VECTOR,
		basm.rom->content,
		basm.rom->size
	);
	buxn_vm_execute(main_thread_vm, BUXN_RESET_VECTOR);

	// Might as well sync everything
	cmd->cmds |=
		  AUDIO_CMD_LOAD_ROM
		| AUDIO_CMD_SYNC_ZERO_PAGE
		| AUDIO_CMD_SYNC_BYTEBEAT;
	memcpy(cmd->zero_page, main_thread_vm->memory, UINT8_MAX);
	cmd->bytebeat = main_thread_devices.bytebeat;
	tribuf_end_send(&audio_cmd_buf);

	if (main_thread_devices.bytebeat.vector == 0) {
		BLOG_WARN("Bytebeat vector is not set");
	}
}

void*
buxn_asm_alloc(buxn_asm_ctx_t* ctx, size_t size, size_t alignment) {
	return barena_memalign(&ctx->arena, size, alignment);
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
	// TODO: watch and reload on header change
	return (void*)fopen(filename, "rb");
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

static uint8_t
bytebeat_dei(buxn_vm_t* vm, uint8_t address) {
	devices_t* devices = vm->config.userdata;
	bytebeat_t* bytebeat = &devices->bytebeat;
	switch (address) {
		case BYTEBEAT_VECTOR:
			return (uint8_t)(bytebeat->vector >> 8);
		case BYTEBEAT_VECTOR + 1:
			return (uint8_t)(bytebeat->vector & 0xff);
		case BYTEBEAT_T:
			return (uint8_t)(bytebeat->t >> 8);
		case BYTEBEAT_T + 1:
			return (uint8_t)(bytebeat->t & 0xff);
		case BYTEBEAT_V:
			return (uint8_t)(bytebeat->v >> 8);
		case BYTEBEAT_V + 1:
			return (uint8_t)(bytebeat->v & 0xff);
		case BYTEBEAT_B:
			return bytebeat->b;
		default:
			return vm->device[address];
	}
}

static void
bytebeat_deo(buxn_vm_t* vm, uint8_t address) {
	devices_t* devices = vm->config.userdata;
	bytebeat_t* bytebeat = &devices->bytebeat;
	switch (address) {
		case BYTEBEAT_VECTOR:
			bytebeat->vector = buxn_vm_dev_load2(vm, BYTEBEAT_VECTOR);
			break;
		case BYTEBEAT_T:
			bytebeat->t = buxn_vm_dev_load2(vm, BYTEBEAT_T);
			break;
		case BYTEBEAT_V:
			bytebeat->v = buxn_vm_dev_load2(vm, BYTEBEAT_V);
			break;
		case BYTEBEAT_B:
			bytebeat->b = buxn_vm_dev_load(vm, BYTEBEAT_B);
			break;
	}
}

uint8_t
buxn_vm_dei(buxn_vm_t* vm, uint8_t address) {
	switch (buxn_device_id(address)) {
		case BYTEBEAT_VECTOR:
			return bytebeat_dei(vm, address);
		default:
			return vm->device[address];
	}
}

void
buxn_vm_deo(buxn_vm_t* vm, uint8_t address) {
	switch (buxn_device_id(address)) {
		case BYTEBEAT_VECTOR:
			bytebeat_deo(vm, address);
			break;
	}
}

static void
slog(
	const char* tag,
	uint32_t log_level,
	uint32_t log_item_id,
	const char* message_or_null,
	uint32_t line_nr,
	const char* filename_or_null,
	void* user_data
) {
	blog_level_t level = BLOG_LEVEL_INFO;
	switch (log_level) {
		case 0: level = BLOG_LEVEL_FATAL;
		case 1: level = BLOG_LEVEL_ERROR;
		case 2: level = BLOG_LEVEL_WARN;
		case 3: level = BLOG_LEVEL_INFO;
	}

	blog_write(level, filename_or_null, line_nr, "%s (%s:%d)", message_or_null, tag, log_item_id);
}

sapp_desc
sokol_main(int argc, char* argv[]) {
	if (argc >= 2) {
		input_file = argv[1];
	}

	blog_init(&(blog_options_t){
		.current_depth_in_project = 0,
		.current_filename = __FILE__,
	});
	static blog_file_logger_options_t log_options = {
		.with_colors = true,
	};
	log_options.file = stderr;
	blog_add_file_logger(BLOG_LEVEL_TRACE, &log_options);

	return (sapp_desc){
		.init_cb = init,
		.frame_cb = frame,
		.cleanup_cb = cleanup,
		.width = 640,
		.height = 480,
		.window_title = "ubeat",
		.icon.sokol_default = true,
		.logger.func = slog,
	};
}
