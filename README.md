# <img src="./vq-icon.png" alt="V logo" width="72" style="vertical-align: middle; margin-right: 12px" /> **Vqueue**

Blazingly fast copy-less IPC.

```c
#define VQUEUE_IMPL
#include "vqueue.h"
#include <stdio.h>

vqueue_t q;

int main(){
	if(!vqueue_open(&q, "my_ipc", -1)){
		fprintf(stderr, "vqueue_open() failed"); return 1;
	}


	char msg[] = "Hello, world!";
	vqueue_block_t slot = vqueue_alloc(&q, sizeof(msg));
	
	memcpy(slot.data, msg, sizeof(msg));
	vqueue_post(&q, slot);


	// Read loop. Can be a different process
	while(true){
		vqueue_block_t msg = vqueue_wait(&q);

		printf("Got %zu bytes:\n", msg.size);
		fwrite(msg.data, 1, msg.size, stdout);
		putc('\n', stdout);

		vqueue_free(&q, msg);
	}

	vqueue_close(&q);
}
```

## Details

Vqueue implements an adaptive (unbounded) ring buffer allocator on top of a shared memory object. The "queue" aspect is implemented as a linked list, allowing allocation order to differ from submit order, and simplifying certain aspects of the implementation.

The entire container is lock-free in order to avoid priority inversion and other unpredictable scheduling shenanigans. It is also designed to be fault tolerant, that is, if one accessing process dies at an arbitrary point, other processes will ensure the queue remains in a valid state with no leaked allocations.

