#ifndef VIRTUAL_MEMORY_H
#define VIRTUAL_MEMORY_H

/*
 * This file contains a wrapper interface for allocating virtual memory.
 *
 */

#include <stddef.h>

#include "mailbox.h"

/*
 * map_segment
 *
 * Map a physical memory segment to virtual memory
 *
 * Memory can be unmapped with unmap_segment
 */
void* map_segment(void* addr, size_t size);

/*
 * unmap_segment
 *
 * Unmap the memory segment mapped with map_segment
 */
void unmap_segment(void* addr, size_t size);

typedef struct {
    VC_MEM_HANDLE vc_h;
    void* phys;
    void* virt;
    size_t size;
} VC_MEM_VIRT_MEM;

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
VC_MEM_VIRT_MEM alloc_uncached_memory(int fd, size_t size);

/*
 * free_uncached_memory
 *
 * Free the uncached memory allocated with alloc_uncached_memory
 */
void free_uncached_memory(int fd, VC_MEM_VIRT_MEM mem);

/*
 * mem_virt_to_phys
 *
 * Convert a virtual memory address to its corresponding physical address
 */
void* mem_virt_to_phys(VC_MEM_VIRT_MEM mem, void* virt);

#endif // VIRTUAL_MEMORY_H
