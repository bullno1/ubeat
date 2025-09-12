// vim: set foldmethod=marker foldlevel=0:
#include <sokol_app.h>
#include <sokol_gfx.h>
#include <sokol_glue.h>
#include <sokol_gl.h>
#include <sokol_audio.h>
#include <sokol_time.h>
#ifdef __clang__
#	pragma clang diagnostic ignored "-Wnewline-eof"
#endif
#include <am_fft.h>
#include <blog.h>
#include <barg.h>
#include <barena.h>
#include <buxn/vm/vm.h>
#include <buxn/vm/jit.h>
#include <buxn/devices/system.h>
#include <buxn/devices/console.h>
#include <buxn/devices/mouse.h>
#include <buxn/devices/controller.h>
#include <buxn/devices/datetime.h>
#include <buxn/devices/screen.h>
#include <buxn/metadata.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tribuf.h"
#include "bytebeat.h"
#include "fpu.h"
#include "asm.h"

#define SAMPLING_RATE 8000
#define FRAME_TIME_US (1000000.0 / 60.0)

#ifndef FFT_SIZE
#	define FFT_SIZE 1024
#endif

typedef struct {
	uint64_t timestamp;
	uint16_t t;
	uint16_t v;
} audio_state_t;

typedef struct {
	sg_image gpu;
	sg_view view;
	uint32_t* cpu;
	size_t size;
} layer_texture_t;

typedef struct {
	buxn_console_t console;
	buxn_mouse_t mouse;
	buxn_controller_t controller;
	buxn_screen_t* screen;
	bytebeat_t bytebeat;
	buxn_fpu_t fpu;

	buxn_jit_t* jit;
	barena_pool_t arena_pool;
	barena_t arena;
} devices_t;

enum {
	AUDIO_CMD_LOAD_ROM            = 1 << 0,
	AUDIO_CMD_SYNC_ZERO_PAGE      = 1 << 1,
	AUDIO_CMD_SYNC_BYTEBEAT       = 1 << 2,
};

typedef struct {
	int cmds;

	rom_t rom;
	uint8_t zero_page[256];
	bytebeat_t bytebeat;
} audio_cmd_t;

static const char* input_file = NULL;

static audio_cmd_t audio_cmds[3] = { 0 };
static tribuf_t audio_cmd_buf;

static audio_state_t last_audio_state = { 0 };
static audio_state_t audio_states[3] = { 0 };
static tribuf_t audio_state_buf;

static buxn_vm_t* main_thread_vm = NULL;
static devices_t main_thread_devices = { 0 };
static buxn_vm_t* audio_thread_vm = NULL;
static devices_t audio_thread_devices = { 0 };

static am_fft_plan_1d_t* fft = NULL;
static am_fft_complex_t* fft_in = NULL;
static am_fft_complex_t* fft_out = NULL;

static uint64_t last_frame;
static double frame_time_accumulator;
static layer_texture_t background_texture = { 0 };
static layer_texture_t foreground_texture = { 0 };
static sg_sampler screen_sampler;
static sgl_pipeline screen_pipeline;

static void
audio(float* buffer, int num_frames, int num_channels);

static void
try_reload_formula(void);

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

// Program {{{

static void
init_layer_texture(
	layer_texture_t* texture,
	int width,
	int height,
	buxn_screen_info_t screen_info,
	const char* label
) {
	texture->cpu = realloc(texture->cpu, screen_info.target_mem_size);
	texture->size = screen_info.target_mem_size;
	memset(texture->cpu, 0, screen_info.target_mem_size);
	if (texture->gpu.id != SG_INVALID_ID) {
		sg_destroy_image(texture->gpu);
	}
	if (texture->view.id != SG_INVALID_ID) {
		sg_destroy_view(texture->view);
	}

	texture->gpu = sg_make_image(&(sg_image_desc){
		.type = SG_IMAGETYPE_2D,
		.width = width,
		.height = height,
		.usage = {
			.stream_update = true,
		},
		.label = label,
	});
	texture->view = sg_make_view(&(sg_view_desc){
		.texture = {
			.image = texture->gpu,
		},
	});
}

