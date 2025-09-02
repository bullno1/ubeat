#ifndef TRIBUF_H
#define TRIBUF_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
	atomic_uintptr_t incoming_ptr;
	uintptr_t ptrs[3];
	int outgoing_index;
	bool should_swap;
} tribuf_t;

static inline void
tribuf_init(tribuf_t* buf, void* storage, size_t element_size) {
	buf->incoming_ptr = 0;
	buf->outgoing_index = 0;
	// At least 3 slots are needed: in-use, incoming, outgoing
	buf->ptrs[0] = (uintptr_t)storage + element_size * 0;
	buf->ptrs[1] = (uintptr_t)storage + element_size * 1;
	buf->ptrs[2] = (uintptr_t)storage + element_size * 2;
	buf->should_swap = false;
}

static inline void
tribuf_try_swap(tribuf_t* buf) {
	if (!buf->should_swap) { return; }

	uintptr_t null = 0;
	bool submitted = atomic_compare_exchange_strong_explicit(
		&buf->incoming_ptr, &null, buf->ptrs[buf->outgoing_index],
		memory_order_release, memory_order_relaxed
	);

	if (submitted) {
		buf->outgoing_index = (buf->outgoing_index + 1) % 3;
	}

	buf->should_swap = !submitted;
}

static inline void*
tribuf_begin_send(tribuf_t* buf) {
	return (void*)buf->ptrs[buf->outgoing_index];
}

static inline void
tribuf_end_send(tribuf_t* buf) {
	buf->should_swap = true;
	tribuf_try_swap(buf);
}

static inline void*
tribuf_begin_recv(tribuf_t* buf) {
	return (void*)atomic_load_explicit(
		&buf->incoming_ptr, memory_order_acquire
	);
}

static inline void
tribuf_end_recv(tribuf_t* buf) {
	atomic_store_explicit(&buf->incoming_ptr, 0, memory_order_release);
}

#endif
