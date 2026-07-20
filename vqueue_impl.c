#pragma once
#define _FILE_OFFSET_BITS 64
#include <vqueue.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "defs.h"

#define _VQUEUE_ERR_MSG "vqueue: mmap(): failed\nThis is likely an out-of-memory exception and is FATAL\n"
#ifndef _WIN32
#include <sys/mman.h>
#include <errno.h>
#include <sys/stat.h>
static inline void* _vqueue_mmap(int shm_fd, off_t off, size_t sz){
	void* addr = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, off);
	if(addr == (void*)-1){ write(STDERR_FILENO, _VQUEUE_ERR_MSG, sizeof(_VQUEUE_ERR_MSG)); abort(); }
	return addr;
}
#else
static inline void* _vqueue_mmap(HANDLE shm_mp, uint64_t off, size_t sz){
	DWORD off_high = (DWORD)(off >> 32), off_low  = (DWORD)(off & 0xffffffff);
	void* addr = MapViewOfFile(shm_mp, FILE_MAP_READ | FILE_MAP_WRITE, off_high, off_low, sz);
	if(!addr){ write(STDERR_FILENO, _VQUEUE_ERR_MSG, sizeof(_VQUEUE_ERR_MSG)); abort(); }
	return addr;
}
#endif

static inline struct _vqueue_mapping _vqueue_acquire_mapping(struct _vqueue* q){
	lock_acquire(&q->mapping_lock, 1);
	atomic_fetch_add_explicit(&q->mapping.ref, 1, memory_order_relaxed);
	struct _vqueue_shmem_region* region = (struct _vqueue_shmem_region*)(q->mapping.ptr<<6);
	size_t sz = _vqueue_uncompress_size(q->mapping.size_packed);
	lock_release(&q->mapping_lock, 1);
	return (struct _vqueue_mapping){q, region, sz};
}