static void
cleanup_layer_texture(layer_texture_t* texture) {
	sg_destroy_view(texture->view);
	sg_destroy_image(texture->gpu);
	free(texture->cpu);
}

static void
init_vm(buxn_vm_t* vm, devices_t* devices) {
	vm->config = (buxn_vm_config_t){
		.memory_size = BUXN_MEMORY_BANK_SIZE,
		.userdata = devices,
	};
	buxn_vm_reset(vm, BUXN_VM_RESET_ALL);

	buxn_console_init(vm, &devices->console, 0, NULL);
	bytebeat_init(&devices->bytebeat);

	barena_pool_init(&devices->arena_pool, 1);
	barena_init(&devices->arena, &devices->arena_pool);
	devices->jit = buxn_jit_init(vm, (buxn_jit_alloc_ctx_t*)&devices->arena);
}

static void
cleanup_vm(buxn_vm_t* vm) {
	devices_t* devices = vm->config.userdata;

	buxn_jit_cleanup(devices->jit);
	barena_reset(&devices->arena);
	barena_pool_cleanup(&devices->arena_pool);

	free(vm);
}

static void
reset_jit(buxn_vm_t* vm) {
	devices_t* devices = vm->config.userdata;

	buxn_jit_cleanup(devices->jit);
	barena_reset(&devices->arena);
	devices->jit = buxn_jit_init(
		vm,
		(buxn_jit_alloc_ctx_t*)&devices->arena
	);
}

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

	main_thread_vm = malloc(sizeof(buxn_vm_t) + BUXN_MEMORY_BANK_SIZE);
	init_vm(main_thread_vm, &main_thread_devices);

	// Screen device for main thread VM
	int width = sapp_width();
	int height = sapp_height();
	buxn_screen_info_t screen_info = buxn_screen_info(width, height);
	main_thread_devices.screen = malloc(screen_info.screen_mem_size);
	memset(main_thread_devices.screen, 0, screen_info.screen_mem_size);
	buxn_screen_resize(main_thread_devices.screen, width, height);
	init_layer_texture(&background_texture, width, height, screen_info, "ubeat.screen.background");
	init_layer_texture(&foreground_texture, width, height, screen_info, "ubeat.screen.foreground");
	screen_sampler = sg_make_sampler(&(sg_sampler_desc){
		.min_filter = SG_FILTER_NEAREST,
		.mag_filter = SG_FILTER_NEAREST,
		.wrap_u = SG_WRAP_CLAMP_TO_EDGE,
		.wrap_v = SG_WRAP_CLAMP_TO_EDGE,
		.label = "ubeat.screen",
	});
	screen_pipeline = sgl_make_pipeline(&(sg_pipeline_desc){
		.colors[0] = {
			.blend = {
				.enabled = true,
				.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
				.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
				.op_rgb = SG_BLENDOP_ADD,
				.src_factor_alpha = SG_BLENDFACTOR_ONE,
				.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
				.op_alpha = SG_BLENDOP_ADD,
			},
		},
		.label = "ubeat.screen",
	});

	last_frame = stm_now();
	frame_time_accumulator = FRAME_TIME_US;  // Render once

	tribuf_init(&audio_cmd_buf, &audio_cmds, sizeof(audio_cmds[0]));
	tribuf_init(&audio_state_buf, &audio_states, sizeof(audio_states[0]));
	last_audio_state.timestamp = stm_now();
	last_audio_state.v = 1;

	audio_thread_vm = malloc(sizeof(buxn_vm_t) + BUXN_MEMORY_BANK_SIZE);
	init_vm(audio_thread_vm, &audio_thread_devices);

	ubeat_asm_init();
	ubeat_asm_set_entry_file(input_file);
	try_reload_formula();
	if (input_file == NULL) {
		BLOG_WARN("No entry file set. Please drag and drop a .tal file into the window");
	}

	saudio_setup(&(saudio_desc){
		.sample_rate = SAMPLING_RATE,
		.num_channels = 1,
		.stream_cb = audio,
		.logger = {
			.func = slog,
		},
	});

	fft = am_fft_plan_1d(AM_FFT_FORWARD, FFT_SIZE);
	fft_in = malloc(sizeof(am_fft_complex_t) * FFT_SIZE);
	fft_out = malloc(sizeof(am_fft_complex_t) * FFT_SIZE);
}

