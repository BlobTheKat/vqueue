#pragma once
#define _FILE_OFFSET_BITS 64
#include <vqueue.h>
#include <limits.h>
#include <sys/mman.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include "defs.h"

static inline void* _vqueue_mmap(int fd, off_t off, size_t sz){
	void* addr = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
#define _VQUEUE_ERR_MSG "vqueue: mmap(): User address space exhausted\nThis is a FATAL out-of-memory exception\n"
	if(addr == (void*)-1){ write(STDERR_FILENO, _VQUEUE_ERR_MSG, sizeof(_VQUEUE_ERR_MSG)); abort(); }
#undef _VQUEUE_ERR_MSG
	return addr;
}

static inline struct _vqueue_mapping _vqueue_acquire_mapping(struct _vqueue* q){
	lock_acquire(&q->mapping_lock, 1);
	atomic_fetch_add_explicit(&q->mapping.ref, 1, memory_order_relaxed);
	struct _vqueue_shmem_region* region = (struct _vqueue_shmem_region*)(q->mapping.ptr<<6);
	size_t sz = _vqueue_uncompress_size(q->mapping.size_packed);
	lock_release(&q->mapping_lock, 1);
	return (struct _vqueue_mapping){q, region, sz};
}

static inline struct _vqueue_mapping _vqueue_acquire_mapping_for(struct _vqueue* q, void* thing){
	lock_acquire(&q->mapping_lock, 1);
	struct _vqueue_mapping_descriptor* v = &q->mapping;
	next:
	char* region = (char*)(v->ptr<<6);
	uint64_t sz = _vqueue_uncompress_size(v->size_packed);
	if(thing >= region && thing < region + sz){
		lock_release(&q->mapping_lock, 1);
		return (struct _vqueue_mapping){q, (struct _vqueue_mapping_descriptor*)region, sz};
	}
	v = v->next;
	if(v) goto next;
	else{
		lock_release(&q->mapping_lock, 1);
		return (struct _vqueue_mapping){q, 0, 0};
	}
}

static inline void _vqueue_release_mapping(struct _vqueue_mapping ctx){
	lock_acquire(&ctx.q->mapping_lock, 1);
	uint64_t ptr1 = (uint64_t)ctx.data8 >> 6;
	if(ctx.q->mapping.ptr == ptr1){
		atomic_fetch_sub_explicit(&ctx.q->mapping.ref, 1, memory_order_relaxed);
		lock_release(&ctx.q->mapping_lock, 1);
		return;
	}
	struct _vqueue_mapping_descriptor* v = ctx.q->mapping.next;
	while(v){
		if(v->ptr != ptr1){ v = v->next; continue; }
		
		bool finished = atomic_fetch_sub_explicit(&ctx.q->mapping.ref, 1, memory_order_relaxed) == 1;
		lock_release(&ctx.q->mapping_lock, 1);
		if(finished){
			munmap(ctx.data8, ctx.size);
			lock_acquire(&ctx.q->mapping_lock, LOCK_MAX);
			struct _vqueue_mapping_descriptor** ov2 = &ctx.q->mapping.next, *v2 = *ov2;
			while(v2){
				if(v == v2){ *ov2 = v->next; break; }
				v2 = *(ov2 = &v2->next);
			}
			lock_release(&ctx.q->mapping_lock, LOCK_MAX);
			free(v);
		}
		return;
	}
}

