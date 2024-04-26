#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "muncher.h"
#include <unistd.h>

#define UNTAG(p) (((uintptr_t) (p)) & 0xfffffffc)
#define MIN_ALLOC_SIZE 4096 /* We allocate blocks in page sized chunks. */

// NOTE: credit for skeleton code for basic mark and sweep to Matthew Plant (https://maplant.com/2020-04-25-Writing-a-Simple-Garbage-Collector-in-C.html)
// A nontrivial amount of that original code has been changed/debugged in one way or another, several functions remain unchanged.

typedef struct header {
    unsigned int    size;
    unsigned int    original_size;
    //unsigned int ref_count;
    struct header   *next;
    uintptr_t mmap_addr;
} header_t;


static header_t base;           /* Zero sized block to get us started. */
static header_t *freep = &base; /* Points to first free block of memory. */
//static header_t *freep = NULL; /* Points to first free block of memory. */
static header_t *usedp;         /* Points to first used block of memory. */

static uintptr_t stack_bottom;

static int num_mmaps = 0;

static size_t used_memory = 0;
static size_t total_memory = 8ULL * 1024 * 1024 * 1024;  // 8GB
static int num_threads = 0; // set at runtime 
static pthread_t *threads = NULL;

static Barrier *barrier;


int optimal_num_threads() {
    return sysconf(_SC_NPROCESSORS_ONLN);  // number of cores. Simple heuristic: one thread per core
}

int get_num_threads(void) {
    if (num_threads == 0) {
        num_threads = optimal_num_threads();
    }
    return num_threads;
}


typedef struct RegisterSnapshot {
    uintptr_t rax, rbx, rcx, rdx, rsi, rdi;
    uintptr_t r8, r9, r10, r11, r12, r13, r14, r15;
} RegisterSnapshot;


static RegisterSnapshot *regs = NULL;

// saves the state of the general-purpose regs for marking later.

// NOTE: we don't capture rbp since it seems to cause some bugs, and also because
// you would just never find a reference to a HEAP ADDRESS in the stack base pointer... tsk tsk
void capture_registers() {
    asm volatile(
    "mov %%rax, %0\n"
    "mov %%rbx, %1\n"
    "mov %%rcx, %2\n"
    "mov %%rdx, %3\n"
    "mov %%rsi, %4\n"
    "mov %%rdi, %5\n"
    "mov %%r8, %6\n"
    "mov %%r9, %7\n"
    "mov %%r10, %8\n"
    "mov %%r11, %9\n"
    "mov %%r12, %10\n"
    "mov %%r13, %11\n"
    "mov %%r14, %12\n"
    "mov %%r15, %13\n"
    : "=m"(regs->rax), "=m"(regs->rbx), "=m"(regs->rcx), "=m"(regs->rdx),
      "=m"(regs->rsi), "=m"(regs->rdi), "=m"(regs->r8),
      "=m"(regs->r9), "=m"(regs->r10), "=m"(regs->r11), "=m"(regs->r12),
      "=m"(regs->r13), "=m"(regs->r14), "=m"(regs->r15)
    :
    : "memory"  // Clobber memory to prevent the compiler from reordering memory access across this point
);


}








// cycle through allocated blocks to see if there are any references in our register snapshot.
void mark_register_roots() {
    uintptr_t* reg_ptr = (uintptr_t*)regs;
    size_t num_registers = sizeof(RegisterSnapshot) / sizeof(uintptr_t);

    for (size_t i = 0; i < num_registers; i++) {
        uintptr_t reg_value = reg_ptr[i];  // Dereference to get the register value

        header_t* bp = usedp;
        do {
            if ((uintptr_t)(bp + 1) <= reg_value && 
                (uintptr_t)(bp + 1) + bp->size > reg_value) {
                bp->next = (header_t*)((uintptr_t)bp->next | 1);  // Mark the block as reached
                break;
            }
        } while ((bp = UNTAG(bp->next)) != usedp);
    }
}