static void
cleanup(void) {
	sg_destroy_sampler(screen_sampler);
	sgl_destroy_pipeline(screen_pipeline);
	cleanup_layer_texture(&foreground_texture);
	cleanup_layer_texture(&background_texture);
	free(main_thread_devices.screen);

	free(fft_in);
	free(fft_out);
	am_fft_plan_1d_free(fft);

	saudio_shutdown();

	cleanup_vm(audio_thread_vm);
	cleanup_vm(main_thread_vm);
	ubeat_asm_cleanup();

	sgl_shutdown();
	sg_shutdown();
}

static void
try_reload_formula(void) {
	if (!ubeat_asm_should_reload()) { return; }

	BLOG_INFO("Compiling %s", input_file);

	rom_t tmp_rom = { 0 };
	if (!ubeat_asm_reload(&tmp_rom)) { return; }

	BLOG_INFO("Executing %s (%d bytes)", input_file, tmp_rom.size);
	buxn_vm_reset(main_thread_vm, BUXN_VM_RESET_SOFT);
	memcpy(
		main_thread_vm->memory + BUXN_RESET_VECTOR,
		tmp_rom.content,
		tmp_rom.size
	);
	bytebeat_t* bytebeat = &main_thread_devices.bytebeat;
	bytebeat->sync_bits = 0;
	buxn_vm_execute(main_thread_vm, BUXN_RESET_VECTOR);

	audio_cmd_t* cmd = tribuf_begin_send(&audio_cmd_buf);
	memcpy(cmd->rom.content, tmp_rom.content, tmp_rom.size);
	cmd->rom.size = tmp_rom.size;
	cmd->cmds |= AUDIO_CMD_LOAD_ROM | AUDIO_CMD_SYNC_ZERO_PAGE;
	memcpy(cmd->zero_page, main_thread_vm->memory, sizeof(cmd->zero_page));
	if (bytebeat->sync_bits != 0) {
		cmd->cmds |= AUDIO_CMD_SYNC_BYTEBEAT;
		cmd->bytebeat = main_thread_devices.bytebeat;
	}
	tribuf_end_send(&audio_cmd_buf);

	if (main_thread_devices.bytebeat.vector == 0) {
		BLOG_WARN("Bytebeat vector is not set");
	}

	reset_jit(main_thread_vm);
}

static float
lerp(float x, float from, float to) {
	return from * (1.f - x) + to * x;
}