static inline void _vqueue_resize_mapping(struct _vqueue_mapping* ctx, uint64_t newsize){
	struct _vqueue* q = ctx->q;
	uint64_t old1 = (uint64_t)ctx->data >> 6;
	uint64_t size_packed = _vqueue_compress_size(newsize);
	newsize = _vqueue_uncompress_size(size_packed);
	lock_acquire(&q->mapping_lock, LOCK_MAX);
	if(q->mapping.size_packed >= size_packed){
		ctx->size = _vqueue_uncompress_size(q->mapping.size_packed);
		ctx->data8 = (_Atomic uint64_t*)(q->mapping.ptr<<6);
		lock_release(&q->mapping_lock, LOCK_MAX);
		return;
	}
	struct _vqueue_shmem_region* new_map = _vqueue_mmap(q->shmem_fd, 0, newsize);
	if(atomic_load_explicit(&q->mapping.ref, memory_order_relaxed) > 1){
		// mapping still in use
		struct _vqueue_mapping_descriptor* desc = malloc(sizeof(struct _vqueue_mapping_descriptor));
		atomic_init(&desc->ref, 0);
		desc->next = q->mapping.next;
		desc->ptr = q->mapping.ptr; desc->size_packed = q->mapping.size_packed;
		q->mapping.next = desc;
		atomic_init(&q->mapping.ref, 1);
	}else if(old1 == q->mapping.ptr){
		munmap(ctx->data8, ctx->size);
	}else{
		struct _vqueue_mapping_descriptor** ov = &q->mapping.next, *v = *ov;
		while(v){
			if(v->ptr != old1){ v = *(ov = &v->next); continue; }
			if(atomic_fetch_sub_explicit(&q->mapping.ref, 1, memory_order_relaxed) == 1){
				munmap(ctx->data8, ctx->size);
				*ov = v->next;
				free(v);
			}
			break;
		}
	}
	q->mapping.ptr = (uint64_t)new_map >> 6;
	q->mapping.size_packed = size_packed;
	lock_release(&q->mapping_lock, LOCK_MAX);
	ctx->size = newsize;
	ctx->data8 = (_Atomic uint64_t*)new_map;
}

bool vqueue_open(vqueue_t* q, const char* name, size_t name_sz){
	if(name_sz == (size_t)-1) name_sz = strlen(name);
	if(name_sz > NAME_MAX) name_sz = NAME_MAX;
	struct _vqueue* q = malloc(sizeof(struct _vqueue));
	// NAME_MAX is pretty small so alloca() is okay
#ifdef __APPLE__
	char* path = alloca(NAME_MAX + 10);
	memcpy(path+9, name, name_sz);
	memcpy(path, "/tmp/shm/", 9);
	path[name_sz+9] = '\0';
	q->shmem_fd = open(path, O_RDWR | O_CREAT, 0666);
	if(q->shmem_fd < 0){
		if(mkdir("/tmp/shm/", 01777) && errno != EEXIST) return false;
		q->shmem_fd = open(path, O_RDWR | O_CREAT, 0666);
		if(q->shmem_fd < 0) return false;
	}
#else
	char* path = alloca(NAME_MAX + 2);
	memcpy(path+1, name, name_sz);
	path[0] = '/'; path[name_sz+1] = '\0';
	q->shmem_fd = shm_open(path, O_RDWR | O_CREAT, 0666);
#endif
	q->mapping.size_packed = _vqueue_compress_size(_VQUEUE_INIT_MAPPING);
	uint64_t sz = _vqueue_uncompress_size(q->mapping.size_packed);
	struct _vqueue_shmem_region* mapping = _vqueue_mmap(q->shmem_fd, 0, sz);
	q->mapping.ptr = (uint64_t)mapping>>6;
	atomic_init(&q->mapping.ref, 0);
	q->mapping.next = 0;
	obtain_id: {
		q->aid = atomic_fetch_add_explicit(&mapping->open_counter, 1, memory_order_relaxed)&0xFFFFFF;
		struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = q->aid, .l_len = 1 };
		if(fcntl(q->shmem_fd, F_SETLK, &l)){
			if(errno == EACCES || errno == EAGAIN) goto obtain_id;
			munmap(mapping, sz);
			close(q->shmem_fd);
			return false;
		}
	}
	atomic_init(&q->mapping_lock, LOCK_MAX);
	return true;
}
void vqueue_close(vqueue_t* q){
	struct _vqueue_mapping_descriptor* v = q->mapping.next;
	while(v){
		struct _vqueue_mapping_descriptor* v2 = v->next;
		munmap((void*)(v->ptr<<6), _vqueue_uncompress_size(v->size_packed));
		free(v); v = v2;
	}
	munmap((void*)(q->mapping.ptr<<6), _vqueue_uncompress_size(q->mapping.size_packed));
	close(q->shmem_fd); // clears locks
}
bool vqueue_unlink(const char* name, size_t name_sz){
	if(name_sz == (size_t)-1) name_sz = strlen(name);
	if(name_sz > NAME_MAX) name_sz = NAME_MAX;
	// NAME_MAX is pretty small so alloca() is okay
#ifdef __APPLE__
	char* path = alloca(NAME_MAX + 10);
	memcpy(path+9, name, name_sz);
	memcpy(path, "/tmp/shm/", 9);
	path[name_sz+9] = '\0';
	return !unlink(path);
#else
	char* path = alloca(NAME_MAX + 2);
	memcpy(path+1, name, name_sz);
	path[0] = '/'; path[name_sz+1] = '\0';
	return !shm_unlink(path);
#endif
}

