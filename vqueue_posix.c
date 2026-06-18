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
	_Atomic size_t size;
	_Atomic uint64_t next;
};
#define _VQUEUE_MSG_HDR_SIZE sizeof(struct _vqueue_msg_hdr)-4

struct _vqueue_shmem_region{
	sem_t sema4;
	_Atomic uint32_t open_counter;
	_Atomic uint64_t head, tailp, left, right, rightinit, end, endinit;
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

static inline uint64_t _vqueue_safe_init(struct _vqueue* q, _Atomic uint64_t* rp, _Atomic uint64_t* rip, struct _vqueue_shmem_region* mapping, uint64_t* mapping_size, uint64_t* right_){
	uint64_t right = *right_;
	retry:
	if(right >> 40){
		uint64_t rinit = atomic_load_explicit(rip, memory_order_acquire);
		if(rinit == (right & 0xFFFFFFFFFF)){
			if(!atomic_compare_exchange_weak_explicit(rp, &right, right & 0xFFFFFFFFFF, memory_order_relaxed, memory_order_relaxed))
				goto retry;
		}else{
			uint64_t expected_mapping_size = (char*)&mapping->blocks[rinit] - (char*)mapping;
			if(expected_mapping_size > mapping_size)
				mapping = _vqueue_resize_mapping(q, mapping, &mapping_size, expected_mapping_size);
			struct _vqueue_msg_hdr* hdr = (struct _vqueue_msg_hdr*)&mapping->blocks[rinit] - 1;
			atomic_store_explicit(&hdr->aid, right>>40, memory_order_relaxed);
			// safe init that writes what gc pass would need but yields to the more precise value written by the actual allocating thread
			size_t sz2 = right - rinit - _VQUEUE_MSG_HDR_SIZE - 63, sz = atomic_load_explicit(&hdr->size, memory_order_relaxed);
			while(((sz - sz2)>>6) && !atomic_compare_exchange_weak_explicit(&hdr->size, &sz, sz2, memory_order_relaxed, memory_order_relaxed));

			if(atomic_compare_exchange_strong_explicit(rip, &rinit, right & 0xFFFFFFFFFF, memory_order_release, memory_order_relaxed)
		 	 && !atomic_compare_exchange_strong_explicit(rp, &right, right & 0xFFFFFFFFFF, memory_order_release, memory_order_relaxed))
				goto retry;
		}
	}
	*right_ = right;
	return mapping;
}

vqueue_block_t vqueue_alloc(vqueue_t* q, size_t size){
	uint64_t mapping_size;
	struct _vqueue_shmem_region* mapping = _vqueue_get_mapping(q, &mapping_size);
	uint8_t n;
	uint64_t left = _vqueue_pointer_acquire(q, mapping, &mapping->left, &n);
	
	uint64_t right = atomic_load_explicit(&mapping->right, memory_order_relaxed);
	retry1:
	mapping = _vqueue_safe_init(q, &mapping->right, &mapping->rightinit, mapping, &mapping_size, &right);
	uint64_t right2 = right + ((size+_VQUEUE_MSG_HDR_SIZE + 63) >> 6);
	if(right2 <= left){
		uint64_t right2_tagged = right2 | q->aid<<40;
		if(!atomic_compare_exchange_weak_explicit(&mapping->right, &right, right2_tagged, memory_order_acquire, memory_order_relaxed))
			goto retry1;

		uint64_t expected_mapping_size = (char*)&mapping->blocks[right2] - (char*)mapping;
		if(expected_mapping_size > mapping_size)
			mapping = _vqueue_resize_mapping(q, mapping, &mapping_size, expected_mapping_size);
		
		struct _vqueue_msg_hdr* hdr = (struct _vqueue_msg_hdr*)&mapping->blocks[right] - 1;
		atomic_store_explicit(&hdr->next, 0, memory_order_relaxed);
		atomic_store_explicit(&hdr->aid, q->aid, memory_order_relaxed);
		atomic_store_explicit(&hdr->size, size, memory_order_relaxed);

		if(atomic_compare_exchange_strong_explicit(&mapping->rightinit, &right, right2, memory_order_release, memory_order_relaxed)){
			atomic_compare_exchange_strong_explicit(&mapping->right, &right2_tagged, right2, memory_order_release, memory_order_relaxed);
		}
		_vqueue_unprotect(q, mapping, left, n);
		return (vqueue_block_t){size, (uint8_t*)&mapping->blocks[right]};
	}
	// Append to end
	uint64_t end = atomic_load_explicit(&mapping->end, memory_order_relaxed);
	retry1:
	mapping = _vqueue_safe_init(q, &mapping->end, &mapping->endinit, mapping, &mapping_size, &end);
	uint64_t end2 = end + ((size+_VQUEUE_MSG_HDR_SIZE + 63) >> 6);
	uint64_t end2_tagged = end2 | q->aid<<40;
	if(!atomic_compare_exchange_weak_explicit(&mapping->end, &end, end2_tagged, memory_order_acquire, memory_order_relaxed))
		goto retry1;

	uint64_t expected_mapping_size = (char*)&mapping->blocks[end2] - (char*)mapping;
	if(expected_mapping_size > mapping_size)
		mapping = _vqueue_resize_mapping(q, mapping, &mapping_size, expected_mapping_size);
	
	struct _vqueue_msg_hdr* hdr = (struct _vqueue_msg_hdr*)&mapping->blocks[end] - 1;
	atomic_store_explicit(&hdr->next, 0, memory_order_relaxed);
	atomic_store_explicit(&hdr->aid, q->aid, memory_order_relaxed);
	atomic_store_explicit(&hdr->size, size, memory_order_relaxed);

	if(atomic_compare_exchange_strong_explicit(&mapping->endinit, &end, end2, memory_order_release, memory_order_relaxed)){
		atomic_compare_exchange_strong_explicit(&mapping->end, &end2_tagged, end2, memory_order_release, memory_order_relaxed);
	}
	_vqueue_unprotect(q, mapping, left, n);
	return (vqueue_block_t){size, (uint8_t*)&mapping->blocks[end]};
}
void vqueue_post(vqueue_t* q, vqueue_block_t block){
	uint64_t mapping_size;
	struct _vqueue_shmem_region* mapping = _vqueue_get_mapping_for(q, &mapping_size, block.data);
	uint64_t new_ptr = ((char(*)[64])block.data - mapping->blocks) | _vqueue_compress_size(block.size)<<40;

	uint8_t n;
	uint64_t ptr = _vqueue_pointer_acquire(q, mapping, &mapping->tailp, &n), vo;
	_Atomic uint64_t* hdr2n;
	// Pointer chase
	while(true){
		hdr2n = ptr ? &(((struct _vqueue_msg_hdr*) &mapping->blocks[ptr]) - 1)->next : mapping->head;
		vo = atomic_load_explicit(hdr2n, memory_order_acquire);
		cmpxchg_failed: {
			uint8_t n2;
			vo = _vqueue_pointer_acquire2(q, mapping, hdr2n, n2, vo);
			_vqueue_unprotect(q, mapping, ptr, n);
			ptr = vo&0xFFFFFFFFFF; n = n2;
		}
	}
	swap:
	if(!atomic_compare_exchange_strong_explicit(hdr2n, &vo, new_ptr, memory_order_acquire, memory_order_acquire)){
		goto cmpxchg_failed;
	}
	_vqueue_unprotect(q, mapping, ptr, n);

	_vqueue_release_mapping(q, mapping, mapping_size);
	if(!ptr) sem_post(&mapping->sema4);
}

vqueue_block_t vqueue_wait(vqueue_t* q){
	size_t mapping_size;
	struct _vqueue_shmem_region* mapping = _vqueue_acquire_mapping(q, &mapping_size);
	retry: {}
	uint8_t n;
	uint64_t ptr = _vqueue_pointer_acquire(q, mapping, &mapping->head, &n);
	find_next:
	if((ptr&0xFFFFFFFFFF) == 0xFFFFFFFFFF){
		// No messages. Block
		sem_wait(&mapping->sema4);
		goto retry;
	}
	size_t expected_mapping_size = ((char*)&mapping->blocks[ptr] - (char*)mapping) + _vqueue_uncompress_size(ptr>>40);
	if(mapping_size < expected_mapping_size) mapping = _vqueue_resize_mapping(q, mapping, &mapping_size, expected_mapping_size);

	uint8_t* payload = (uint8_t*)mapping->blocks[ptr&0xFFFFFFFFFF];
	struct _vqueue_msg_hdr* hdr = ((struct _vqueue_msg_hdr*) &payload) - 1;
	uint32_t v = atomic_load_explicit(&hdr->aid, memory_order_relaxed);
	retry:
	if(v >> 24){
		// Someone else acquired it, we find next message
		uint8_t n2;
		uint64_t ptr2 = _vqueue_pointer_acquire(q, mapping, &hdr->next, &n);
		_vqueue_unprotect(q, mapping, ptr&0xFFFFFFFFFF, n);
		ptr = ptr2; n = n2;
		goto find_next;
	}
	// TODO: find absolute next to avoid UAF for other readers once we unprotect ptr
	if(!atomic_compare_exchange_weak_explicit(&hdr->aid, &v, q->aid|0x1000000, memory_order_acquire, memory_order_relaxed)) goto retry;
	atomic_store_explicit(&mapping->head, atomic_load_explicit(&hdr->next, memory_order_relaxed), memory_order_relaxed);
	_vqueue_unprotect(q, mapping, ptr&0xFFFFFFFFFF, n);
	return (vqueue_block_t){atomic_load_explicit(&hdr->size, memory_order_relaxed), payload};
}

static inline void _vqueue_trim(struct _vqueue* q, struct _vqueue_shmem_region* mapping, uint64_t left, bool strict){
	uint32_t last_dead = -1;
	if(strict){
		// TODO: check hazfield
	}
	struct _vqueue_msg_hdr* hdr = ((struct _vqueue_msg_hdr*) &mapping->blocks[left]) - 1;
	again:
	uint32_t current = -1u, n = _vqueue_check_protect(q, mapping, left);
	if(n == -1u) return;
	if(atomic_load_explicit(&mapping->left, memory_order_acquire) == left){
		uint64_t oleft = left;
		if(atomic_load_explicit(&hdr->aid, memory_order_relaxed) == -1u){
			// TODO: bound by end
			// TODO: wrap
			// TODO: extra check if strict
			uint64_t next = left + atomic_load_explicit(&hdr->size, memory_order_relaxed) + _VQUEUE_MSG_HDR_SIZE;
			next += (-next)&63;
			atomic_store_explicit(&mapping->left, next, memory_order_relaxed);
			left = next; goto again;
		}
		_vqueue_unprotect2(q, mapping, oleft, n, 0);
	}else{
		bool again = _vqueue_unprotect2(q, mapping, left, n, 0);
		thread_memory_barrier(mb_co_release);
		if(again) goto again;
	}
}

void vqueue_free(vqueue_t* q, vqueue_block_t block){
	uint64_t mapping_size;
	struct _vqueue_shmem_region* mapping = _vqueue_get_mapping_for(q, &mapping_size, block.data);
	uint64_t ptr = (char(*)[64])block.data - mapping->blocks;
	struct _vqueue_msg_hdr* hdr = ((struct _vqueue_msg_hdr*) block.data) - 1;
	atomic_store_explicit(&hdr->aid, -1, memory_order_relaxed);
	if(atomic_load_explicit(&mapping->left, memory_order_relaxed) == ptr)
		_vqueue_trim(q, mapping, ptr, false);
	_vqueue_release_mapping(q, mapping, mapping_size);
}