static void
event(const sapp_event* event) {
	bool update_mouse = false;
	buxn_mouse_t* mouse = &main_thread_devices.mouse;
	buxn_controller_t* controller = &main_thread_devices.controller;

	switch (event->type) {
		case SAPP_EVENTTYPE_MOUSE_UP:
		case SAPP_EVENTTYPE_MOUSE_DOWN: {
			int button;
			switch (event->mouse_button) {
				case SAPP_MOUSEBUTTON_LEFT:
					button = 0;
					break;
				case SAPP_MOUSEBUTTON_RIGHT:
					button = 2;
					break;
				case SAPP_MOUSEBUTTON_MIDDLE:
					button = 1;
					break;
				default:
					button = -1;
					break;
			}
			if (button >= 0) {
				buxn_mouse_set_button(
					mouse,
					button,
					event->type == SAPP_EVENTTYPE_MOUSE_DOWN
				);
				update_mouse = true;
			}
		} break;
		case SAPP_EVENTTYPE_MOUSE_SCROLL:
			mouse->scroll_x = (int16_t)event->scroll_x;
			mouse->scroll_y = -(int16_t)event->scroll_y;
			update_mouse = true;
			break;
		case SAPP_EVENTTYPE_MOUSE_MOVE: {
			mouse->x = event->mouse_x;
			mouse->y = event->mouse_y;
			update_mouse = true;
		} break;
		case SAPP_EVENTTYPE_KEY_DOWN:
		case SAPP_EVENTTYPE_KEY_UP: {
			bool down = event->type == SAPP_EVENTTYPE_KEY_DOWN;
			int button = -1;
			char ch = 0;
			switch (event->key_code) {
				case SAPP_KEYCODE_LEFT_CONTROL:
				case SAPP_KEYCODE_RIGHT_CONTROL:
					button = BUXN_CONTROLLER_BTN_A;
					break;
				case SAPP_KEYCODE_LEFT_ALT:
				case SAPP_KEYCODE_RIGHT_ALT:
					button = BUXN_CONTROLLER_BTN_B;
					break;
				case SAPP_KEYCODE_LEFT_SHIFT:
				case SAPP_KEYCODE_RIGHT_SHIFT:
					button = BUXN_CONTROLLER_BTN_SELECT;
					break;
				case SAPP_KEYCODE_HOME:
					button = BUXN_CONTROLLER_BTN_START;
					break;
				case SAPP_KEYCODE_UP:
					button = BUXN_CONTROLLER_BTN_UP;
					break;
				case SAPP_KEYCODE_DOWN:
					button = BUXN_CONTROLLER_BTN_DOWN;
					break;
				case SAPP_KEYCODE_LEFT:
					button = BUXN_CONTROLLER_BTN_LEFT;
					break;
				case SAPP_KEYCODE_RIGHT:
					button = BUXN_CONTROLLER_BTN_RIGHT;
					break;
				case SAPP_KEYCODE_ENTER:
					ch = '\r';
					break;
				case SAPP_KEYCODE_ESCAPE:
					ch = 27;
					break;
				case SAPP_KEYCODE_BACKSPACE:
					ch = 8;
					break;
				case SAPP_KEYCODE_TAB:
					ch = '\t';
					break;
				case SAPP_KEYCODE_DELETE:
					ch = 127;
					break;
				default:
					break;
			}
			if (button >= 0) {
				buxn_controller_send_button(main_thread_vm, controller, 0, button, down);
			}
			if (ch > 0 && down) {
				buxn_controller_send_char(main_thread_vm, controller, ch);
			}
		} break;
		case SAPP_EVENTTYPE_CHAR: {
			uint32_t ch = event->char_code;
			if (ch <= 127) {
				// Sync the modifiers in case we missed their release due to
				// focus change
				buxn_controller_set_button(
					controller,
					0,
					BUXN_CONTROLLER_BTN_A,
					(event->modifiers & SAPP_MODIFIER_CTRL) != 0
				);
				buxn_controller_set_button(
					controller,
					0,
					BUXN_CONTROLLER_BTN_B,
					(event->modifiers & SAPP_MODIFIER_ALT) != 0
				);
				buxn_controller_set_button(
					controller,
					0,
					BUXN_CONTROLLER_BTN_SELECT,
					(event->modifiers & SAPP_MODIFIER_SHIFT) != 0
				);
				// Send the actual character
				buxn_controller_send_char(main_thread_vm, controller, ch);
			}
		} break;
		case SAPP_EVENTTYPE_FILES_DROPPED:
			if (sapp_get_num_dropped_files() > 0) {
				input_file = sapp_get_dropped_file_path(0);
				ubeat_asm_set_entry_file(input_file);
				try_reload_formula();
			}
			break;
		default: break;
	}

	if (update_mouse) {
		buxn_mouse_update(main_thread_vm);
		mouse->scroll_x = mouse->scroll_y = 0;
	}
}

static void
blit_layer_texture(layer_texture_t* texture, float width, float height) {
	sgl_texture(texture->view, screen_sampler);
	sgl_c1i(0xffffffff);
	sgl_begin_quads();
	{
		sgl_v2f_t2f(0.f, 0.f, 0.f, 0.f);
		sgl_v2f_t2f(width, 0.f, 1.f, 0.f);
		sgl_v2f_t2f(width, height, 1.f, 1.f);
		sgl_v2f_t2f(0.f, height, 0.f, 1.f);
	}
	sgl_end();
}

