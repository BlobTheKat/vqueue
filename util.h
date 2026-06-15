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

// "Protect" a pointer
static inline uint8_t _vqueue_protect(struct _vqueue* q, uint64_t ptr){
	union{
		uint64_t u64;
		uint8_t u8[8];
	} hash = {.u64 = _vqueue_mix64(ptr)};
	ptr |= q->aid<<40;
	for(unsigned i = 0; ; i++){
		uint8_t n = hash.u8[i&7];
		uint64_t current = 0;
		retry:
		if(atomic_compare_exchange_strong_explicit(&q->header->hazfield[n], &current, ptr, memory_order_acquire, memory_order_relaxed)){
			return n;
		}
		if(i >= 63){
			struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = current&0xFFFFFF, .l_len = 1 };
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
}

static inline bool _vqueue_reprotect(struct _vqueue* q, uint8_t n, uint64_t ptr){
	uint64_t put = ptr<<24|q->aid;
	put = atomic_exchange_explicit(&q->header->hazfield[n], put, memory_order_acquire, memory_order_relaxed);
	return (put | 0xFFFFFF0000000000) == (uint64_t)-1ull;
}

static inline bool _vqueue_unprotect(struct _vqueue* q, uint8_t n){
	return (atomic_exchange_explicit(&q->header->hazfield[n], 0, memory_order_release) | 0xFFFFFF0000000000) == (uint64_t)-1ull;
}

static inline uint32_t _vqueue_kill(struct _vqueue* q, uint64_t ptr){
	atomic_thread_fence(memory_order_release);
	union{
		uint64_t u64;
		uint8_t u8[8];
	} hash = {.u64 = _vqueue_mix64(ptr)};
	for(unsigned i = 0; i < 8; i++){
		_Atomic uint64_t* slot = &q->header->hazfield[hash.u8[i&7]];
		uint64_t val = atomic_load_explicit(slot, memory_order_relaxed);
		if((val >> 24) == ptr){
			uint64_t val2 = val & 0xFFFFFF0000000000 | 0xFFFFFFFFFF;
			if(atomic_compare_exchange_strong_explicit(slot, &val, val2, memory_order_relaxed, memory_order_relaxed)){
				// Ownership transferred
				return val>>40;
			}
		}
	}
	return -1u;
}

static inline void _vqueue_try_free(struct _vqueue* q, uint64_t ptr);

static inline uint64_t _vqueue_pointer_acquire2(struct _vqueue* q, _Atomic uint64_t* ptr, uint64_t v, uint8_t* lock_out){
	if(!(v&0xFFFFFFFFFF)) return v;
	uint8_t n = _vqueue_protect(q, v);
	retry: {}
	uint64_t v2 = v; v = atomic_load_explicit(ptr, memory_order_acquire);
	if(v != v2){
		if(_vqueue_reprotect(q, n, v)){
			_vqueue_try_free(q, v2);
		}
		goto retry;
	}
	*lock_out = n;
	return v;
}

static inline uint64_t _vqueue_pointer_acquire(struct _vqueue* q, _Atomic uint64_t* ptr, uint8_t* lock_out){
	_vqueue_pointer_acquire2(q, ptr, lock_out, atomic_load_explicit(ptr, memory_order_acquire));
}

static inline uint64_t _vqueue_pointer_release(struct _vqueue* q, uint64_t ptr, uint8_t lock){
	if((ptr&0xFFFFFFFFFF) && _vqueue_unprotect(q, lock)) _vqueue_try_free(q, ptr);
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