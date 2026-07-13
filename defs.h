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

#define _VQUEUE_SHARED_FDS
#define _vqueue_shmem_fd e->fd
struct _vqueue_fd_map_entry{
	struct _vqueue_fd_map_entry* next; uint64_t hash; int fd; _Atomic uint32_t ref; char name[NAME_MAX+1];
};
lock_t _vqueue_fd_map_s = LOCK_MAX, _vqueue_fd_map_e = 1;
struct _vqueue_fd_map_entry** _vqueue_fd_map = 0;
size_t _vqueue_fd_map_count = 0;
struct _vqueue_fd_map_entry* _vqueue_fd_map_find(const char* name, size_t name_sz){
	uint64_t hash = 0;
	size_t end = name_sz & ~7;
	memcpy(&hash, name+end, name_sz&7);
	for(size_t i = 0; i < end; i += 8){
		uint64_t n;
		memcpy(&n, name+i, 8);
		hash = _vqueue_mix64(hash) ^ n;
	}
	struct _vqueue_fd_map_entry* e, **ep;
	bool excl = false;
	lock_wait(&_vqueue_fd_map_e, 1);
	lock_acquire(&_vqueue_fd_map_s, 1);
	retry:
	if(!_vqueue_fd_map_count){
		if(!excl) get_excl: {
			excl = true;
			if(!lock_try_acquire(&_vqueue_fd_map_e, 1)){
				lock_release(&_vqueue_fd_map_s, 1);
				lock_acquire(&_vqueue_fd_map_e, 1);
				lock_acquire(&_vqueue_fd_map_s, LOCK_MAX);
				goto retry;
			}else{
				lock_acquire(&_vqueue_fd_map_s, LOCK_MAX);
			}
		}
		ep = (struct _vqueue_fd_map_entry**) &_vqueue_fd_map;
		_vqueue_fd_map_count = 1;
		init:
		struct _vqueue_fd_map_entry* old = *ep;
		*ep = e = (struct _vqueue_fd_map_entry*) malloc(sizeof(struct _vqueue_fd_map_entry));
		if(!e) goto end;
		atomic_init(&e->ref, 1);
		e->hash = hash;
		e->next = old;
		e->fd = -1;
		memcpy(e->name, name, name_sz);
		e->name[name_sz] = '\0';
	}else{
		uint64_t mask = (_vqueue_fd_map_count-1)>>2;
		mask |= mask>>1; mask |= mask>>2; mask |= mask>>4; mask |= mask>>8; mask |= mask>>16; mask |= mask>>32;

		e = !mask ? (struct _vqueue_fd_map_entry*) _vqueue_fd_map : _vqueue_fd_map[hash & mask];
		while(e){
			if(e->hash == hash && !memcmp(e->name, name, name_sz)){
				atomic_fetch_add_explicit(&e->ref, 1, memory_order_relaxed);
				goto end;
			}
			e = e->next;
		}
		if(!excl) goto get_excl;
		ep = !mask ? (struct _vqueue_fd_map_entry**) &_vqueue_fd_map : &_vqueue_fd_map[hash & mask];

		if(_vqueue_fd_map_count >= 4 && !(_vqueue_fd_map_count&(_vqueue_fd_map_count-1))){
			mask = mask<<1|1;
			size_t sz = sizeof(struct _vqueue_fd_map_entry*)*(mask+1);
			struct _vqueue_fd_map_entry** list = malloc(sz);
			if(!list){ e = 0; goto end; }
			memset(list, 0, sz);
			for(size_t i = 0; i <= mask>>1; i++){
				e = !mask ? (struct _vqueue_fd_map_entry*) _vqueue_fd_map : &_vqueue_fd_map[i];
				while(e){
					struct _vqueue_fd_map_entry *prev = list[e->hash&mask], *next = e->next;
					e->next = prev; list[e->hash&mask] = e;
					e = next;
				}
			}
			_vqueue_fd_map = list;
			ep = (struct _vqueue_fd_map_entry**) &list[hash&mask];
		}
		_vqueue_fd_map_count++;
		goto init;
	}
	end:
	if(excl){
		lock_release(&_vqueue_fd_map_s, LOCK_MAX-1);
		lock_release(&_vqueue_fd_map_e, 1);
	}
	return e;
}

