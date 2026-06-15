#pragma once
#define _FILE_OFFSET_BITS 64
#include <vqueue.h>
#include <semaphore.h>
#include <limits.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include "util.h"

#define _VQUEUE_INIT_MAPPING 16384

struct _vqueue_msg_hdr{
	// High bits used to indicate whether it has been consumed yet
	_Atomic uint32_t aid;
	size_t size;
	_Atomic uint64_t next;
};
#define _VQUEUE_MSG_HDR_SIZE sizeof(struct _vqueue_msg_hdr)-4

struct _vqueue_shmem_region{
	sem_t sema4;
	_Atomic uint32_t open_counter;
	_Atomic uint64_t head, tailp;
	// 8-way hash table of hazard pointers
	_Atomic uint64_t hazfield[256];
	char unused_[_VQUEUE_MSG_HDR_SIZE];
	_Alignas(64) char blocks[][64];
};

struct _vqueue_mapping_descriptor{
	_Atomic uint64_t ref;
	uint64_t ptr:40, size_packed:24;
	struct _vqueue_mapping_descriptor *next;
};

struct _vqueue{
	int shmem_fd; uint32_t aid;
	lock_t mapping_lock;
	struct _vqueue_mapping_descriptor mapping;
};

static inline void* _vqueue_mmap(int fd, off_t off, size_t sz){
	void* addr = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
#define _VQUEUE_ERR_MSG "vqueue: mmap(): User address space exhausted\nThis is a FATAL out-of-memory exception\n"
	if(addr == (void*)-1){ write(STDERR_FILENO, _VQUEUE_ERR_MSG, sizeof(_VQUEUE_ERR_MSG)); abort(); }
#undef _VQUEUE_ERR_MSG
	return addr;
}

static inline struct _vqueue_shmem_region* _vqueue_acquire_mapping(struct _vqueue* q, size_t* szout){
	lock_acquire(&q->mapping_lock, 1);
	atomic_fetch_add_explicit(&q->mapping.ref, 1, memory_order_relaxed);
	struct _vqueue_shmem_region* region = (struct _vqueue_shmem_region*)(q->mapping.ptr<<6);
	*szout = _vqueue_uncompress_size(q->mapping.size_packed);
	lock_release(&q->mapping_lock, 1);
	return region;
}

static inline struct _vqueue_shmem_region* _vqueue_get_mapping_for(struct _vqueue* q, size_t* szout, void* thing){
	lock_acquire(&q->mapping_lock, 1);
	struct _vqueue_mapping_descriptor* v = &q->mapping;
	next:
	char* region = (char*)(v->ptr<<6);
	uint64_t sz = _vqueue_uncompress_size(v->size_packed);
	if(thing >= region && thing < region + sz){
		lock_release(&q->mapping_lock, 1);
		*szout = sz;
		return (struct _vqueue_mapping_descriptor*)region;
	}
	v = v->next;
	if(v) goto next;
	else{
		lock_release(&q->mapping_lock, 1);
		return 0;
	}
}

static inline struct _vqueue_shmem_region* _vqueue_release_mapping(struct _vqueue* q, struct _vqueue_shmem_region* ptr, size_t size){
	lock_acquire(&q->mapping_lock, 1);
	uint64_t ptr1 = (uint64_t)ptr>>6;
	if(q->mapping.ptr == ptr1){
		atomic_fetch_sub_explicit(&q->mapping.ref, 1, memory_order_relaxed);
		lock_release(&q->mapping_lock, 1);
		return;
	}
	struct _vqueue_mapping_descriptor* v = q->mapping.next;
	while(v){
		if(v->ptr != ptr1){ v = v->next; continue; }
		
		bool finished = atomic_fetch_sub_explicit(&q->mapping.ref, 1, memory_order_relaxed) == 1;
		lock_release(&q->mapping_lock, 1);
		if(finished){
			munmap(ptr, size);
			lock_acquire(&q->mapping_lock, LOCK_MAX);
			struct _vqueue_mapping_descriptor** ov2 = &q->mapping.next, *v2 = *ov2;
			while(v2){
				if(v == v2){ *ov2 = v->next; break; }
				v2 = *(ov2 = &v2->next);
			}
			lock_release(&q->mapping_lock, LOCK_MAX);
			free(v);
		}
		return;
	}
}

