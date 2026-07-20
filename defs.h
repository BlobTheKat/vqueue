#include "a.h"
#include <fcntl.h>
#include <semaphore.h>

// 1 page on xnu, 4 pages elsewhere
#define _VQUEUE_INIT_MAPPING 16384
#define _VQUEUE_PTR_INVALID 0xFFFFFFFFFF

#ifdef F_OFD_GETLK
	#define _VQUEUE_F_GETLK F_OFD_GETLK
	#define _VQUEUE_F_SETLK F_OFD_SETLK
#else // for the OSes that don't have reasonable lock semantics, we have a fallback that involves sharing fds within the process
	#define _VQUEUE_F_GETLK F_GETLK
	#define _VQUEUE_F_SETLK F_SETLK
#endif

struct _vqueue_msg_hdr{
	// Bit 24 used to indicate whether it has been claimed from the queue yet
#if SIZE_MAX == UINT32_MAX
#define _VQUEUE_MSG_HDR_SIZE sizeof(struct _vqueue_msg_hdr)
	_Atomic uint32_t aid, size;
#else
#define _VQUEUE_MSG_HDR_SIZE sizeof(struct _vqueue_msg_hdr)-4
	_Atomic uint32_t _, aid;
	_Atomic size_t size;
#endif
	_Atomic uint64_t next;
	uint8_t payload[];
};

struct _vqueue_shmem_region{
	sem_t sema4;
	_Atomic uint32_t open_counter, trim_lock;
	_Atomic uint64_t failed_trim, lsize;
	_Atomic uint64_t head, tail, left;
	_Atomic uint64_t right1, right1init, right2, right2init;
	// 8-way hash table of hazard pointers
	_Atomic uint64_t hazarena[256];
	char unused_[_VQUEUE_MSG_HDR_SIZE];
	_Alignas(64) char blocks[][64];
};

#define _vqueue_offsetof(member) ((&((struct _vqueue_shmem_region*)0x10000)->member)-(_Atomic uint64_t*)0x10000)

struct _vqueue_mapping_descriptor{
	lock_t lock;
	_Atomic uint32_t ref;
	uint64_t ptr:40, size_packed:24;
	struct _vqueue_mapping_descriptor *next;
};

struct _vqueue_mapping{
	struct _vqueue* q;
	union{ struct _vqueue_shmem_region* data; _Atomic uint64_t* data8; };
	uint8_t size;
};

struct _vqueue{
	int shmem_fd;
	uint32_t aid;
	struct _vqueue_mapping_descriptor mapping;
};

static inline uint64_t _vqueue_mix64(uint64_t x){
	x *= 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 27; x ^= x >> 31;
	return x;
}

// "Protect" a pointer
static inline uint8_t _vqueue_protect(struct _vqueue_mapping* ctx, uint64_t ptr){
	uint64_t h = _vqueue_mix64(ptr);
	h &= 0xFF00FF00FF00FFULL;
	h |= h<<8; h ^= 0x0100010001000100ULL;
	ptr = ~(ptr|(uint64_t)ctx->q->aid<<40);
	for(unsigned i = 0; ; i++){
		uint8_t n = h>>((i&7)*8)&255;
		uint64_t current = 0;
		retry:
		if(atomic_compare_exchange_strong_explicit(&ctx->data->hazarena[n], &current, ptr, memory_order_acquire, memory_order_relaxed)){
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
	if(ptr == _VQUEUE_PTR_INVALID) return;
	atomic_store_explicit(&ctx->data->hazarena[n], 0, memory_order_release);
	if(atomic_load_explicit(&ctx->data->left, memory_order_acquire) == ptr) _vqueue_trim(ctx, ptr, false);
}

static inline bool _vqueue_check(struct _vqueue_mapping* ctx, uint64_t ptr){
	thread_memory_barrier(mb_co_release);
	uint64_t h = _vqueue_mix64(ptr);
	h &= 0xFF00FF00FF00FFULL;
	h |= h<<8; h ^= 0x0100010001000100ULL;
	for(unsigned i = 0; i < 8; i++){
		_Atomic uint64_t* slot = &ctx->data->hazarena[h>>((i&7)*8)&255];
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
	if(v == _VQUEUE_PTR_INVALID) return v;
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
	return _vqueue_pointer_acquire2(ctx, ptr, atomic_load_explicit(ctx->data8+ptr, memory_order_relaxed), lock_out);
}

static inline uint8_t _vqueue_compress_size(size_t sz){
	if(sz <= 65536) return sz>>14;
	unsigned long magn = 0;
	// 64-clz64(x)
#ifdef _MSC_VER
	if(_BitScanReverse64(&magn, sz)) magn++;
#else
	magn = __LLONG_WIDTH__-__builtin_clzll(sz);
#endif
	magn -= 2;
	sz += (1ull<<magn)-1;
	return (magn-13)<<1 | ((sz>>magn)&1);
}
static inline size_t _vqueue_round_size(size_t sz){
	size_t mask = 16383;
	if(sz > 65536){
		unsigned long magn = 0;
		// 64-clz64(x)
#ifdef _MSC_VER
		if(_BitScanReverse64(&magn, sz)) magn++;
#else
		magn = __LLONG_WIDTH__-__builtin_clzll(sz);
#endif
		mask = (1ull<<(magn-2))-1;
	}
	sz += mask; sz &= ~mask;
	return sz;
}
static inline size_t _vqueue_uncompress_size(uint8_t x){
	if(x <= 4) return x<<14;
	return ((x&3)|2)<<((x>>1)+13);
}

_Static_assert(ATOMIC_INT64_LOCK_FREE, "atomic uint64_t is not lock-free. Non-lock-free atomics do not work on shared memory");