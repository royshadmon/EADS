#include "virtual_memory.h"

/*
 * This file contains a wrapper interface for allocating virtual memory.
 *
 */

#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <assert.h>

#include "util.h"
#include "mailbox.h"

/*
 * map_segment
 *
 * Map a physical memory segment to virtual memory
 *
 * Memory can be unmapped with unmap_segment
 */
void *map_segment(void *addr, size_t size) {
    // Check page alignment.
    assert(addr);
    assert((uint32_t)addr % PAGE_SIZE == 0);
    // Check page-size multiple.
    assert(size % PAGE_SIZE == 0);

    // Open the physical memory
    int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);

    if (fd < 0) {
        perror("Error: can't open /dev/mem");
        exit(-1);
    }

    // Create virtual mapping to given address
    void* mem = mmap(0, size, PROT_WRITE|PROT_READ, MAP_SHARED, fd, (uint32_t)addr);

    close(fd);

    if (mem == MAP_FAILED) {
        perror("Error: can't map memory");
        exit(-1);
    }

    return mem;
}

/*
 * unmap_segment
 *
 * Unmap the memory segment mapped with map_segment
 */
void unmap_segment(void *addr, size_t size) {
    // Check page alignment.
    assert(addr);
    assert((uint32_t)addr % PAGE_SIZE == 0);
    // Check page-size multiple.
    assert(size % PAGE_SIZE == 0);

    // Unmap the memory
    munmap(addr, size);
}

/*
 * alloc_uncached_memory
 *
 * Allocate a memory segment which is not included in the CPU cache. Makes use
 * of the VideoCore mailbox interface to allocate memory.
 *
 * Memory can be freed with free_uncached_memory
 *
 * size must be a multiple of the size of a page
 */
VC_MEM_VIRT_MEM alloc_uncached_memory(int fd, size_t size) {
    // Check page-size multiple.
    assert(size % PAGE_SIZE == 0);

    VC_MEM_VIRT_MEM mem = {.size = size};

    mem.vc_h = mailbox_mem_alloc(fd, mem.size, PAGE_SIZE, DMA_MEM_FLAGS);
    mem.phys = mailbox_mem_lock(fd, mem.vc_h);
    // Map the physical address in to virtual memory. The physical address is
    // computed as the bus address with the top two bits zeroed.
    mem.virt = map_segment((void*)((uint32_t)mem.phys & 0x3FFFFFFF), mem.size);

    return mem;
}

/*
 * free_uncached_memory
 *
 * Free the uncached memory allocated with alloc_uncached_memory
 */
void free_uncached_memory(int fd, VC_MEM_VIRT_MEM mem) {
    unmap_segment(mem.virt, mem.size);
    mailbox_mem_unlock(fd, mem.vc_h);
    mailbox_mem_free(fd, mem.vc_h);
}

/*
 * mem_virt_to_phys
 *
 * Convert a virtual memory address to its corresponding physical address
 */
inline void* mem_virt_to_phys(VC_MEM_VIRT_MEM mem, void* virt) {
    return (void*) ((uint32_t) virt - (uint32_t) mem.virt + (uint32_t) mem.phys);
}