static void
frame(void) {
	bytebeat_t* bytebeat = &main_thread_devices.bytebeat;
	audio_cmd_t* cmd = NULL;

	if (bytebeat->sync_bits != 0) {
		cmd = cmd == NULL ? tribuf_begin_send(&audio_cmd_buf) : cmd;
		cmd->bytebeat = *bytebeat;
		cmd->cmds |= AUDIO_CMD_SYNC_BYTEBEAT;
		bytebeat->sync_bits = 0;
	}

	static uint8_t last_zero_page[256] = { 0 };
	if (memcmp(last_zero_page, main_thread_vm->memory, sizeof(last_zero_page))) {
		cmd = cmd == NULL ? tribuf_begin_send(&audio_cmd_buf) : cmd;
		memcpy(cmd->zero_page, main_thread_vm->memory, sizeof(cmd->zero_page));
		cmd->cmds |= AUDIO_CMD_SYNC_ZERO_PAGE;
		memcpy(last_zero_page, main_thread_vm->memory, sizeof(cmd->zero_page));
	}

	if (cmd != NULL) {
		tribuf_end_send(&audio_cmd_buf);
	}

	try_reload_formula();
	tribuf_try_swap(&audio_cmd_buf);

	audio_state_t* audio_state_ptr = tribuf_begin_recv(&audio_state_buf);
	if (audio_state_ptr != NULL) {
		last_audio_state = *audio_state_ptr;
		tribuf_end_recv(&audio_state_buf);
	}

	float width = sapp_widthf();
	float height = sapp_heightf();
	bool playing_forward = bytebeat->v < UINT16_MAX / 2;
	uint8_t bytebeat_opts = bytebeat_options(main_thread_vm);

	sgl_defaults();
	sgl_viewport(0, 0, sapp_width(), sapp_height(), true);
	sgl_ortho(0.f, sapp_widthf(), sapp_heightf(), 0.f, -1.f, 1.f);

	// Screen
	uint32_t palette[4];
	buxn_system_palette(main_thread_vm, palette);
	if (
		palette[0] != 0xff000000
		||
		palette[1] != 0xff000000
		||
		palette[2] != 0xff000000
		||
		palette[3] != 0xff000000
	) {
		if (
			(int)width != main_thread_devices.screen->width
			||
			(int)height != main_thread_devices.screen->height
		) {
			buxn_screen_info_t screen_info = buxn_screen_info(width, height);
			main_thread_devices.screen = realloc(main_thread_devices.screen, screen_info.screen_mem_size);
			buxn_screen_resize(main_thread_devices.screen, width, height);
			init_layer_texture(&background_texture, width, height, screen_info, "ubeat.screen.background");
			init_layer_texture(&foreground_texture, width, height, screen_info, "ubeat.screen.foreground");
		}

		uint64_t now = stm_now();
		double time_diff = stm_us(stm_diff(now, last_frame));
		last_frame = now;
		frame_time_accumulator += time_diff;

		bool should_redraw = frame_time_accumulator >= FRAME_TIME_US;
		while (frame_time_accumulator >= FRAME_TIME_US) {
			frame_time_accumulator -= FRAME_TIME_US;
			buxn_screen_update(main_thread_vm);
		}

		if (should_redraw) {
			if (buxn_screen_render(
				main_thread_devices.screen,
				BUXN_SCREEN_LAYER_BACKGROUND,
				palette,
				background_texture.cpu
			)) {
				sg_update_image(
					background_texture.gpu,
					&(sg_image_data) {
						.subimage[0][0] = {
							.ptr = background_texture.cpu,
							.size = background_texture.size,
						},
					}
				);
			}

			palette[0] = 0; // Foreground treats color0 as transparent
			if (buxn_screen_render(
				main_thread_devices.screen,
				BUXN_SCREEN_LAYER_FOREGROUND,
				palette,
				foreground_texture.cpu
			)) {
				sg_update_image(
					foreground_texture.gpu,
					&(sg_image_data) {
						.subimage[0][0] = {
							.ptr = foreground_texture.cpu,
							.size = foreground_texture.size,
						},
					}
				);
			}
		}

		sgl_enable_texture();
		sgl_push_pipeline();
		sgl_load_pipeline(screen_pipeline);
		blit_layer_texture(&background_texture, width, height);
		blit_layer_texture(&foreground_texture, width, height);
		sgl_pop_pipeline();
		sgl_disable_texture();
	}

	// Bytebeat visual
	if (bytebeat_opts & (BYTEBEAT_OPTS_SHOW_WAVEFORM | BYTEBEAT_OPTS_SHOW_FFT)) {
		sgl_begin_points();
		{
			sgl_point_size(2.f);

			if (playing_forward) {
				sgl_c4b(0, 0, 255, 255);
			} else {
				sgl_c4b(0, 255, 255, 255);
			}

			double time_diff_s = stm_sec(stm_now()) - stm_sec(last_audio_state.timestamp);
			uint16_t t = last_audio_state.t + (uint16_t)(time_diff_s * (double)SAMPLING_RATE) * (double)last_audio_state.v;
			uint16_t old_t = bytebeat->t;
			devices_t* devices = main_thread_vm->config.userdata;
			buxn_jit_t* jit = devices->jit;
			for (uint16_t i = 0; i < SAMPLING_RATE; ++i) {
				bytebeat->t = t + i;
				buxn_jit_execute(jit, bytebeat->vector);

				if (bytebeat_opts & BYTEBEAT_OPTS_SHOW_WAVEFORM) {
					sgl_v2f(
						(float)i / (float)SAMPLING_RATE * width,
						height - height * (float)bytebeat->b / 255.f
					);
				}

				if (i < (float)FFT_SIZE) {
					fft_in[(int)i][0] = (float)bytebeat->b / 255.f * 2.f - 1.f;
					fft_in[(int)i][1] = 0.f;
				}
			}
			bytebeat->t = old_t;
		}
		sgl_end();

		if (bytebeat_opts & BYTEBEAT_OPTS_SHOW_FFT) {
			am_fft_1d(fft, fft_in, fft_out);
			sgl_begin_line_strip();
			for (int i = 0; i < FFT_SIZE / 2; ++i) {
				float amplitude = sqrtf(fft_out[i][0] * fft_out[i][0] + fft_out[i][1] * fft_out[i][1]) / (float)FFT_SIZE;

				float lerp_factor = sqrtf(amplitude);
				if (playing_forward) {
					sgl_c3f(
						lerp(lerp_factor, 0.f, 1.f),
						lerp(lerp_factor, 1.f, 0.f),
						lerp(lerp_factor, 1.f, 0.f)
					);
				} else {
					sgl_c3f(
						lerp(lerp_factor, 1.f, 1.f),
						0.f,
						lerp(lerp_factor, 1.f, 0.f)
					);
				}
				sgl_v2f(
					(float)i / ((float)FFT_SIZE / 2.f) * width + 1.f,
					height - height * amplitude
				);
			}
			sgl_end();
		}
	}

	// Actual rendering
	sg_begin_pass(&(sg_pass){
		.swapchain = sglue_swapchain(),
		.action.colors[0] = {
			.load_action = SG_LOADACTION_CLEAR,
		},
	});
	sgl_draw();
	sg_end_pass();
	sg_commit();

	if (audio_state_ptr != NULL) {
		bytebeat->t = last_audio_state.t;
		bytebeat->v = last_audio_state.v;
	}
}

