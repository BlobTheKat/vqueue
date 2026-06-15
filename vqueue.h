#pragma once
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>

typedef struct _vqueue vqueue_t;
typedef struct{
	size_t size;
	uint8_t* data;
} vqueue_block_t;

// Open a vqueue from a path identifier. This path does not point to a path on the filesystem, but rather an abstract namespace managed by the operating system. The queue is created if it does not yet exist
bool vqueue_open(vqueue_t* q_out, const char* name, size_t name_sz);
// Close a vqueue handle. The underlying queue is not touched, messages may still be appended, and will continue to consume memory until a reader marks them as seen.
void vqueue_close(vqueue_t* q);
// Unlink a path to a vqueue. Any open handles to the underlying queue remain open and may be used, but all pending messages will be freed once the last handle is closed. A new, separate vqueue can be opened at the same path once this function returns
bool vqueue_unlink(const char* name, size_t name_sz);

vqueue_block_t vqueue_alloc(vqueue_t* q, size_t size);
void vqueue_post(vqueue_t* q, vqueue_block_t block);

vqueue_block_t vqueue_wait(vqueue_t* q);
void vqueue_free(vqueue_t* q, vqueue_block_t block);

#if defined(VQUEUE_IMPL) && defined(_WIN32)
	#include "vqueue_win.c"
#elif defined(VQUEUE_IMPL) && defined(_POSIX_VERSION)
	#include "vqueue_posix.c"
#endif