#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

// simple sanity test to see if we're able to trigger copy-on-write on this system
int main() {
    // Get the system page size
    size_t pagesize = getpagesize();

    // Open the file created in the Btrfs subvolume
    int fd = open("./gc_space/gc_file", O_RDWR);
    if (fd == -1) {
        perror("Failed to open file");
        return 1;
    }

    // Map the file
    void* region = mmap(NULL, pagesize, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (region == MAP_FAILED) {
        perror("mmap failed");
        close(fd);
        return 1;
    }

    // Close the file descriptor as it is no longer needed
    close(fd);

    // Set the region to read-only to enable Copy-On-Write
    if (mprotect(region, pagesize, PROT_READ) == -1) {
        perror("mprotect failed to set read-only");
        munmap(region, pagesize);
        return 1;
    }

    // Attempt to write to the read-only memory
    printf("Attempting to write to read-only memory...\n");
    *(int*)region = 1;  // This should now trigger a COW page fault, not a SIGSEGV

    // Check if the write was successful
    if (*(int*)region != 1) {
        printf("Write failed, Copy-On-Write did not occur as expected.\n");
    } else {
        printf("Write succeeded, Copy-On-Write occurred.\n");
    }

    // Cleanup
    munmap(region, pagesize);
    return 0;
}