static void
audio(float* buffer, int num_frames, int num_channels) {
	bytebeat_t* bytebeat = &audio_thread_devices.bytebeat;

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

			reset_jit(audio_thread_vm);

			BLOG_DEBUG("Loaded new rom: %d bytes", cmd->rom.size);
		}

		if (cmd->cmds & AUDIO_CMD_SYNC_ZERO_PAGE) {
			memcpy(
				audio_thread_vm->memory,
				cmd->zero_page,
				sizeof(cmd->zero_page)
			);
			BLOG_DEBUG("Synced zero page");
		}

		if (cmd->cmds & AUDIO_CMD_SYNC_BYTEBEAT) {
			if (cmd->bytebeat.sync_bits & BYTEBEAT_SYNC_VECTOR) {
				bytebeat->vector = cmd->bytebeat.vector;
				BLOG_DEBUG("Updated .Bytebeat/vector");
			}

			if (cmd->bytebeat.sync_bits & BYTEBEAT_SYNC_T) {
				bytebeat->t = cmd->bytebeat.t;
				BLOG_DEBUG("Updated .Bytebeat/t");
			}

			if (cmd->bytebeat.sync_bits & BYTEBEAT_SYNC_V) {
				bytebeat->v = cmd->bytebeat.v;
				BLOG_DEBUG("Updated .Bytebeat/v");
			}
		}

		cmd->cmds = 0;
		tribuf_end_recv(&audio_cmd_buf);
	}

	// Send state update
	audio_state_t* audio_state = tribuf_begin_send(&audio_state_buf);
	audio_state->t = bytebeat->t;
	audio_state->v = bytebeat->v;
	audio_state->timestamp = stm_now();
	tribuf_end_send(&audio_state_buf);

	// Render audio
	devices_t* devices = audio_thread_vm->config.userdata;
	buxn_jit_t* jit = devices->jit;
	for (int i = 0; i < num_frames; ++i, bytebeat->t += bytebeat->v) {
		buxn_jit_execute(jit, bytebeat->vector);
		buffer[i] = (float)bytebeat->b / 255.f * 2.f - 1.f;
	}
}

