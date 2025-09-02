#include <sokol_app.h>
#include <sokol_gfx.h>
#include <sokol_glue.h>
#include <sokol_gl.h>
#include <sokol_audio.h>
#include <sokol_time.h>
#include <blog.h>

#define SAMPLING_RATE 8000

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
	sg_setup(&(sg_desc){
		.environment = sglue_environment(),
		.logger.func = slog,
	});
	sgl_setup(&(sgl_desc_t){
		.logger = {
			.func = slog,
		},
	});

	/*saudio_setup(&(saudio_desc){*/
		/*.sample_rate = SAMPLING_RATE,*/
		/*.num_channels = 1,*/
		/*.stream_cb = audio,*/
		/*.logger = {*/
			/*.func = slog_func,*/
		/*},*/
	/*});*/
}

static void
frame(void) {
}

static void
cleanup(void) {
	sgl_shutdown();
	sg_shutdown();
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
		.window_title = "uxn-beat",
		.icon.sokol_default = true,
		.logger.func = slog,
	};
}
