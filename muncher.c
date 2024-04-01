#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>

#include "muncher.h"
#include <unistd.h>


typedef struct header {
    unsigned int    size;
    struct header   *next;
} header_t;

static header_t base;           /* Zero sized block to get us started. */
static header_t *freep = &base; /* Points to first free block of memory. */
static header_t *usedp;         /* Points to first used block of memory. */

static uintptr_t stack_bottom;


static size_t used_memory = 0;
static size_t total_memory = 8ULL * 1024 * 1024 * 1024;  // 8GB
static int num_threads = 0; // set at runtime 
static pthread_t *threads = NULL;


int optimal_num_threads() {
    return sysconf(_SC_NPROCESSORS_ONLN);  // number of cores. Simple heuristic: one thread per core
}

int get_num_threads(void) {
    if (num_threads == 0) {
        num_threads = optimal_num_threads();
    }
    return num_threads;
}



/*
 * Scan the free list and look for a place to put the block. Basically, we're 
 * looking for any block that the to-be-freed block might have been partitioned from.
 */
static void add_to_free_list(header_t *bp) {
    header_t *p;

    for (p = freep; !(bp > p && bp < p->next); p = p->next)
        if (p >= p->next && (bp > p || bp < p->next))
            break;

    if (bp + bp->size == p->next) {
        bp->size += p->next->size;
        bp->next = p->next->next;
    } else
        bp->next = p->next;

    if (p + p->size == bp) {
        p->size += bp->size;
        p->next = bp->next;
    } else
        p->next = bp;

    freep = p;
}

#define MIN_ALLOC_SIZE 4096 /* We allocate blocks in page sized chunks. */

/*
 * Request more memory from the kernel.
 */
static header_t* morecore(size_t num_units) {
    void *vp;
    header_t *up;

    if (num_units > MIN_ALLOC_SIZE)
        num_units = MIN_ALLOC_SIZE / sizeof(header_t);

    if ((vp = sbrk(num_units * sizeof(header_t))) == (void *) -1)
        return NULL;

    up = (header_t *) vp;
    up->size = num_units;
    add_to_free_list (up);
    return freep;
}

#define UNTAG(p) (((uintptr_t) (p)) & 0xfffffffc)

/*
 * Scan a region of memory and mark any items in the used list appropriately.
 * Both arguments should be word aligned.
 */
static void scan_region(uintptr_t *sp, uintptr_t *end) {
    header_t *bp;

    for (; sp < end; sp++) {
        uintptr_t v = *sp;
        bp = usedp;
        do {
            if (bp + 1 <= v &&
                bp + 1 + bp->size > v) {
                    bp->next = ((uintptr_t) bp->next) | 1;
		    //printf("marking block. size: %d\n", bp->size);
                    break;
            }
        } while ((bp = UNTAG(bp->next)) != usedp);
    }
}

void* munch_alloc(size_t size) {
    // check to see if we need to trigger garbage collection
    // TODO we will probably want to move this somewhere nicer eventually
    //float usage = (float)used_memory / total_memory;
    //if(usage > 0.75) {
    /*
    if(usage > 0.0001) {
	printf("going to MUNCH!... using %d bytes \n", used_memory);
	muncher_collect();
    }
    */

    size_t num_units;
    header_t *p, *prevp;

    num_units = (size + sizeof(header_t) - 1) / sizeof(header_t) + 1;  
    prevp = freep;

    for (p = prevp->next;; prevp = p, p = p->next) {
        if (p->size >= num_units) { /* Big enough. */
            if (p->size == num_units) /* Exact size. */
                prevp->next = p->next;
            else {
                p->size -= num_units;
                p += p->size;
                p->size = num_units;
            }

            freep = prevp;

            /* Add to p to the used list. */
            if (usedp == NULL)  
                usedp = p->next = p;
            else {
                p->next = usedp->next;
                usedp->next = p;
            }
	    used_memory += size; // We are now using 'size' extra bytes of memory in total
            return (void *) (p + 1);
        }
        if (p == freep) { /* Not enough memory. */
            p = morecore(num_units);
            if (p == NULL) /* Request for more memory failed. */
                return NULL;
        }
    }
}

void muncher_endgame(void) {




}



/*
 * Find the absolute bottom of the stack and set stuff up.
 */
void muncher_init(void) {
    static int initted;
    FILE *statfp;

    if (initted)
        return;

    initted = 1;
    get_num_threads();
    // malloc all of our execution threads..
    threads = (pthread_t *)malloc(num_threads * sizeof(pthread_t));
    if (threads == NULL) { // allocation failure
        exit(1);
    }

    atexit(muncher_endgame); // specify that we want to call 'muncher_endgame()' right before program exit to clean up


    statfp = fopen("/proc/self/stat", "r");
    assert(statfp != NULL);
    fscanf(statfp,
           "%*d %*s %*c %*d %*d %*d %*d %*d %*u "
           "%*lu %*lu %*lu %*lu %*lu %*lu %*ld %*ld "
           "%*ld %*ld %*ld %*ld %*llu %*lu %*ld "
           "%*lu %*lu %*lu %lu", &stack_bottom);
    fclose(statfp);

    usedp = NULL;
    base.next = freep = &base;
    base.size = 0;
}


/*
 * Scan the marked blocks for references to other unmarked blocks.
 */
static void scan_heap(void) {
    uintptr_t *vp;
    header_t *bp, *up;

    for (bp = UNTAG(usedp->next); bp != usedp; bp = UNTAG(bp->next)) {
        if (!((uintptr_t)bp->next & 1))
            continue;
        for (vp = (uintptr_t *)(bp + 1);
             vp < (bp + bp->size + 1);
             vp++) {
            uintptr_t v = *vp;
            up = UNTAG(bp->next);
            do {
                if (up != bp &&
                    up + 1 <= v &&
                    up + 1 + up->size > v) {
                    up->next = ((uintptr_t) up->next) | 1;
                    break;
                }
            } while ((up = UNTAG(up->next)) != bp);
        }
    }
}




/*
 * Mark blocks of memory in use and free the ones not in use.
 */
void muncher_collect(void) {
    header_t *p, *prevp, *tp;
    uintptr_t stack_top;
    extern char end, etext; /* Provided by the linker. */

    if (usedp == NULL)
        return;

    /* Scan the BSS and initialized data segments. */
    scan_region(&etext, &end);

    /* Scan the stack. */
    asm volatile ("movq %%rbp, %0" : "=r" (stack_top));
    scan_region(stack_top, stack_bottom);

    /* Mark from the heap. */
    scan_heap();

    /* And now we collect! */
    for (prevp = usedp, p = UNTAG(usedp->next);; prevp = p, p = UNTAG(p->next)) {
    next_chunk:
        if (!((unsigned int)p->next & 1)) {
            /*
             * The chunk hasn't been marked. Thus, it must be set free. 
             */
	    // decrement total size usage
	    //printf("sweeping block!, size: %d\n", p->size);
	    used_memory -= p->size;

            tp = p;
            p = UNTAG(p->next);
            add_to_free_list(tp);

            if (usedp == tp) { 
                usedp = NULL;
                break;
            }

            prevp->next = (uintptr_t)p | ((uintptr_t) prevp->next & 1);
            goto next_chunk;
        }
        p->next = ((uintptr_t) p->next) & ~1;
        if (p == usedp)
            break;
    }
}