// }}}

// Devices {{{

uint8_t
buxn_vm_dei(buxn_vm_t* vm, uint8_t address) {
	devices_t* devices = vm->config.userdata;
	switch (buxn_device_id(address)) {
		case BUXN_DEVICE_SYSTEM:
			return buxn_system_dei(vm, address);
		case BUXN_DEVICE_CONSOLE:
			return buxn_console_dei(vm, &devices->console, address);
		case BUXN_DEVICE_MOUSE:
			return buxn_mouse_dei(vm, &devices->mouse, address);
		case BUXN_DEVICE_CONTROLLER:
			return buxn_controller_dei(vm, &devices->controller, address);
		case BUXN_DEVICE_SCREEN:
			if (devices->screen) {
				return buxn_screen_dei(vm, devices->screen, address);
			} else {
				return 0;
			}
		case BUXN_DEVICE_DATETIME:
			return buxn_datetime_dei(vm, address);
		case BYTEBEAT_VECTOR:
			return bytebeat_dei(vm, &devices->bytebeat, address);
		case BUXN_DEVICE_FPU:
			return buxn_fpu_dei(vm, &devices->fpu, address);
		default:
			return vm->device[address];
	}
}

void
buxn_vm_deo(buxn_vm_t* vm, uint8_t address) {
	devices_t* devices = vm->config.userdata;
	switch (buxn_device_id(address)) {
		case BUXN_DEVICE_SYSTEM:
			buxn_system_deo(vm, address);
			break;
		case BUXN_DEVICE_CONSOLE:
			buxn_console_deo(vm, &devices->console, address);
			break;
		case BUXN_DEVICE_MOUSE:
			buxn_mouse_deo(vm, &devices->mouse, address);
			break;
		case BUXN_DEVICE_CONTROLLER:
			buxn_controller_deo(vm, &devices->controller, address);
			break;
		case BUXN_DEVICE_SCREEN:
			if (devices->screen) {
				buxn_screen_deo(vm, devices->screen, address);
			}
			break;
		case BYTEBEAT_VECTOR:
			bytebeat_deo(vm, &devices->bytebeat, address);
			break;
		case BUXN_DEVICE_FPU:
			buxn_fpu_deo(vm, &devices->fpu, address);
			break;
	}
}

void
buxn_system_debug(struct buxn_vm_s* vm, uint8_t value) {
	if (value == 0) { return; }

	fprintf(stderr, "WST");
	for (uint8_t i = 0; i < vm->wsp; ++i) {
		fprintf(stderr, " %02hhX", vm->ws[i]);
	}
	fprintf(stderr, "\n");

	fprintf(stderr, "RST");
	for (uint8_t i = 0; i < vm->rsp; ++i) {
		fprintf(stderr, " %02hhX", vm->rs[i]);
	}
	fprintf(stderr, "\n");
}

