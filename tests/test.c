#define VQUEUE_IMPL
#include "vqueue.h"
#include <stdio.h>

vqueue_t q;
void* reader_thread(void* _){
	// process 2 (reader)
	while(true){
		vqueue_block_t msg = vqueue_wait(&q);

		printf("Got %zu bytes:\n", msg.size);
		fwrite(msg.data, 1, msg.size, stdout);

		vqueue_free(&q, msg);
	}
}

int main(){
	thread_t thr = thread_create(reader_thread, 0, 0);
	if(!vqueue_open(&q, "my_ipc", -1)){
		fprintf(stderr, "vqueue_open() failed"); return 1;
	}

	// process 1 (writer)
	char msg[] = "Hello, world!";
	vqueue_block_t slot = vqueue_alloc(&q, sizeof(msg));
	
	memcpy(slot.data, msg, sizeof(msg));

	vqueue_post(&q, slot);

	thread_join(thr);
}