// this should disable write permissions on EVERY page, so we need to be smart about how we do this
int prepare_cow_snapshot() {
    size_t pagesize = getpagesize();
    header_t *temp = freep;
    /*
    while (temp != NULL) {
	printf("HERE %d\n", temp->mmap_addr);
	fflush(stdout);
	size_t aligned_size = (sizeof(header_t) + temp->original_size + pagesize - 1) & ~(pagesize - 1);
	if (mprotect(temp->mmap_addr, aligned_size, PROT_READ) == -1) {
	    perror("mprotect");
	    return -1;
	}
	temp = temp->next;
    }
    */

    header_t *p = usedp;
    if (p != NULL) { // Ensure there's at least one element in the list
	do {
	    printf("top\n");
	    fflush(stdout);
	    header_t *next_page = p;
	    while((p->mmap_addr < next_page) && (next_page <= (p->mmap_addr+pagesize)) && (next_page != usedp)) {
		next_page = next_page->next;	
	    }

	    printf("HERE %d, %d\n", p->mmap_addr, p);
	    fflush(stdout);
	    // ignore all zero address blocks
	    if(p->mmap_addr == 0) {
		p = p->next;
		printf("continuing \n");
		fflush(stdout);
		continue;
	    }
            size_t aligned_size = (sizeof(header_t) + p->original_size + pagesize - 1) & ~(pagesize - 1);
	    uintptr_t addr_to_protect = p->mmap_addr;
	    p = next_page; // increment the page BEFORE we make it readonly...
	    if (mprotect(p->mmap_addr, aligned_size, PROT_READ) == -1) {
		perror("mprotect");
		return -1;
	    }
	    printf("success\n");
	    fflush(stdout);
	    //return 0;
	    //p = p->next;
	    //p = next_page;
	} while (p != usedp);
    }
    return 0;
}


// basically just enable write permissions for every single block to resume normal use
int restore_heap_write() {
    header_t *temp = freep;
    while (temp != NULL) { // TODO does 'size' actually have bytes, or is it in 'units'?
	if (mprotect(temp, sizeof(header_t) + temp->size, PROT_READ | PROT_WRITE) == -1) {
	    perror("mprotect");
	    return -1;
	}
	temp = temp->next;
    }

    header_t *p = usedp;
    if (p != NULL) { // Ensure there's at least one element in the list
	do {
	    if (mprotect(p, sizeof(header_t) + p->size, PROT_READ | PROT_WRITE) == -1) {
		perror("mprotect");
		return -1;
	    }
	    p = p->next;
	} while (p != usedp);
    }
    return 0;
}


/*
 * Scan the free list and look for a place to put the block. Basically, we're 
 * looking for any block that the to-be-freed block might have been partitioned from.
 */
static void add_to_free_list(header_t *bp) {
    header_t *p;
    /*
    if (freep == NULL) {
        // Empty free list, set freep to the new block
        freep = bp;
        bp->next = NULL;
	printf("adding to free\n");
	fflush(stdout);
	return;
    }
    */
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

/*
 * Request more memory from the kernel.
 */
static header_t* morecore(size_t num_units) {
    size_t pagesize = getpagesize();
    size_t required_size = num_units * sizeof(header_t);
    // Align required size to the next page boundary
    size_t total_size = (required_size + pagesize - 1) & ~(pagesize - 1);

    void *vp;
    if (posix_memalign(&vp, pagesize, total_size) != 0) {
	printf("misalignment\n");
	fflush(stdout);
        return NULL;
    }
    if (mmap(vp, total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) {
	printf("map failed\n");
	fflush(stdout);
        return NULL;
    }
    num_mmaps += 1; // increment this for debugging purposes
    header_t *up = (header_t*) vp;
    up->size = total_size / sizeof(header_t); // Convert total size back to units
    up->mmap_addr = vp;
    printf("mmap addr original: %d\n", up->mmap_addr);
    fflush(stdout);
    up->original_size = total_size / sizeof(header_t);
    add_to_free_list(up);
    return freep;
}

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
            if (p->size == num_units) {/* Exact size. */
		printf("this1\n");
		fflush(stdout);
                prevp->next = p->next;
	    }
            else {
		printf("this2 %d\n", p->mmap_addr);
		printf("p val: %d\n", p);
		fflush(stdout);
		uintptr_t mmap_addr_copy = p->mmap_addr;
		size_t original_size_copy = p->original_size;
                p->size -= num_units;
                p += p->size;
                p->size = num_units;
		//p->mmap_addr = prevp->mmap_addr;
		//p->original_size = prevp->original_size;
		p->mmap_addr = mmap_addr_copy;
		p->original_size = original_size_copy;
		printf("this2 AGAIN %d\n", p->mmap_addr);
		printf("p val: %d\n", p);
		fflush(stdout);
            }

            //freep = prevp;

            /* Add to p to the used list. */
            if (usedp == NULL) {
                usedp = p->next = p;
		usedp->mmap_addr = p->mmap_addr;
		usedp->original_size = p->original_size;
		printf("this3... %d\n", usedp->mmap_addr);
		fflush(stdout);
	    }
            else {
		printf("this4\n");
		fflush(stdout);
                p->next = usedp->next;
                usedp->next = p;
		p->mmap_addr = usedp->mmap_addr; // Set the mmap_addr for the new used block
		p->original_size = usedp->original_size; // Set the original_size for the new used block
            }
	    used_memory += size; // We are now using 'size' extra bytes of memory in total
            return (void *) (p + 1);
        }
        if (p == freep) { /* Not enough memory. */
            p = morecore(num_units);
	    printf("this5 %d, %d, %d\n", p->mmap_addr, p->next->mmap_addr, freep);
	    printf("p val: %d, p->next: %d\n", p, p->next);
	    fflush(stdout);
            if (p == NULL) /* Request for more memory failed. */
                return NULL;
        }
    }
}