static inline void _vqueue_finish_pending_alloc(struct _vqueue_mapping* ctx, _Atomic uint64_t rp, _Atomic uint64_t rip, uint64_t* v_){
	retry:
	if(*v_ == 0xFFFFFFFFFF){
		uint8_t n = _vqueue_protect(ctx, 0);
		if((*v_ = atomic_load_explicit(ctx->data8+rp, memory_order_relaxed)) == 0xFFFFFFFFFF){
			atomic_store_explicit(&ctx->data->left, 0, memory_order_release);
			atomic_compare_exchange_strong_explicit(ctx->data8+rp, &v_, 0, memory_order_acq_rel, memory_order_acquire);
			// todo: this is not valid
			atomic_store_explicit(ctx->data8+rip, 0, memory_order_release);
		}
		_vqueue_unprotect(ctx, 0, n);
	}
	uint64_t rinit = atomic_load_explicit(ctx->data8+rip, memory_order_relaxed);
	uint64_t vm = *v_&0xFFFFFFFFFF;
	if(vm > rinit){
		retry2:
		uint8_t n = _vqueue_protect(ctx, rinit);
		uint64_t rinit2 = rinit; rinit = atomic_load_explicit(ctx->data8+rip, memory_order_relaxed);
		if(rinit != rinit2){
			_vqueue_unprotect(ctx, rinit2, n);
			goto retry2;
		}
		uint64_t expected_mapping_size = (char*)&ctx->data->blocks[rinit] - (char*)ctx->data;
		if(expected_mapping_size > ctx->size)
			_vqueue_resize_mapping(ctx, expected_mapping_size);
		struct _vqueue_msg_hdr* hdr = (struct _vqueue_msg_hdr*)&ctx->data->blocks[rinit] - 1;
		uint32_t aid2 = atomic_load_explicit(&hdr->aid, memory_order_relaxed);
		while(aid2 != 0xFFFFFFFF && !atomic_compare_exchange_weak_explicit(&hdr->aid, &aid2, vm>>40, memory_order_relaxed, memory_order_relaxed));
		// safe init that writes what gc pass would need but yields to the more precise value written by the actual allocating thread
		size_t sz2 = vm - rinit - _VQUEUE_MSG_HDR_SIZE - 63, sz = atomic_load_explicit(&hdr->size, memory_order_relaxed);
		while(((sz - sz2)>>6) && !atomic_compare_exchange_weak_explicit(&hdr->size, &sz, sz2, memory_order_relaxed, memory_order_relaxed));

		if(atomic_compare_exchange_strong_explicit(ctx->data8+rip, &rinit, vm, memory_order_relaxed, memory_order_relaxed)
			&& !atomic_compare_exchange_strong_explicit(ctx->data8+rp, v_, vm, memory_order_release, memory_order_acquire)){
			_vqueue_unprotect(ctx, rinit, n);
			goto retry;
		}
		_vqueue_unprotect(ctx, rinit, n);
	}
}