void
buxn_system_set_metadata(struct buxn_vm_s* vm, uint16_t address) {
	buxn_metadata_t metadata = buxn_metadata_parse_from_memory(vm, address);
	if (metadata.content == NULL) {
		BLOG_WARN("ROM tried to set invalid metadata");
		return;
	}

	char* ch = metadata.content;
	while (ch < metadata.content + metadata.content_len && *ch != '\n') {
		++ch;
	}
	char old_char = *ch;
	*ch = '\0';
	sapp_set_window_title(metadata.content);
	*ch = old_char;
}

void
buxn_system_theme_changed(struct buxn_vm_s* vm) {
	// TODO: update visualization
}

void
buxn_console_handle_write(struct buxn_vm_s* vm, buxn_console_t* device, char c) {
	(void)vm;
	(void)device;
	fputc(c, stdout);
	fflush(stdout);
}

void
buxn_console_handle_error(struct buxn_vm_s* vm, buxn_console_t* device, char c) {
	(void)vm;
	(void)device;
	fputc(c, stderr);
	fflush(stdout);
}

buxn_screen_t*
buxn_screen_request_resize(
	struct buxn_vm_s* vm,
	buxn_screen_t* screen,
	uint16_t width, uint16_t height
) {
	BLOG_WARN("Resizing is not supported");
	return screen;
}

void*
buxn_jit_alloc(buxn_jit_alloc_ctx_t* ctx, size_t size, size_t alignment) {
	return barena_memalign((barena_t*)ctx, size, alignment);
}

// }}}

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

static const char*
parse_log_level(void* userdata, const char* value) {
	blog_level_t* log_level = userdata;
	if        (strcmp(value, "trace") == 0) {
		*log_level = BLOG_LEVEL_TRACE;
	} else if (strcmp(value, "debug") == 0) {
		*log_level = BLOG_LEVEL_DEBUG;
	} else if (strcmp(value, "info") == 0) {
		*log_level = BLOG_LEVEL_INFO;
	} else if (strcmp(value, "warn") == 0) {
		*log_level = BLOG_LEVEL_WARN;
	} else if (strcmp(value, "error") == 0) {
		*log_level = BLOG_LEVEL_ERROR;
	} else if (strcmp(value, "fatal") == 0) {
		*log_level = BLOG_LEVEL_FATAL;
	} else {
		return "Invalid log level";
	}

	return NULL;
}

int
main(int argc, const char* argv[]) {
	blog_level_t log_level = BLOG_LEVEL_INFO;
	int width = 640;
	int height = 480;
	barg_opt_t opts[] = {
		{
			.name = "log",
			.summary = "Log level",
			.description = "Accepted values are: 'trace', 'debug', 'info', 'warn', 'error', 'fatal'",
			.value_name = "level",
			.short_name = 'l',
			.parser = {
				.parse = parse_log_level,
				.userdata = &log_level,
			},
		},
		{
			.name = "width",
			.summary = "Initial window width",
			.short_name = 'w',
			.parser = barg_int(&width),
		},
		{
			.name = "height",
			.summary = "Initial window height",
			.short_name = 'h',
			.parser = barg_int(&height),
		},
		barg_opt_help(),
	};
	barg_t barg = {
		.usage = "ubeat [options] [input.tal]",
		.summary = "Start the live coding session",
		.opts = opts,
		.num_opts = sizeof(opts) / sizeof(opts[0]),
		.allow_positional = true,
	};
	barg_result_t result = barg_parse(&barg, argc, argv);
	if (result.status != BARG_OK) {
		barg_print_result(&barg, result, stderr);
		return result.status == BARG_PARSE_ERROR;
	}
	int num_args = argc - result.arg_index;

	if (num_args == 1) {
		input_file = argv[result.arg_index];
	}

	blog_init(&(blog_options_t){
		.current_depth_in_project = 0,
		.current_filename = __FILE__,
	});
	blog_add_file_logger(log_level, &(blog_file_logger_options_t){
		.file = stderr,
		.with_colors = true,
	});

	sapp_run(&(sapp_desc){
		.init_cb = init,
		.frame_cb = frame,
		.event_cb = event,
		.cleanup_cb = cleanup,
		.width = width,
		.height = height,
		.window_title = "ubeat",
		.icon.sokol_default = true,
		.enable_dragndrop = true,
		.max_dropped_files = 1,
		.logger.func = slog,
	});

	return 0;
}