static inline struct _vqueue_shmem_region* _vqueue_resize_mapping(struct _vqueue* q, struct _vqueue_shmem_region* old, uint64_t* size, uint64_t newsize){
	uint64_t old1 = (uint64_t)old>>6;
	uint64_t size_packed = _vqueue_compress_size(newsize);
	newsize = _vqueue_uncompress_size(size_packed);
	lock_acquire(&q->mapping_lock, LOCK_MAX);
	if(q->mapping.size_packed >= size_packed){
		*size = _vqueue_uncompress_size(q->mapping.size_packed);
		lock_release(&q->mapping_lock, LOCK_MAX);
		return old;
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
		munmap(old, *size);
	}else{
		struct _vqueue_mapping_descriptor** ov = &q->mapping.next, *v = *ov;
		while(v){
			if(v->ptr != old1){ v = *(ov = &v->next); continue; }
			if(atomic_fetch_sub_explicit(&q->mapping.ref, 1, memory_order_relaxed) == 1){
				munmap(old, *size);
				*ov = v->next;
				free(v);
			}
			break;
		}
	}
	q->mapping.ptr = new_map;
	q->mapping.size_packed = size_packed;
	lock_release(&q->mapping_lock, LOCK_MAX);
	*size = newsize;
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

vqueue_block_t vqueue_alloc(vqueue_t* q, size_t bytes){

}
void vqueue_post(vqueue_t* q, vqueue_block_t block){
	uint64_t mapping_size;
	struct _vqueue_shmem_region* mapping = _vqueue_get_mapping_for(q, &mapping_size, block.data);
	uint64_t new_ptr = ((char(*)[64])block.data - mapping->blocks) | _vqueue_compress_size(block.size)<<40;

	uint8_t n;
	uint64_t ptr = _vqueue_pointer_acquire(q, &mapping->tailp, &n), vo;
	_Atomic uint64_t* hdr2n;
	// Pointer chase
	while(true){
		hdr2n = ptr ? &(((struct _vqueue_msg_hdr*) &mapping->blocks[ptr]) - 1)->next : mapping->head;
		vo = atomic_load_explicit(hdr2n, memory_order_acquire);
		cmpxchg_failed: {
			uint8_t n2;
			vo = _vqueue_pointer_acquire2(q, hdr2n, n2, vo);
			_vqueue_pointer_release(q, ptr, n);
			ptr = vo&0xFFFFFFFFFF; n = n2;
		}
	}
	swap:
	if(!atomic_compare_exchange_strong_explicit(hdr2n, &vo, new_ptr, memory_order_acquire, memory_order_acquire)){
		goto cmpxchg_failed;
	}
	_vqueue_pointer_release(q, ptr, n);
	
	_vqueue_release_mapping(q, mapping, mapping_size);
	if(!ptr) sem_post(&mapping->sema4);
}

vqueue_block_t vqueue_wait(vqueue_t* q){
	size_t mapping_size;
	struct _vqueue_shmem_region* mapping = _vqueue_acquire_mapping(q, &mapping_size);
	retry: {}
	uint8_t n;
	uint64_t ptr = _vqueue_pointer_acquire(q, &mapping->head, &n);
	find_next:
	if(!(ptr&0xFFFFFFFFFF)){
		// No messages. Block
		sem_wait(&mapping->sema4);
		goto retry;
	}
	size_t expected_size = (ptr<<6) + _vqueue_uncompress_size(ptr>>40);
	if(mapping_size < expected_size) mapping = _vqueue_resize_mapping(q, mapping, &mapping_size, expected_size);

	uint8_t* payload = (uint8_t*)mapping->blocks[ptr&0xFFFFFFFFFF];
	struct _vqueue_msg_hdr* hdr = ((struct _vqueue_msg_hdr*) &payload) - 1;
	uint32_t v = atomic_load_explicit(&hdr->aid, memory_order_relaxed);
	retry:
	if(v >> 24){
		// Someone else acquired it, we find next message
		uint8_t n2;
		uint64_t ptr2 = _vqueue_pointer_acquire(q, &hdr->next, &n);
		_vqueue_pointer_release(q, ptr, n);
		ptr = ptr2; n = n2;
		goto find_next;
	}
	if(!atomic_compare_exchange_weak_explicit(&hdr->aid, &v, q->aid|0x1000000, memory_order_acquire, memory_order_relaxed)) goto retry;
	_vqueue_pointer_release(q, ptr, n);
	return (vqueue_block_t){hdr->size, payload};
}
void vqueue_free(vqueue_t* q, vqueue_block_t block){
	uint64_t mapping_size;
	struct _vqueue_shmem_region* mapping = _vqueue_get_mapping_for(q, &mapping_size, block.data);

	_vqueue_release_mapping(q, mapping, mapping_size);
}
