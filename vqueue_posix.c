#pragma once
#define _FILE_OFFSET_BITS 64
#include <vqueue.h>
#include <semaphore.h>
#include <limits.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>

#define _VQUEUE_INIT_MAPPING 16384
struct _vqueue{
	int shmem_fd; uint32_t aid;
	struct{
		sem_t sema4;
		_Atomic uint64_t msg_count;
		_Atomic uint32_t open_count, open_counter;
	}* header;
};

static inline void* _vqueue_mmap(int fd, off_t off, size_t sz){
	void* addr = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
#define _VQUEUE_ERR_MSG "vqueue: mmap(): User address space exhausted\nThis is a FATAL out-of-memory exception\n"
	if(addr == (void*)-1){ write(STDERR_FILENO, _VQUEUE_ERR_MSG, sizeof(_VQUEUE_ERR_MSG)); abort(); }
#undef _VQUEUE_ERR_MSG
	return addr;
}

vqueue_t vqueue_open(const char* path){
	struct _vqueue q;
	q.shmem_fd = shm_open(path, O_RDWR | O_CREAT, 0666);
	q.header = _vqueue_mmap(q.shmem_fd, 0, _VQUEUE_INIT_MAPPING);
	q.aid = atomic_fetch_add_explicit(&q.header->open_counter, 1, memory_order_relaxed);
	struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = q.aid, .l_len = 1 };
	fcntl(q.shmem_fd, F_SETLK, &l);
	return q;
}
void vqueue_close(vqueue_t q){
	munmap(q.header, _VQUEUE_INIT_MAPPING);
	close(q.shmem_fd);
}
void vqueue_unlink(const char* path){ shm_unlink(path); }

vqueue_block_t* vqueue_alloc(vqueue_t q, size_t bytes){

}
void vqueue_post(vqueue_t q, const vqueue_block_t* block){

}

vqueue_block_t* vqueue_wait(vqueue_t q){

}
void vqueue_free(vqueue_t q, const vqueue_block_t* block){

}