static uint8_t* _vqueue_try_alloc(struct _vqueue_mapping* ctx, _Atomic uint64_t* rp, _Atomic uint64_t* rip, uint64_t blocks, uint64_t size, uint64_t v){
	bool protect_left = false;
	uint8_t n; uint64_t left;
	retry:
	_vqueue_finish_pending_alloc(ctx, rp, rip, &v);
	if(!protect_left){
		n = _vqueue_protect(ctx, v);
		left = atomic_load_explicit(&ctx->data->left, memory_order_relaxed);
	}
	if(v <= left && v + blocks > left){
		// dummy allocation to fill the gap
		if(!protect_left){
			protect_left = true;
			_vqueue_unprotect(ctx, v, n);
			left = _vqueue_pointer_acquire(ctx, _vqueue_offsetof(left), &n);
			goto retry;
		}
		if(v < left){
			uint64_t v3 = left | ctx->q->aid<<40;
			if(!atomic_compare_exchange_strong_explicit(rp, &v, v3, memory_order_acq_rel, memory_order_acquire)){
				_vqueue_unprotect(ctx, left, n);
				goto retry;
			}
			uint64_t expected_mapping_size = (char*)&ctx->data->blocks[left] - (char*)ctx->data;
			if(expected_mapping_size > ctx->size)
				_vqueue_resize_mapping(ctx, expected_mapping_size);
			
			struct _vqueue_msg_hdr* hdr = (struct _vqueue_msg_hdr*)&ctx->data->blocks[v] - 1;
			atomic_store_explicit(&hdr->next, 0xFFFFFFFFFF, memory_order_relaxed);
			atomic_store_explicit(&hdr->aid, 0xFFFFFFFF, memory_order_relaxed);
			atomic_store_explicit(&hdr->size, size, memory_order_relaxed);

			atomic_compare_exchange_strong_explicit(rip, &v, left, memory_order_relaxed, memory_order_relaxed);
		}

		// Carefully ordered wrap
		atomic_store_explicit(&ctx->data->left, 0, memory_order_release);
		// reader reading right<rinit is inconsequential
		// we override rp which is limit and cannot have changed, and rip which can be {old, limit} either of which is fine
		atomic_store_explicit(rp, 0, memory_order_release);
		// todo: this is not valid
		atomic_store_explicit(rip, 0, memory_order_release);
		_vqueue_unprotect(ctx, left, n);
		v = 0;
		if(_vqueue_trim(ctx, 0, false)){
			protect_left = false;
			goto retry;
		}
		return 0;
	}
	uint64_t v2 = v + blocks, v3 = v2 | ctx->q->aid<<40;
	if(!atomic_compare_exchange_strong_explicit(rp, &v, v3, memory_order_acq_rel, memory_order_acquire)){
		_vqueue_unprotect(ctx, protect_left ? left : v2 - blocks, n);
		goto retry;
	}
	uint64_t expected_mapping_size = (char*)&ctx->data->blocks[v2] - (char*)ctx->data;
	if(expected_mapping_size > ctx->size)
		_vqueue_resize_mapping(ctx, expected_mapping_size);
	
	struct _vqueue_msg_hdr* hdr = (struct _vqueue_msg_hdr*)&ctx->data->blocks[v] - 1;
	atomic_store_explicit(&hdr->next, 0xFFFFFFFFFF, memory_order_relaxed);
	atomic_store_explicit(&hdr->aid, ctx->q->aid, memory_order_relaxed);
	atomic_store_explicit(&hdr->size, size, memory_order_relaxed);

	if(atomic_compare_exchange_strong_explicit(rip, &v, v2, memory_order_relaxed, memory_order_relaxed))
		atomic_compare_exchange_strong_explicit(rp, &v3, v2, memory_order_release, memory_order_relaxed);
	
	_vqueue_unprotect(ctx, protect_left ? left : v2 - blocks, n);
	return (uint8_t*)&ctx->data->blocks[v];
}

