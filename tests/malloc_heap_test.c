#include <stdlib.h>
#include <stdio.h>

// control test for 'munch_heap_test', using regular malloc().

typedef struct Node {
    int data;
    struct Node* next;
} Node;

Node* create_list(int size) {
    Node* head = NULL;
    for (int i = 0; i < size; ++i) {
        Node* new_node = (Node*)malloc(sizeof(Node));
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
    read_vm_data();
    Node* list_head = create_list(list_size);
    manipulate_list(list_head, iterations);

    list_head = NULL;  // Drop the reference to the list

    read_vm_data();

    return 0;
}
