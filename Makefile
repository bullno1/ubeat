.PHONY: all clean

SANITIZE := -fsanitize=address,undefined -fno-sanitize=vptr

all: ubeat

clean:
	rm -rf .build sbeat *.dbg

ubeat: .build/main.o .build/libs.o .build/deps/buxn/src/asm/asm.o .build/deps/buxn/src/vm/vm.o
	clang \
		-O3 \
		-fno-omit-frame-pointer \
		-fuse-ld=mold \
		-Wl,--separate-debug-file \
		${SANITIZE} \
		-lX11 -lXi -lXcursor -lEGL -lGL -lasound -lm \
		$^ \
		-o $@

.build/%.o: %.c
	mkdir -p $(shell dirname $@)
	clang \
		-c \
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
