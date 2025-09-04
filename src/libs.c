#if defined(__linux__)
#	define SOKOL_GLES3
#	define _GNU_SOURCE
#else
#	error "Unsupported platform"
#endif

#define SOKOL_NO_ENTRY
#define SOKOL_IMPL
#include <sokol_app.h>
#include <sokol_log.h>
#include <sokol_gfx.h>
#include <sokol_glue.h>
#include <sokol_gl.h>
#include <sokol_audio.h>
#include <sokol_time.h>

#define BLIB_IMPLEMENTATION
#include <blog.h>
#include <bresmon.h>
#include <barena.h>
#include <bhash.h>
#include <barg.h>

#define AM_FFT_IMPLEMENTATION
#ifdef __clang__
#	pragma clang diagnostic ignored "-Wnewline-eof"
#endif
#include <am_fft.h>