vqueue_block_t vqueue_alloc(vqueue_t* q, size_t size){
	uint64_t blocks = (size+_VQUEUE_MSG_HDR_SIZE + 63) >> 6;
	struct _vqueue_mapping ctx = _vqueue_acquire_mapping(q);
	retry:
	uint64_t r1 = atomic_load_explicit(&ctx.data->right1, memory_order_relaxed), r1m = r1&0xFFFFFFFFFF;
	uint64_t r2 = atomic_load_explicit(&ctx.data->right2, memory_order_relaxed), r2m = r2&0xFFFFFFFFFF;
	atomic_thread_fence(memory_order_acquire);
	if(r2m > r1m){
		uint8_t* p = _vqueue_try_alloc(&ctx, _vqueue_offsetof(right1), _vqueue_offsetof(right1init), blocks, size, r1);
		if(p) return (vqueue_block_t){size, p};
	}
	uint8_t* p = _vqueue_try_alloc(&ctx, _vqueue_offsetof(right2), _vqueue_offsetof(right2init), blocks, size, r2);
	if(p) return (vqueue_block_t){size, p};
	if(r2m < r1m){
		uint8_t* p = _vqueue_try_alloc(&ctx, _vqueue_offsetof(right1), _vqueue_offsetof(right1init), blocks, size, r1);
		if(p) return (vqueue_block_t){size, p};
	}
	goto retry;
}
void vqueue_post(vqueue_t* q, vqueue_block_t block){
	struct _vqueue_mapping ctx = _vqueue_acquire_mapping_for(q, block.data);
	uint64_t new_ptr = ((char(*)[64])block.data - ctx.data->blocks) | _vqueue_compress_size(block.size)<<40;

	retry: {}
	uint64_t ptr = atomic_load_explicit(&ctx.data->tail, memory_order_relaxed)&0xFFFFFFFFFF, vo;
	retry2:
	if(ptr == 0xFFFFFFFFFF){
		uint64_t expected = 0xFFFFFFFFFF;
		if(!atomic_compare_exchange_weak_explicit(&ctx.data->head, &expected, new_ptr, memory_order_release, memory_order_relaxed)) goto retry;
		else{
			sem_post(&ctx.data->sema4);
			goto end;
		}
	}
	uint8_t n = _vqueue_protect(&ctx, ptr);
	uint64_t ptr2 = atomic_load_explicit(&ctx.data->tail, memory_order_acquire);
	if(ptr2 != ptr){
		_vqueue_unprotect(&ctx, ptr, n);
		goto retry2;
	}
	struct _vqueue_msg_hdr* hdr = ((struct _vqueue_msg_hdr*) &ctx.data->blocks[ptr]) - 1;
	uint64_t actual = 0xFFFFFFFFFF;
	if(!atomic_compare_exchange_strong_explicit(&hdr->next, &actual, new_ptr, memory_order_release, memory_order_relaxed)){
		uint64_t ptr2 = ptr;
		bool xchg_success = atomic_compare_exchange_strong_explicit(&ctx.data->tail, &ptr2, actual, memory_order_release, memory_order_relaxed);
		_vqueue_unprotect(&ctx, ptr, n);
		if(xchg_success){
			ptr = actual&0xFFFFFFFFFF;
			goto retry2;
		}else goto retry;
	}
	_vqueue_unprotect(&ctx, ptr, n);
	// if it fails another thread got it anyway
	atomic_compare_exchange_strong_explicit(&ctx.data->tail, &ptr, new_ptr, memory_order_release, memory_order_relaxed);
	end:
	_vqueue_release_mapping(ctx);
}

