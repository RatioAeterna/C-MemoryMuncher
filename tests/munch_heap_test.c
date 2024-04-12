#include <stdlib.h>
#include <stdio.h>
#include "../muncher.h"

// test the ability of the gc to deal specifically with references that live on the heap.. i.e., testing usage of memory barriers
// Prints out the process' VmData before and after usage.

typedef struct Node {
    int data;
    struct Node* next;
} Node;

Node* create_list(int size) {
    Node* head = NULL;
    for (int i = 0; i < size; ++i) {
        Node* new_node = (Node*)munch_alloc(sizeof(Node));
        new_node->data = i;
        new_node->next = head;
        head = new_node;
    }
    return head;
}

void manipulate_list(Node* head, int iterations) {
    for (int i = 0; i < iterations; ++i) {
        head->data += i;  // Modify the data to ensure it's used
    }
}


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
    const int list_size = 1000000;
    const int iterations = 100;

    muncher_init(); // Initialize the GC

    read_vm_data();

    Node* list_head = create_list(list_size);
    manipulate_list(list_head, iterations);

    // At this point, the entire list is still reachable, so GC should not collect

    list_head = NULL;  // Drop the reference to the list
    // Now the entire list should be collectible

    muncher_collect();  // Force a collection

    read_vm_data();

    return 0;
}

