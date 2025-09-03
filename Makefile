.PHONY: all clean

SANITIZE := -fsanitize=address,undefined -fno-sanitize=vptr

OBJS := \
	.build/src/main.c.o \
	.build/src/bytebeat.c.o \
	.build/src/libs.c.o \
	.build/deps/buxn/src/devices/system.c.o \
	.build/deps/buxn/src/devices/console.c.o \
	.build/deps/buxn/src/devices/mouse.c.o \
	.build/deps/buxn/src/devices/controller.c.o \
	.build/deps/buxn/src/devices/screen.c.o \
	.build/deps/buxn/src/devices/datetime.c.o \
	.build/deps/buxn/src/metadata.c.o \
	.build/deps/buxn/src/vm/vm.c.o \
	.build/deps/buxn/src/asm/asm.c.o \
	.build/deps/buxn/src/vm/vm.c.o

all: ubeat

clean:
	rm -rf .build sbeat *.dbg

ubeat: $(OBJS)
	clang \
		-g \
		-flto \
		-O3 \
		-fno-omit-frame-pointer \
		-fuse-ld=mold \
		-Wl,--separate-debug-file \
		${SANITIZE} \
		-lX11 -lXi -lXcursor -lEGL -lGL -lasound -lm \
		$^ \
		-o $@

.build/%.c.o: %.c
	mkdir -p $(shell dirname $@)
	clang \
		-c \
		-flto \
		-g \
		-O3 \
		-fno-omit-frame-pointer \
		${SANITIZE} \
		-Wall \
		-Werror \
		-pedantic \
		-std=c11 \
		-Ideps/blibs \
		-Ideps/sokol \
		-Ideps/sokol/util \
		-Ideps/buxn/include \
		-Ideps/am_fft \
		-o $@ \
		$^
