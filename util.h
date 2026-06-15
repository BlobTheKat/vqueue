#include <stdint.h>
#include <stdatomic.h>
#include "a.h"

static inline uint64_t _vqueue_mix64(uint64_t x){
	x ^= x >> 30;
	x *= 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 27;
	x *= 0x94d049bb133111ebULL;
	x ^= x >> 31;
	return x;
}

static inline uint8_t _vqueue_protect_h(struct _vqueue* q, struct _vqueue_shmem_region* mapping, uint64_t ptr, uint64_t h){
	ptr = ~(ptr|q->aid<<40);
	for(unsigned i = 0; ; i++){
		uint8_t n = h>>((i&7)*8)&255;
		uint64_t current = 0;
		retry:
		if(atomic_compare_exchange_strong_explicit(&mapping->hazfield[n], &current, ptr, memory_order_acquire, memory_order_relaxed)){
			return n;
		}
		if(i >= 63){
			struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = (~current)>>40, .l_len = 1 };
			if(!fcntl(q->shmem_fd, F_GETLK, &l) && l.l_type == F_UNLCK){
				// previous slot owner died, it's okay to replace the current value
				i &= 7;
				goto retry;
			}
		}
		if(i >= 15){
			thread_yield();
		}
	}
	thread_memory_barrier(mb_co_acquire);
}

// "Protect" a pointer
static inline uint8_t _vqueue_protect(struct _vqueue* q, struct _vqueue_shmem_region* mapping, uint64_t ptr){
	_vqueue_protect_h(q, mapping, ptr, _vqueue_mix64(ptr));
}

static inline void _vqueue_trim(struct _vqueue* q, struct _vqueue_shmem_region* mapping, uint64_t left);

static inline void _vqueue_unprotect(struct _vqueue* q, struct _vqueue_shmem_region* mapping, uint64_t ptr, uint8_t n, uint64_t z){
	atomic_store_explicit(&mapping->hazfield[n], z, memory_order_release);
	if(atomic_load_explicit((_Atomic uint64_t*) &mapping->left, memory_order_acquire) == ptr){
		_vqueue_trim(q, mapping, ptr);
	}
}

static inline uint32_t _vqueue_check_protect(struct _vqueue* q, struct _vqueue_shmem_region* mapping, uint64_t ptr){
	uint64_t h = _vqueue_mix64(ptr);
	for(unsigned i = 0; i < 8; i++){
		_Atomic uint64_t* slot = &mapping->hazfield[h>>((i&7)*8)&255];
		uint64_t val = atomic_load_explicit(slot, memory_order_relaxed);
		if(((~val) & 0xFFFFFFFFFF) == ptr){
			// Ownership transferred
			return -1u;
		}
	}
	return _vqueue_protect_h(q, mapping, ptr, h);
}

static inline uint64_t _vqueue_pointer_acquire2(struct _vqueue* q, struct _vqueue_shmem_region* mapping, _Atomic uint64_t* ptr, uint64_t v, uint8_t* lock_out){
	if(!(v&0xFFFFFFFFFF)) return v;
	uint8_t n = _vqueue_protect(q, mapping, v);
	retry: {}
	uint64_t v2 = v; v = atomic_load_explicit(ptr, memory_order_relaxed);
	if(v != v2){
		_vqueue_unprotect(q, mapping, v2, n, ~(v<<24|q->aid));
		goto retry;
	}
	*lock_out = n;
	return v;
}

static inline uint64_t _vqueue_pointer_acquire(struct _vqueue* q, struct _vqueue_shmem_region* mapping, _Atomic uint64_t* ptr, uint8_t* lock_out){
	_vqueue_pointer_acquire2(q, mapping, ptr, lock_out, atomic_load_explicit(ptr, memory_order_acquire));
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