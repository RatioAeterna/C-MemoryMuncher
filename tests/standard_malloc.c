#include <stdlib.h>
#include <stdio.h>


// simple control test to check system memory usage when ABUSING standard malloc
// prints out the process' VmData before and after usage. Run to watch it skyrocket.

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

    const int iterations = 100000;
    const size_t block_size = 1024; // 1 KB


    read_vm_data();

    for(int i = 0; i < iterations; ++i) {
	char* addr = (char*)malloc(block_size);
	addr[0] = 'n';
	addr[1] = 'o';
	addr[2] = ' ';
	addr[3] = 'm';
	addr[4] = 'u';
	addr[5] = 'n';
	addr[6] = 'c';
	addr[7] = 'h';
    }

    read_vm_data();
}
