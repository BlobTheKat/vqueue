#include "a.h"
#define VQUEUE_IMPL
#include "vqueue.h"
#include <stdio.h>

vqueue_t q;
void* reader_thread(void* _){
	// process 2 (reader)
	while(true){
		vqueue_block_t* msg = vqueue_wait(q);

		printf("Got %zu bytes:\n", msg->size);
		fwrite(msg->data, 1, msg->size, stdout);

		vqueue_free(q, msg);
	}
}

int main(){
	thread_create(reader_thread, 0, 0);
	q = vqueue_open("/my_ipc");

	// process 1 (writer)
	char msg[] = "Hello, world!";
	vqueue_block_t* slot = vqueue_alloc(q, sizeof(msg));
	
	memcpy(slot->data, msg, sizeof(msg));

	vqueue_post(q, slot);
}