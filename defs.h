#include "a.h"
#include <fcntl.h>
#include <semaphore.h>

#define _VQUEUE_INIT_MAPPING 16384

struct _vqueue_msg_hdr{
	// High bits used to indicate whether it has been consumed yet
	_Atomic uint32_t aid;
	_Atomic size_t size;
	_Atomic uint64_t next;
	uint8_t payload[];
};
#define _VQUEUE_MSG_HDR_SIZE sizeof(struct _vqueue_msg_hdr)-4

struct _vqueue_shmem_region{
	sem_t sema4;
	_Atomic uint32_t open_counter;
	_Atomic uint32_t trim_lock;
	_Atomic uint64_t head, tail, left;
	union{ struct{ _Atomic uint64_t right1, right1init, right2, right2init; }; _Atomic uint64_t right_v[4]; };
	// 8-way hash table of hazard pointers
	_Atomic uint64_t hazfield[256];
	char unused_[_VQUEUE_MSG_HDR_SIZE];
	_Alignas(64) char blocks[][64];
};

#define _vqueue_offsetof(member) ((size_t) &((struct _vqueue_shmem_region*)0)->member)

struct _vqueue_mapping_descriptor{
	_Atomic uint64_t ref;
	uint64_t ptr:40, size_packed:24;
	struct _vqueue_mapping_descriptor *next;
};

struct _vqueue_mapping{
	struct _vqueue* q;
	union{ struct _vqueue_shmem_region* data; _Atomic uint64_t* data8; };
	size_t size;
};

struct _vqueue{
	int shmem_fd; uint32_t aid;
	lock_t mapping_lock;
	struct _vqueue_mapping_descriptor mapping;
};

static inline uint64_t _vqueue_mix64(uint64_t x){
	x *= 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 27;
	x ^= x >> 31;
	return x;
}

// "Protect" a pointer
static inline uint8_t _vqueue_protect(struct _vqueue_mapping* ctx, uint64_t ptr){
	uint64_t h = _vqueue_mix64(ptr);
	ptr = ~(ptr|ctx->q->aid<<40);
	for(unsigned i = 0; ; i++){
		uint8_t n = h>>((i&7)*8)&255;
		uint64_t current = 0;
		retry:
		if(atomic_compare_exchange_strong_explicit(&ctx->data->hazfield[n], &current, ptr, memory_order_acquire, memory_order_relaxed)){
			return n;
		}
		if(i >= 63){
			struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = (~current)>>40, .l_len = 1 };
			if(!fcntl(ctx->q->shmem_fd, F_GETLK, &l) && l.l_type == F_UNLCK){
				// previous slot owner died, it's okay to replace the current value
				i &= 7;
				goto retry;
			}
		}
		if(i >= 15) thread_yield();
	}
	thread_memory_barrier(mb_co_acquire);
}

static inline bool _vqueue_trim(struct _vqueue_mapping*, uint64_t, bool);

static inline void _vqueue_unprotect(struct _vqueue_mapping* ctx, uint64_t ptr, uint8_t n){
	atomic_store_explicit(&ctx->data->hazfield[n], 0, memory_order_release);
	if(atomic_load_explicit(&ctx->data->left, memory_order_acquire) == ptr) _vqueue_trim(ctx, ptr, false);
}

static inline bool _vqueue_check(struct _vqueue_mapping* ctx, uint64_t ptr){
	thread_memory_barrier(mb_co_release);
	uint64_t h = _vqueue_mix64(ptr);
	for(unsigned i = 0; i < 8; i++){
		_Atomic uint64_t* slot = &ctx->data->hazfield[h>>((i&7)*8)&255];
		uint64_t val = atomic_load_explicit(slot, memory_order_relaxed);
		if(((~val) & 0xFFFFFFFFFF) == ptr){
			// Ownership transferred
			return false;
		}
	}
	thread_memory_barrier(mb_acquire);
	return true;
}

static inline uint64_t _vqueue_pointer_acquire2(struct _vqueue_mapping* ctx, size_t ptr, uint64_t v, uint8_t* lock_out){
	retry: {}
	if((v&0xFFFFFFFFFF) == 0xFFFFFFFFFF) return v;
	uint8_t n = _vqueue_protect(ctx, v);
	uint64_t v2 = v; v = atomic_load_explicit(ctx->data8+ptr, memory_order_acquire);
	if(v != v2){
		_vqueue_unprotect(ctx, v2, n);
		goto retry;
	}
	*lock_out = n;
	return v;
}

static inline uint64_t _vqueue_pointer_acquire(struct _vqueue_mapping* ctx, size_t ptr, uint8_t* lock_out){
	_vqueue_pointer_acquire2(ctx, ptr, lock_out, atomic_load_explicit(ctx->data8+ptr, memory_order_relaxed));
}

static inline uint64_t _vqueue_compress_size(size_t sz){
	if(sz <= 0x4000000) return (sz+63)>>6;
	unsigned long magn = 0;
	// 64-clz64(x)
#ifdef _MSC_VER
	if(_BitScanReverse64(&magn, sz)) magn++;
#else
	magn = __LLONG_WIDTH__-__builtin_clzll(sz);
#endif
	magn -= 26;
	sz += (1ull<<magn)-1; sz >>= magn;
	return (magn+1)<<19 | (sz&0x7FFFF);
}
static inline size_t _vqueue_uncompress_size(uint64_t x){
	if(x <= 0x100000) return x<<6;
	return ((x&0x7FFFF)|0x80000)<<((x>>19)-1);
}