static inline struct _vqueue_mapping _vqueue_acquire_mapping_for(struct _vqueue* q, char* thing){
	lock_acquire(&q->mapping_lock, 1);
	struct _vqueue_mapping_descriptor* v = &q->mapping;
	next: {}
	char* region = (char*)(v->ptr<<6);
	uint64_t sz = _vqueue_uncompress_size(v->size_packed);
	if(thing >= region && thing < region + sz){
		lock_release(&q->mapping_lock, 1);
		return (struct _vqueue_mapping){q, (struct _vqueue_shmem_region*)region, sz};
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
			munmap(ctx.data8, _vqueue_uncompress_size(ctx.size));
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

static void _vqueue_resize_mapping(struct _vqueue_mapping* ctx, uint8_t size_packed){
	struct _vqueue* q = ctx->q;
	uint64_t old1 = (uint64_t)ctx->data >> 6;
	uint64_t newsize = _vqueue_uncompress_size(size_packed);
	lock_acquire(&q->mapping_lock, LOCK_MAX);
	if(q->mapping.size_packed >= size_packed){
		ctx->size = q->mapping.size_packed;
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
		munmap(ctx->data8, _vqueue_uncompress_size(ctx->size));
	}else{
		struct _vqueue_mapping_descriptor** ov = &q->mapping.next, *v = *ov;
		while(v){
			if(v->ptr != old1){ v = *(ov = &v->next); continue; }
			if(atomic_fetch_sub_explicit(&q->mapping.ref, 1, memory_order_relaxed) == 1){
				munmap(ctx->data8, _vqueue_uncompress_size(ctx->size));
				*ov = v->next;
				free(v);
			}
			break;
		}
	}
	q->mapping.ptr = (uint64_t)new_map >> 6;
	q->mapping.size_packed = size_packed;
	lock_release(&q->mapping_lock, LOCK_MAX);
	ctx->size = size_packed;
	ctx->data8 = (_Atomic uint64_t*)new_map;
}

bool vqueue_open(vqueue_t* q, const char* name, size_t name_sz){
	if(name_sz == (size_t)-1) name_sz = strlen(name);
	if(name_sz > NAME_MAX) name_sz = NAME_MAX;
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
	struct stat st;
	fstat(q->shmem_fd, &st);
	struct _vqueue_shmem_region* mapping = _vqueue_mmap(q->shmem_fd, 0, sz);
	if(st.st_size < sizeof(struct _vqueue_shmem_region) || !atomic_load_explicit(&mapping->lsize, memory_order_acquire)){
		struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0x1000000, .l_len = 1 };
		if(fcntl(q->shmem_fd, F_SETLKW, &l)) goto err;
		if(fstat(q->shmem_fd, &st)) goto err;
		if(st.st_size < sizeof(struct _vqueue_shmem_region)){
			if(ftruncate(q->shmem_fd, sz)) goto err;
			atomic_init(&mapping->head, _VQUEUE_PTR_INVALID);
			atomic_init(&mapping->tail, _VQUEUE_PTR_INVALID);
			atomic_init(&mapping->trim_lock, 0xFFFFFFFF);
			atomic_thread_fence(memory_order_release);
			atomic_init(&mapping->lsize, sz);
		}
		l.l_type = F_UNLCK;
		if(fcntl(q->shmem_fd, F_SETLKW, &l)) goto err;
	}
	q->mapping.ptr = (uint64_t)mapping>>6;
	atomic_init(&q->mapping.ref, 0);
	q->mapping.next = 0;
	obtain_id: {
		q->aid = atomic_fetch_add_explicit(&mapping->open_counter, 1, memory_order_relaxed)&0xFFFFFF;
		struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = q->aid, .l_len = 1 };
		if(fcntl(q->shmem_fd, F_SETLK, &l)){
			if(errno == EACCES || errno == EAGAIN) goto obtain_id;
			munmap(mapping, sz);
			err:
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

static inline void _vqueue_finish_pending_alloc(struct _vqueue_mapping* ctx, uint64_t rp, uint64_t rip, uint64_t* v_){
	retry: {}
	uint64_t vm = *v_&0xFFFFFFFFFF;
	uint64_t rinit = atomic_load_explicit(ctx->data8+rip, memory_order_relaxed);
	if(vm == _VQUEUE_PTR_INVALID){
		uint8_t n = _vqueue_protect(ctx, 0);
		vm = (*v_ = atomic_load_explicit(ctx->data8+rp, memory_order_relaxed)) & 0xFFFFFFFFFF;
		retry1:
		if(vm == _VQUEUE_PTR_INVALID){
			if(!(*v_ >> 48)){
				uint64_t expected_left = rinit;
				if(!atomic_compare_exchange_strong_explicit(&ctx->data->left, &expected_left, 0, memory_order_relaxed, memory_order_relaxed)){
					if(atomic_compare_exchange_strong_explicit(ctx->data8+rp, v_, rinit, memory_order_relaxed, memory_order_relaxed))
						*v_ = rinit;
					vm = *v_ & 0xFFFFFFFFFF;
					goto retry1;
				}
			}else atomic_store_explicit(&ctx->data->left, 0, memory_order_relaxed);
			atomic_store_explicit(ctx->data8+rip, 0, memory_order_release);
			if(atomic_compare_exchange_strong_explicit(ctx->data8+rp, v_, 0, memory_order_release, memory_order_relaxed)) *v_ = vm = 0;
			else goto retry1;
		}
		_vqueue_unprotect(ctx, 0, n);
	}
	if(vm > rinit){
		retry2: {}
		uint8_t n = _vqueue_protect(ctx, rinit);
		uint64_t rinit2 = rinit; rinit = atomic_load_explicit(ctx->data8+rip, memory_order_relaxed);
		if(rinit != rinit2){
			_vqueue_unprotect(ctx, rinit2, n);
			goto retry2;
		}
		uint64_t v0 = vm;
		vm = (*v_ = atomic_load_explicit(ctx->data8+rp, memory_order_acquire)) & 0xFFFFFFFFFF;
		if(v0 != vm){
			_vqueue_unprotect(ctx, rinit2, n);
			goto retry;
		}
		uint64_t emsz = _vqueue_compress_size((char*)&ctx->data->blocks[rinit] - (char*)ctx->data);
		if(emsz > ctx->size) _vqueue_resize_mapping(ctx, emsz);
		struct _vqueue_msg_hdr* hdr = (struct _vqueue_msg_hdr*)&ctx->data->blocks[rinit] - 1;
		uint32_t aid2 = atomic_load_explicit(&hdr->aid, memory_order_relaxed);
		while(aid2 != 0xFFFFFFFF && !atomic_compare_exchange_weak_explicit(&hdr->aid, &aid2, *v_>>40, memory_order_relaxed, memory_order_relaxed));
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

static uint8_t* _vqueue_try_alloc(struct _vqueue_mapping* ctx, uint64_t rp, uint64_t rip, uint64_t blocks, uint64_t size, uint64_t v){
	uint8_t n; 
	retry:
	_vqueue_finish_pending_alloc(ctx, rp, rip, &v);
	uint64_t left = atomic_load_explicit(&ctx->data->left, memory_order_relaxed);
	retry2:
	if(!v && left == 0 && rp == _vqueue_offsetof(right2)){
		n = _vqueue_protect(ctx, v);
		if((left = atomic_load_explicit(&ctx->data->left, memory_order_relaxed)) == 0){
			goto alloc;
		}
		_vqueue_unprotect(ctx, v, n);
	}
	if(v <= left && v + blocks > left){
		// dummy allocation to fill the gap
		if(v < left){
			uint64_t v3 = left | (uint64_t)ctx->q->aid<<40;
			if(!atomic_compare_exchange_strong_explicit(ctx->data8+rp, &v, v3, memory_order_acq_rel, memory_order_acquire)){
				_vqueue_unprotect(ctx, left, n);
				goto retry;
			}
			uint64_t emsz = _vqueue_compress_size((char*)&ctx->data->blocks[left] - (char*)ctx->data);
			if(emsz > ctx->size) _vqueue_resize_mapping(ctx, emsz);
			
			struct _vqueue_msg_hdr* hdr = (struct _vqueue_msg_hdr*)&ctx->data->blocks[v] - 1;
			atomic_store_explicit(&hdr->next, _VQUEUE_PTR_INVALID, memory_order_relaxed);
			atomic_store_explicit(&hdr->aid, 0xFFFFFFFF, memory_order_relaxed);
			atomic_store_explicit(&hdr->size, size, memory_order_relaxed);
			uint64_t v2 = v;
			atomic_compare_exchange_strong_explicit(ctx->data8+rip, &v2, left, memory_order_relaxed, memory_order_relaxed);
		}
		uint8_t n = _vqueue_protect(ctx, 0);
		uint64_t mark = _VQUEUE_PTR_INVALID|(uint64_t)n<<40;
		if(!atomic_compare_exchange_strong_explicit(ctx->data8+rp, &v, mark, memory_order_acquire, memory_order_relaxed))
			goto retry;

		if(!atomic_compare_exchange_strong_explicit(&ctx->data->left, &left, 0, memory_order_release, memory_order_relaxed)){
			atomic_compare_exchange_strong_explicit(ctx->data8+rp, &mark, v, memory_order_relaxed, memory_order_relaxed);
			_vqueue_unprotect(ctx, 0, n);
			goto retry2;
		}
		atomic_store_explicit(ctx->data8+rip, 0, memory_order_release);

		atomic_compare_exchange_strong_explicit(ctx->data8+rp, &mark, 0, memory_order_release, memory_order_relaxed);
		_vqueue_unprotect(ctx, 0, n);
		return 0;
	}
	n = _vqueue_protect(ctx, v);
	alloc: {}
	uint64_t v2 = v + blocks, v3 = v2 | (uint64_t)ctx->q->aid<<40;
	if(!atomic_compare_exchange_strong_explicit(ctx->data8+rp, &v, v3, memory_order_acq_rel, memory_order_acquire)){
		_vqueue_unprotect(ctx, v2 - blocks, n);
		goto retry;
	}
	uint8_t emsz = _vqueue_compress_size((char*)&ctx->data->blocks[v2] - (char*)ctx->data);
	uint64_t expected_mapping_size = _vqueue_uncompress_size(emsz);
	//thread_memory_barrier(mb_co_acquire); // the cmpxchg already gave acquire
	if(atomic_load_explicit(&ctx->data->lsize, memory_order_acquire) < expected_mapping_size){
		int fd = ctx->q->shmem_fd;
		struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0x1000000, .l_len = 1 };
		if(fcntl(fd, F_SETLKW, &l)) err: abort();
		struct stat st;
		if(fstat(fd, &st)) goto err;
		if(st.st_size < expected_mapping_size) if(ftruncate(fd, expected_mapping_size)) goto err;
		l.l_type = F_UNLCK;
		atomic_store_explicit(&ctx->data->lsize, expected_mapping_size, memory_order_release);
		if(fcntl(fd, F_SETLKW, &l)) goto err;
	}
	if(emsz > ctx->size) _vqueue_resize_mapping(ctx, emsz);
	
	struct _vqueue_msg_hdr* hdr = (struct _vqueue_msg_hdr*)&ctx->data->blocks[v] - 1;
	atomic_store_explicit(&hdr->next, _VQUEUE_PTR_INVALID, memory_order_relaxed);
	atomic_store_explicit(&hdr->aid, ctx->q->aid, memory_order_relaxed);
	atomic_store_explicit(&hdr->size, size, memory_order_relaxed);

	if(atomic_compare_exchange_strong_explicit(ctx->data8+rip, &v, v2, memory_order_relaxed, memory_order_relaxed))
		atomic_compare_exchange_strong_explicit(ctx->data8+rp, &v3, v2, memory_order_release, memory_order_relaxed);
	
	_vqueue_unprotect(ctx, v2 - blocks, n);
	return (uint8_t*)&ctx->data->blocks[v];
}

vqueue_block_t vqueue_alloc(vqueue_t* q, size_t size){
	uint64_t blocks = (size+_VQUEUE_MSG_HDR_SIZE + 63) >> 6;
	struct _vqueue_mapping ctx = _vqueue_acquire_mapping(q);
	retry: {}
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
	struct _vqueue_mapping ctx = _vqueue_acquire_mapping_for(q, (char*)block.data);
	uint64_t new_ptr = ((char(*)[64])block.data - ctx.data->blocks) | (uint64_t)_vqueue_compress_size((char*)(block.data+block.size) - (char*)ctx.data)<<40;
	uint8_t n;
	uint64_t ptr = atomic_load_explicit(&ctx.data->tail, memory_order_relaxed)&0xFFFFFFFFFF;
	retry: {}
	uint64_t ptr2 = ptr;
	if(ptr == _VQUEUE_PTR_INVALID){
		if(!atomic_compare_exchange_weak_explicit(&ctx.data->head, &ptr, new_ptr, memory_order_release, memory_order_relaxed)){
			// Load head, treat is as the new tail
			ptr &= 0xFFFFFFFFFF; ptr2 = ptr;
			n = _vqueue_protect(&ctx, ptr);
			ptr = atomic_load_explicit(&ctx.data->head, memory_order_acquire)&0xFFFFFFFFFF;
			if(ptr2 != ptr){
				_vqueue_unprotect(&ctx, ptr2, n);
				goto retry;
			}
			ptr2 = _VQUEUE_PTR_INVALID;
		}else{
			sem_post(&ctx.data->sema4);
			goto end;
		}
	}else{
		n = _vqueue_protect(&ctx, ptr);
		ptr = atomic_load_explicit(&ctx.data->tail, memory_order_acquire)&0xFFFFFFFFFF;
		if(ptr2 != ptr){
			_vqueue_unprotect(&ctx, ptr2, n);
			goto retry;
		}
	}
	struct _vqueue_msg_hdr* hdr = ((struct _vqueue_msg_hdr*) &ctx.data->blocks[ptr]) - 1;
	uint64_t actual = _VQUEUE_PTR_INVALID;
	if(!atomic_compare_exchange_strong_explicit(&hdr->next, &actual, new_ptr, memory_order_release, memory_order_relaxed)){
		bool xchg_success = atomic_compare_exchange_strong_explicit(&ctx.data->tail, &ptr2, actual, memory_order_release, memory_order_relaxed);
		_vqueue_unprotect(&ctx, ptr, n);
		ptr = xchg_success ? actual : ptr2;
		ptr &= 0xFFFFFFFFFF;
		goto retry;
	}
	_vqueue_unprotect(&ctx, ptr, n);
	// if it fails another thread got it anyway
	atomic_compare_exchange_strong_explicit(&ctx.data->tail, &ptr2, new_ptr, memory_order_release, memory_order_relaxed);
	end:
	_vqueue_release_mapping(ctx);
}

vqueue_block_t vqueue_wait(vqueue_t* q){
	struct _vqueue_mapping ctx = _vqueue_acquire_mapping(q);
	retry: {}
	uint8_t n;
	uint64_t ptr = _vqueue_pointer_acquire(&ctx, _vqueue_offsetof(head), &n);
	find_next:
	if(ptr == _VQUEUE_PTR_INVALID){
		// No messages. Block
		sem_wait(&ctx.data->sema4);
		goto retry;
	}
	size_t emsz = ptr>>40;
	if(emsz > ctx.size) _vqueue_resize_mapping(&ctx, emsz);

	struct _vqueue_msg_hdr* hdr = ((struct _vqueue_msg_hdr*) &ctx.data->blocks[ptr&0xFFFFFFFFFF]) - 1;
	uint32_t v = atomic_load_explicit(&hdr->aid, memory_order_relaxed);
	uint64_t ptr2 = atomic_load_explicit(&hdr->next, memory_order_relaxed);
	retry2:
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
	if(!atomic_compare_exchange_strong_explicit(&hdr->aid, &v, q->aid|0x1000000, memory_order_relaxed, memory_order_relaxed)) goto retry2;
	uint64_t ptr1 = ptr;
	atomic_compare_exchange_strong_explicit(&ctx.data->head, &ptr1, ptr2, memory_order_release, memory_order_relaxed);
	ptr1 = ptr;
	atomic_compare_exchange_strong_explicit(&ctx.data->tail, &ptr1, ptr2, memory_order_relaxed, memory_order_relaxed);
	_vqueue_unprotect(&ctx, ptr&0xFFFFFFFFFF, n);
	return (vqueue_block_t){atomic_load_explicit(&hdr->size, memory_order_relaxed), hdr->payload};
}

static inline bool _vqueue_trim(struct _vqueue_mapping* ctx, uint64_t left, bool strict){
	if(strict){
		struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_len = 1 };
		for(unsigned i = 0; i < 256; i++){
			uint64_t v = atomic_load_explicit(&ctx->data->hazarena[i], memory_order_relaxed);
			if(!v) continue;
			l.l_start = (uint32_t)(~v>>40);
			if(!fcntl(ctx->q->shmem_fd, F_GETLK, &l) && l.l_type == F_UNLCK){
				atomic_compare_exchange_strong_explicit(&ctx->data->hazarena[i], &v, 0, memory_order_relaxed, memory_order_relaxed);
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
	while(left < rmax){
		struct _vqueue_msg_hdr* hdr = ((struct _vqueue_msg_hdr*) &ctx->data->blocks[left]) - 1;
		uint32_t aid = atomic_load_explicit(&hdr->aid, memory_order_relaxed);
		if(strict && aid != 0xFFFFFFFF){
			struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = aid, .l_len = 1};
			if(!fcntl(ctx->q->shmem_fd, F_GETLK, &l) && l.l_type == F_UNLCK) aid = 0xFFFFFFFF;
		}
		if(aid == 0xFFFFFFFF && _vqueue_check(ctx, left)){
			uint64_t next = left + ((atomic_load_explicit(&hdr->size, memory_order_relaxed) + _VQUEUE_MSG_HDR_SIZE + 63) >> 6);
			cleaned = true;
			recheck:
			printf("%llu, %llu\n", next, rmax);
			if(next == rmax){
				uint64_t mark = _VQUEUE_PTR_INVALID|256ull<<40;
				if(!atomic_compare_exchange_strong_explicit(ctx->data8+right, &rmax, mark, memory_order_relaxed, memory_order_relaxed)){
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
				atomic_compare_exchange_strong_explicit(&ctx->data->left, &left, 0, memory_order_release, memory_order_relaxed);
				left = 0;
				atomic_compare_exchange_strong_explicit(ctx->data8+right, &mark, 0, memory_order_release, memory_order_relaxed);
				if(right == _vqueue_offsetof(right1)){ r1 = 0; rmax = r2; }else{ r2 = 0; rmax = r1; }
				right = rmax == r2 ? _vqueue_offsetof(right2) : _vqueue_offsetof(right1);
				if(((char*)ctx->data->blocks[rmax] - (char*)ctx->data) < atomic_load_explicit(&ctx->data->lsize, memory_order_acquire)>>1){
					int fd = ctx->q->shmem_fd;
					struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0x1000000, .l_len = 1 };
					if(!fcntl(fd, F_SETLKW, &l)){
						atomic_store_explicit(&ctx->data->lsize, 0, memory_order_relaxed);
						thread_memory_barrier(mb_co_release);
						uint64_t r1 = atomic_load_explicit(&ctx->data->right1, memory_order_relaxed);
						uint64_t r2 = atomic_load_explicit(&ctx->data->right2, memory_order_relaxed);
						rmax = r2 >= r1 ? r2 : r1;
						right = rmax == r2 ? _vqueue_offsetof(right2) : _vqueue_offsetof(right1);
						uint64_t newsz = _vqueue_round_size((char*)ctx->data->blocks[rmax] - (char*)ctx->data);
						atomic_store_explicit(&ctx->data->lsize, newsz, memory_order_release);
						ftruncate(fd, newsz);
						l.l_type = F_UNLCK;
						if(fcntl(fd, F_SETLKW, &l)) abort();
					}
				}
			}else{
				atomic_compare_exchange_strong_explicit(&ctx->data->left, &left, next, memory_order_relaxed, memory_order_relaxed);
				left = next;
			}
		}else break;
	}
	atomic_store_explicit(&ctx->data->trim_lock, 0xFFFFFFFF, memory_order_release);
	return cleaned;
}

void vqueue_free(vqueue_t* q, vqueue_block_t block){
	struct _vqueue_mapping ctx = _vqueue_acquire_mapping_for(q, (char*)block.data);
	uint64_t ptr = (char(*)[64])block.data - ctx.data->blocks;
	struct _vqueue_msg_hdr* hdr = ((struct _vqueue_msg_hdr*) block.data) - 1;
	atomic_store_explicit(&hdr->aid, 0xFFFFFFFF, memory_order_relaxed);
	if(atomic_load_explicit(&ctx.data->left, memory_order_acquire) == ptr)
		_vqueue_trim(&ctx, ptr, false);
	_vqueue_release_mapping(ctx);
}