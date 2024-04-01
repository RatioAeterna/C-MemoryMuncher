#include <stdlib.h>
#include <stdio.h>
#include "../muncher.h"

// simple test to make sure that check system memory usage is relatively constant when doing garbage collection.
// Prints out the process' VmData before and after usage.

char* read_vm_data() {
    FILE* fp = fopen("/proc/self/status", "r");
    if (fp == NULL) {
	fprintf(stderr, "Failed to open /proc/self/status\n");
	return;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "VmData:", 7) == 0) {
            printf("%s", line);
            break;
        }
    }

    fclose(fp);
}


int main() {

    const int iterations = 500;
    const size_t block_size = 1024; // 1 KB
    
    read_vm_data();

    muncher_init(); // initialize the gc
    for(int i = 0; i < iterations; ++i) {
	//printf("i: %d\n", i);
	char* addr = (char*)munch_alloc(block_size);
	addr[0] = 'm';
	addr[1] = 'u';
	addr[2] = 'n';
	addr[3] = 'c';
	addr[4] = 'h';
	muncher_collect();
    }

    read_vm_data();
}