void muncher_cleanup(void) {
    printf("number of allocs: %d\n", num_mmaps);
    fflush(stdout);
    free(regs);
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

    // initialize memory barrier
    //barrier_init(barrier);

    get_num_threads();
    // malloc all of our execution threads..
    threads = (pthread_t *)malloc(num_threads * sizeof(pthread_t));
    if (threads == NULL) { // allocation failure
        exit(1);
    }

    atexit(muncher_cleanup); // specify that we want to call 'muncher_cleanup()' right before program exit to clean up


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
    //freep = NULL;
    base.next = &base;
    base.size = 0;


    regs = malloc(sizeof(RegisterSnapshot));
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


void mark(void) {
    uintptr_t stack_top;
    extern char end, etext; /* Provided by the linker. */


    if (usedp == NULL)
        return;

    printf("in mark\n");
    fflush(stdout);

    /* Scan the BSS and initialized data segments. */
    scan_region(&etext, &end);

    printf("scanned bss\n");
    fflush(stdout);

    /* Scan the stack. */
    asm volatile ("movq %%rbp, %0" : "=r" (stack_top));
    scan_region(stack_top, stack_bottom);

    printf("scanned stack\n");
    fflush(stdout);

    /* Mark from the heap. */
    //scan_heap();

    //printf("scanned heap\n");
    //fflush(stdout);

    /* We want to get a snapshot of the register values at this particular moment in time,
     * so we can check for references */
    capture_registers();

    /* Mark from registers. */
    mark_register_roots();

    //printf("scanned regs\n");
    //fflush(stdout);

}

void sweep(void) {
    header_t *p, *prevp, *tp;
    size_t pagesize = getpagesize();

    /* And now we collect! */
    for (prevp = usedp, p = UNTAG(usedp->next);; prevp = p, p = UNTAG(p->next)) {
    next_chunk:
        if (!((unsigned int)p->next & 1)) {
            /*
             * The chunk hasn't been marked. Thus, it must be set free. 
             */
	    // decrement total size usage
	    printf("sweeping block!, size: %d\n", p->size);
	    fflush(stdout);

	    used_memory -= p->size;

            tp = p;
            p = UNTAG(p->next);

	    // Unmap the freed chunk
            size_t block_size = tp->size * sizeof(header_t);
	    size_t aligned_size = (block_size + pagesize - 1) & ~(pagesize - 1);

	    printf("aligned size: %d\n", aligned_size);
	    fflush(stdout);
	    printf("tp: %d\n", tp);
	    fflush(stdout);

            if (munmap(tp->mmap_addr, aligned_size) == -1) {
                perror("munmap");
                // Handle the error appropriately
            }
            //add_to_free_list(tp);

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

/*
 * Mark blocks of memory in use and free the ones not in use.
 */
void muncher_collect(void) {
    prepare_cow_snapshot();
    printf("going to mark\n");
    fflush(stdout);
    mark();
    printf("going to sweep\n");
    fflush(stdout);
    sweep();
    restore_heap_write();
}