void _vqueue_fd_map_finish(struct _vqueue_fd_map_entry* e){
	if(atomic_fetch_sub_explicit(&e->ref, 1, memory_order_release) > 1) return;
	bool excl = false;
	lock_acquire(&_vqueue_fd_map_e, 1);
	lock_acquire(&_vqueue_fd_map_s, LOCK_MAX);
	if(!_vqueue_fd_map_count || atomic_load_explicit(&e->ref, memory_order_relaxed)) goto end;
	close(e->fd);
	struct _vqueue_fd_map_entry* e2, **ep;
	uint64_t mask = (_vqueue_fd_map_count-1)>>2;
	ep = !mask ? (struct _vqueue_fd_map_entry**) &_vqueue_fd_map : &_vqueue_fd_map[e->hash & mask]; e2 = *ep;
	while(e2){
		if(e == e2){
			*ep = e->next;
			free(e);
			_vqueue_fd_map_count--;
			if(_vqueue_fd_map_count >= 4 && !(_vqueue_fd_map_count&(_vqueue_fd_map_count-1))){
				size_t omask = mask; mask >>= 1;
				size_t sz = sizeof(struct _vqueue_fd_map_entry*)*(mask+1);
				struct _vqueue_fd_map_entry** old = _vqueue_fd_map, **list;
				if(mask){
					_vqueue_fd_map = list = malloc(sz);
					if(!list) abort();
				}else list = (struct _vqueue_fd_map_entry**) &_vqueue_fd_map;
				memset(list, 0, sz);
				for(size_t i = 0; i <= omask; i++){
					e2 = old[i];
					while(e2){
						struct _vqueue_fd_map_entry *prev = list[e2->hash&mask], *next = e2->next;
						e2->next = prev; list[e2->hash&mask] = e2;
						e2 = next;
					}
				}
				free(old);
			}
		}
		e2 = *(ep = &e2->next);
	}
	end:
	lock_release(&_vqueue_fd_map_s, LOCK_MAX);
	lock_release(&_vqueue_fd_map_e, 1);
}
#endif

struct _vqueue_msg_hdr{
	// High bits used to indicate whether it has been claimed from the queue yet
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
	_Atomic uint64_t lsize;
	_Atomic uint64_t head, tail, left;
	union{ struct{ _Atomic uint64_t right1, right1init, right2, right2init; }; _Atomic uint64_t right_v[4]; };
	// 8-way hash table of hazard pointers
	_Atomic uint64_t hazarena[256];
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
#ifdef _VQUEUE_SHARED_FDS
	struct _vqueue_fd_map_entry* e;
#else
	int _vqueue_shmem_fd;
#endif
	uint32_t aid;
	lock_t mapping_lock;
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
	ptr = ~(ptr|ctx->q->aid<<40);
	for(unsigned i = 0; ; i++){
		uint8_t n = h>>((i&7)*8)&255;
		uint64_t current = 0;
		retry:
		if(atomic_compare_exchange_strong_explicit(&ctx->data->hazarena[n], &current, ptr, memory_order_acquire, memory_order_relaxed)){
			return n;
		}
		if(i >= 63){
			struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = (~current)>>40, .l_len = 1 };
			if(!fcntl(ctx->q->_vqueue_shmem_fd, F_GETLK, &l) && l.l_type == F_UNLCK){
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

_Static_assert(ATOMIC_INT64_LOCK_FREE, "atomic uint64_t is not lock-free. Non-lock-free atomics do not work on shared memory");