vqueue_block_t vqueue_wait(vqueue_t* q){
	struct _vqueue_mapping ctx = _vqueue_acquire_mapping(q);
	retry: {}
	uint8_t n;
	uint64_t ptr = _vqueue_pointer_acquire(&ctx, _vqueue_offsetof(head), &n);
	find_next:
	if((ptr&0xFFFFFFFFFF) == 0xFFFFFFFFFF){
		// No messages. Block
		_vqueue_unprotect(&ctx, ptr, n);
		sem_wait(&ctx.data->sema4);
		goto retry;
	}
	size_t expected_mapping_size = ((char*)&ctx.data->blocks[ptr] - (char*)ctx.data) + _vqueue_uncompress_size(ptr>>40);
	if(ctx.size < expected_mapping_size) _vqueue_resize_mapping(&ctx, expected_mapping_size);

	struct _vqueue_msg_hdr* hdr = ((struct _vqueue_msg_hdr*) &ctx.data->blocks[ptr&0xFFFFFFFFFF]) - 1;
	uint32_t v = atomic_load_explicit(&hdr->aid, memory_order_relaxed);
	uint64_t ptr2 = atomic_load_explicit(&hdr->next, memory_order_relaxed);
	retry:
	if(v >> 24){
		// Someone else acquired it, we find next message
		_vqueue_unprotect(&ctx, ptr&0xFFFFFFFFFF, n);
		n = _vqueue_protect(&ctx, ptr2);
		if(atomic_compare_exchange_strong_explicit(&ctx.data->head, &ptr, ptr2, memory_order_relaxed, memory_order_relaxed)){
			ptr = ptr2;
		}else{
			_vqueue_unprotect(&ctx, ptr2, n);
			ptr = _vqueue_pointer_acquire2(&ctx, _vqueue_offsetof(head), ptr, &n);
		}
		goto find_next;
	}
	hdr = ((struct _vqueue_msg_hdr*) &ctx.data->blocks[ptr&0xFFFFFFFFFF]) - 1;
	if(!atomic_compare_exchange_strong_explicit(&hdr->aid, &v, q->aid|0x1000000, memory_order_relaxed, memory_order_relaxed)) goto retry;
	atomic_compare_exchange_strong_explicit(&ctx.data->head, &ptr, ptr2, memory_order_release, memory_order_relaxed);
	_vqueue_unprotect(&ctx, ptr&0xFFFFFFFFFF, n);
	return (vqueue_block_t){atomic_load_explicit(&hdr->size, memory_order_relaxed), hdr->payload};
}

