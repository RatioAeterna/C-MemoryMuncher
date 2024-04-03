#include <stdatomic.h>
#include <stdlib.h>

typedef struct BarrierEntry {
    void* address;
    struct BarrierEntry* next;
} BarrierEntry;

typedef struct {
    atomic_intptr_t head;
} Barrier;

void barrier_init(Barrier* barrier) {
    barrier->head = ATOMIC_VAR_INIT((intptr_t)NULL);
}

void barrier_add(Barrier* barrier, void* address) {
    BarrierEntry* new_entry = malloc(sizeof(BarrierEntry));
    new_entry->address = address;

    BarrierEntry* old_head;
    do {
        old_head = (BarrierEntry*)atomic_load(&barrier->head);
        new_entry->next = old_head;
    } while (!atomic_compare_exchange_weak(&barrier->head, (intptr_t*)&old_head, (intptr_t)new_entry));
}

void barrier_process(Barrier* barrier) {
    BarrierEntry* entry = (BarrierEntry*)atomic_exchange(&barrier->head, (intptr_t)NULL);

    while (entry != NULL) {
        // Process the entry
        BarrierEntry* temp = entry;
        entry = entry->next;
        free(temp);
    }
}