static inline bool _vqueue_trim(struct _vqueue_mapping* ctx, uint64_t left, bool strict){
	if(strict){
		struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_len = 1 };
		for(unsigned i = 0; i < 256; i++){
			uint64_t v = atomic_load_explicit(&ctx->data->hazfield[i], memory_order_relaxed);
			if(!v) continue;
			l.l_start = (uint32_t)(~v>>40);
			if(!fcntl(ctx->q->shmem_fd, F_GETLK, &l) && l.l_type == F_UNLCK){
				atomic_compare_exchange_strong_explicit(&ctx->data->hazfield[i], &v, 0, memory_order_relaxed, memory_order_relaxed);
			}
		}
	}
	uint32_t actual = 0xFFFFFFFF;
	acq:
	if(!atomic_compare_exchange_strong_explicit(&ctx->data->trim_lock, &actual, ctx->q->aid, memory_order_acquire, memory_order_relaxed)){
		if(strict){
			struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = actual, .l_len = 1};
			if(!fcntl(ctx->q->shmem_fd, F_GETLK, &l) && l.l_type == F_UNLCK)
				goto acq;
		}
		return false;
	}
	bool cleaned = false;
	uint64_t r1 = atomic_load_explicit(&ctx->data->right1, memory_order_relaxed);
	uint64_t r2 = atomic_load_explicit(&ctx->data->right2, memory_order_relaxed);
	_vqueue_finish_pending_alloc(ctx, _vqueue_offsetof(right1), _vqueue_offsetof(right1init), &r1);
	_vqueue_finish_pending_alloc(ctx, _vqueue_offsetof(right2), _vqueue_offsetof(right2init), &r2);
	r1 &= 0xFFFFFFFFFF; r2 &= 0xFFFFFFFFFF;
	uint64_t rmax = r2 >= r1 ? r2 : r1;
	uint64_t right = r2 >= r1 ? _vqueue_offsetof(right2) : _vqueue_offsetof(right1);
	while(true){
		struct _vqueue_msg_hdr* hdr = ((struct _vqueue_msg_hdr*) &ctx->data->blocks[left]) - 1;
		uint32_t aid = atomic_load_explicit(&hdr->aid, memory_order_relaxed);
		if(strict && aid != -1u){
			struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = aid, .l_len = 1};
			if(!fcntl(ctx->q->shmem_fd, F_GETLK, &l) && l.l_type == F_UNLCK) aid = -1u;
		}
		if(aid == -1u && _vqueue_check(ctx, left)){
			uint64_t next = left + atomic_load_explicit(&hdr->size, memory_order_relaxed) + _VQUEUE_MSG_HDR_SIZE;
			next += (-next)&63;
			cleaned = true;
			recheck:
			if(next == rmax){
				if(!atomic_compare_exchange_strong_explicit(ctx->data8+right, &rmax, 0xFFFFFFFFFF, memory_order_relaxed, memory_order_relaxed)){
					// update max
					_vqueue_finish_pending_alloc(ctx, right, right == _vqueue_offsetof(right1) ? _vqueue_offsetof(right1init) : _vqueue_offsetof(right2init), &rmax);
					rmax &= 0xFFFFFFFFFF;
					if(right == _vqueue_offsetof(right1)) r1 = rmax; else r2 = rmax;
					rmax = r2 >= r1 ? r2 : r1;
					right = rmax == r2 ? _vqueue_offsetof(right2) : _vqueue_offsetof(right1);
					goto recheck;
				}
				// cmpxchg succeeded
				atomic_store_explicit(right == _vqueue_offsetof(right1) ? &ctx->data->right1init : &ctx->data->right2init, 0, memory_order_release);
				if(atomic_compare_exchange_strong_explicit(&ctx->data->left, &left, 0, memory_order_release, memory_order_relaxed))
					left = 0;
				rmax = 0xFFFFFFFFFF;
				atomic_compare_exchange_strong_explicit(ctx->data8+right, &rmax, 0, memory_order_release, memory_order_relaxed);
				if(right == _vqueue_offsetof(right1)){ r1 = 0; rmax = r2; }else{ r2 = 0; rmax = r1; }
				right = rmax == r2 ? _vqueue_offsetof(right2) : _vqueue_offsetof(right1);
			}else{
				if(atomic_compare_exchange_strong_explicit(&ctx->data->left, &left, next, memory_order_relaxed, memory_order_relaxed))
					left = next;
			}
		}else break;
	}
	atomic_store_explicit(&ctx->data->trim_lock, -1u, memory_order_release);
	return cleaned;
}

void vqueue_free(vqueue_t* q, vqueue_block_t block){
	struct _vqueue_mapping ctx = _vqueue_acquire_mapping_for(q, block.data);
	uint64_t ptr = (char(*)[64])block.data - ctx.data->blocks;
	struct _vqueue_msg_hdr* hdr = ((struct _vqueue_msg_hdr*) block.data) - 1;
	atomic_store_explicit(&hdr->aid, -1u, memory_order_relaxed);
	if(atomic_load_explicit(&ctx.data->left, memory_order_acquire) == ptr)
		_vqueue_trim(&ctx, ptr, false);
	_vqueue_release_mapping(ctx);
}
