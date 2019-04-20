/**	
 * \file            dyn_mem.c
 * \brief           Dynamic memory manager
 */
 
/*
 * Copyright (c) 2018 Tilen Majerle
 *  
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software, 
 * and to permit persons to whom the Software is furnished to do so, 
 * subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE
 * AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * This file is part of dynamic memory library.
 *
 * Author:          Tilen MAJERLE <tilen@majerle.eu>
 */
#include "dyn_mem/dyn_mem.h"
#include "limits.h"

/* --- Memory unique part starts --- */
/* Prefix for all buffer functions and typedefs */
/**
 * \brief           Memory function/typedef prefix string
 */
#define MEM_PREF(x)                     x

/**
 * \brief           Number of bits to align memory address and size.
 *
 * Most CPUs do not offer unaligned memory access (Cortex-M0 as an example)
 * therefore it is important to have alignment.
 *
 * \note            This value can be power of `2`. Usually alignment of `4` bytes fits to all processors.
 */
#define MEM_ALIGN_NUM                   ((size_t)4)

#define MEM_MEMSET                      memset
#define MEM_MEMCPY                      memcpy
/* --- Memory unique part ends --- */

/**
 * \brief           Transform alignment number (power of `2`) to bits
 */
#define MEM_ALIGN_BITS                  ((size_t)(MEM_ALIGN_NUM - 1))

/**
 * \brief           Align input value to number of bits.
 *
 * As an example, when \ref MEM_ALIGN_NUM is set to `4`:
 *
 *  - Input: `0`; Output: `0`
 *  - Input: `1`; Output: `4`
 *  - Input: `2`; Output: `4`
 *  - Input: `3`; Output: `4`
 *  - Input: `4`; Output: `4`
 *  - Input: `5`; Output: `8`
 *  - Input: `6`; Output: `8`
 *  - Input: `7`; Output: `8`
 *  - Input: `8`; Output: `8`
 */
#define MEM_ALIGN(x)                    ((x + (MEM_ALIGN_BITS)) & ~(MEM_ALIGN_BITS))

/**
 * \brief           Memory block structure
 */
typedef struct mem_block {
    struct mem_block* next;                     /*!< Next free memory block on linked list.
                                                        Set to `NULL` when block is allocated and in use */
    size_t size;                                /*!< Size of free block.
                                                        MSB bit is set to `1` when block is allocated and in use */
} mem_block_t;

/**
 * \brief           Size of metadata header for block information
 */
#define MEM_BLOCK_META_SIZE             MEM_ALIGN(sizeof(mem_block_t))

static mem_block_t start_block;                 /*!< Holds beginning of memory allocation regions */
static mem_block_t* end_block;                  /*!< Pointer to the last memory location in regions linked list */
static size_t mem_alloc_bit;                    /*!< Bit indicating block is allocated, highest (MSB) bit indication */
static size_t mem_available_bytes;              /*!< Memory size available for allocation */
static size_t mem_regions_count;                /*!< Number of regions used for allocation */

/**
 * \brief           Insert free block to linked list of free blocks
 * \param[in]       nb: New free block to insert into linked list
 */
void
insert_free_block(mem_block_t* nb) {
    mem_block_t* curr;

    /* 
     * Try to find position to put new block
     * Search until all free block addresses are lower than new block
     */
    for (curr = &start_block; curr != NULL && curr->next < nb; curr = curr->next) {}

    /*
     * At this point we have valid currrent block
     * Current block is block before new block
     */

    /*
     * Check if current block and new block together create one big contiguous block
     * If this is the case, merge blocks together and increase current block by new block size
     */
    if (((uint8_t *)curr + curr->size) == (uint8_t *)nb) {
        curr->size += nb->size;                 /* Increase current block by size of new block */
        nb = curr;                              /* New block and current are now the same thing */
        /* 
         * It is important to set new block as current one
         * as this allows merging previous and next blocks together with new block
         * at the same time; follow next steps
         */
    }

    /* Check if new block and next of current create big contiguous block */
    if ((uint8_t *)nb + nb->size == (uint8_t *)curr->next) {
        if (curr->next == end_block) {          /* Does it points to the end? */
            nb->next = end_block;               /* Set end block pointer */
        } else {
            nb->size += curr->next->size;       /* Expand of current block for size of next free block which is right behind new block */
            nb->next = curr->next->next;        /* Next free is pointed to the next one of previous next */
        }
    } else {
        nb->next = curr->next;                  /* Set next of new block as next of current one */
    }

    /*
     * If between current and new block are more allocated blocks (gap exists),
     * set next of current to new block
     */
    if (curr != nb) {
        curr->next = nb;
    }
}

/**
 * \brief           Initialize and set memory regions for dynamic allocations
 * \param[in]       regions: Array of regions with address and its size.
 *                  Regions must be in increasing order (start address) and must not overlap in-between
 * \param[in]       len: Number of regions in array
 * \return          `0` on failure, number of final regions used for memory manager on success
 */
size_t
MEM_PREF(mem_init)(const MEM_PREF(mem_region_t)* regions, size_t len) {
    uint8_t* mem_start_addr;
    size_t mem_size;
    mem_block_t* first_block, *prev_end_block;

    /* Init function may only be called once */
    if (end_block != NULL) {
        return 0;
    }

    /* Ensure regions are growing linearly and do not overlap in between */
    mem_start_addr = (void *)0;
    mem_size = 0;
    for (size_t i = 0; i < len; i++) {
        /* New regions must be higher than previous one */
        if ((mem_start_addr + mem_size) > (uint8_t *)regions[i].start_addr) {
            return 0;
        }

        /* Save new values for next try */
        mem_start_addr = regions[i].start_addr;
        mem_size = regions[i].size;
    }

    for (; len--; regions++) {
        /* Ensure region size has enough memory */
        /* Size of region must be for at least block meta size + 1 minimum byte allocation alignment */
        mem_size = regions->size;
        if (mem_size < (MEM_BLOCK_META_SIZE + MEM_ALIGN_NUM)) {
            /* Ignore region, go to next one */
            continue;
        }

        /* Check region start address and align start address accordingly */
        /* It is ok to cast to size_t, even if pointer could be larger */
        /* Important is to check lower-bytes (and bits) */
        mem_start_addr = regions->start_addr;
        if ((size_t)mem_start_addr & MEM_ALIGN_BITS) {  /* Check alignment boundary */
            /* Start address needs manual alignment */
            /* Increase start address and decrease effective region size because of that */
            mem_start_addr += MEM_ALIGN_NUM - ((size_t)mem_start_addr & MEM_ALIGN_BITS);
            mem_size -= mem_start_addr - (uint8_t *)regions->start_addr;
        }

        /* Check region size alignment */
        if (mem_size & MEM_ALIGN_BITS) {        /* Lower bits must be zero */
            mem_size &= ~MEM_ALIGN_BITS;        /* Set lower bits to 0, decrease effective region size */
        }
        
        /* Ensure region size has enough memory after all the alignment checks */
        if (mem_size < (MEM_BLOCK_META_SIZE + MEM_ALIGN_NUM)) {
            /* Ignore region, go to next one */
            continue;
        }

        /*
         * If end_block == NULL, this indicates first iteration.
         * In first indication application shall set start_block and never again
         * end_block value holds 
         */
        if (end_block == NULL) {
            /* Next entry of start block is first region */
            /* It points to beginning of region data */
            /* In the later step(s) first block is manually set on top of memory region */
            start_block.next = (void *)mem_start_addr;
            start_block.size = 0;               /* Size of dummy start block is zero */
        }

        /* Save current end block status as it is used later for linked list insertion */
        prev_end_block = end_block;

        /* Put end block to the end of the region with size = 0 */
        end_block = (void *)((uint8_t *)mem_start_addr + mem_size - MEM_BLOCK_META_SIZE);
        end_block->next = NULL;                 /* End block in region does not have next entry */
        end_block->size = 0;                    /* Size of end block is zero */

        /*
         * Create memory region first block.
         *
         * First block meta size includes size of metadata too
         * Subtract MEM_BLOCK_META_SIZE as there is one more block (end_block) at the end of region
         *
         * Actual maximal available size for application in the region is mem_size - 2 * MEM_BLOCK_META_SIZE
         */
        first_block = (void *)mem_start_addr;
        first_block->next = end_block;          /* Next block of first is last block */
        first_block->size = mem_size - MEM_BLOCK_META_SIZE;

        /* Check if previous regions exist by checking previous end block state */
        if (prev_end_block != NULL) {
            prev_end_block->next = first_block; /* End block of previous region now points to start of current region */
        }

        mem_available_bytes += first_block->size;   /* Increase number of available bytes */
        mem_regions_count++;                    /* Increase number of used regions */
    }

    /* Set bit indicating memory allocated */
    /* Set MSB bit to `1` */
    mem_alloc_bit = (size_t)((size_t)1 << (sizeof(size_t) * CHAR_BIT - 1));

    return mem_regions_count;                   /* Return number of regions used by manager */
}

/**
 * \brief           Initialize and set memory regions for dynamic allocations
 * \param[in]       regions: Array of regions with address and its size.
 *                  Regions must be in increasing order (start address) and must not overlap in-between
 * \param[in]       len: Number of regions in array
 * \return          `0` on failure, number of final regions used for memory manager on success
 */
size_t
MEM_PREF(mem_assignmem)(const MEM_PREF(mem_region_t)* regions, size_t len) {
    return MEM_PREF(mem_init)(regions, len);
}

/**
 * \brief           Allocate memory of requested size
 * \note            Function declaration is in-line with standard C function `malloc`
 * \param[in]       size: Number of bytes to allocate
 * \return          Pointer to allocated memory on success, `NULL` otherwise
 */
void *
MEM_PREF(mem_malloc)(size_t size) {
    mem_block_t *prev, *curr, *next;
    void* retval = NULL;

    /* Check if initialized and if size is in the limits */
    if (end_block == NULL || size == 0 || (size & mem_alloc_bit)) {
        return NULL;
    }

    /* Calculate final size, including meta size */
    size = MEM_ALIGN(size) + MEM_BLOCK_META_SIZE;

    /* Check upper size limit */
    if (size & mem_alloc_bit) {
        return NULL;
    }

    /* Try to find first block with has at least `size` bytes available memory */
    prev = &start_block;                        /* Always start with start block which contains valid information about first available block */
    curr = prev->next;                          /* Set current as next of start = first available block */
    while (curr->size < size) {                 /* Loop until available block contains less memory than required */
        if (curr->next == NULL || curr == end_block) {  /* If no more blocks available */
            return NULL;                        /* No sufficient memory available to allocate block of memory */
        }
        prev = curr;                            /* Set current as previous */
        curr = curr->next;                      /* Go to next empty entry */
    }

    /* There is a valid block available */
    retval = (void *)((uint8_t *)prev->next + MEM_BLOCK_META_SIZE); /* Return pointer does not include meta */
    prev->next = curr->next;                    /* Remove this block by setting next of previous to next of current */

    /* curr block is now removed from linked list */
    
    /* 
     * If block size is bigger than required,
     * split it to to make available memory for other allocations
     * Threshold is 2 * MEM_BLOCK_META_SIZE of remaining size
     */
    if ((curr->size - size) > 2 * MEM_BLOCK_META_SIZE) {
        next = (void *)((uint8_t *)curr + size);    /* Put next block after size of current allocation */
        next->size = curr->size - size;         /* Set as remaining size */
        curr->size = size;                      /* Current size is now smaller */

        /* Insert this block to list = align all pointers to match linked list */
        insert_free_block(next);
    }

    /* curr block is now allocated and has no next entry */
    curr->size |= mem_alloc_bit;                /* Bit indicates block is allocated */
    curr->next = NULL;                          /* Allocated blocks have no next entries */

    mem_available_bytes -= size;                /* Decrease available bytes */

    return retval;
}

/**
 * \brief           Allocate contiguous block of memory for requested number of items and its size.
 *
 * It resets allocated block of memory to zero if allocation is successful
 *
 * \note            Function declaration is in-line with standard C function `calloc`
 * \param[in]       nitems: Number of elements to be allocated
 * \param[in]       size: Size of each element, in units of bytes
 * \return          Pointer to allocated memory on success, `NULL` otherwise
 */
void *
MEM_PREF(mem_calloc)(size_t nitems, size_t size) {
    void* ptr;

    size *= nitems;
    if ((ptr = MEM_PREF(mem_malloc(size))) != NULL) {
        MEM_MEMSET(ptr, 0x00, size);
    }
    return ptr;
}

/**
 * \brief           Reallocates already allocated memory with new size
 *
 * Function behaves differently, depends on input parameter of `ptr` and `size`:
 *
 *  - `ptr == NULL; size == 0`: Function returns `NULL`, no memory is allocated or freed
 *  - `ptr == NULL; size > 0`: Function tries to allocate new block of memory with `size` length, equivalent to `malloc(size)`
 *  - `ptr != NULL; size == 0`: Function frees memory, equivalent to `free(ptr)`
 *  - `ptr != NULL; size > 0`: Function tries to allocate new memory of copy content before returning pointer on success
 *
 * \note            Function declaration is in-line with standard C function `realloc`
 *
 * \param[in]       ptr: Memory block previously allocated with one of allocation functions.
 *                      It may be set to `NULL` to create new clean allocation
 * \param[in]       size: Size of new memory to reallocate
 * \return          Pointer to allocated memory on success, `NULL` otherwise
 */
void *
MEM_PREF(mem_realloc)(void *ptr, size_t size) {
    if (size == 0) {
        if (ptr != NULL) {
            MEM_PREF(mem_free(ptr));
        }
        return NULL;
    }
    if (ptr == NULL) {
        return MEM_PREF(mem_malloc(size));
    } else {
        void* new_ptr;
        size_t old_size;

        /* Get size of input pointer */
        old_size = (size_t)(((mem_block_t *)((uint8_t *)ptr - MEM_BLOCK_META_SIZE))->size) & ~mem_alloc_bit;
        new_ptr = MEM_PREF(mem_malloc(size));
        if (new_ptr != NULL) {
            MEM_MEMCPY(new_ptr, ptr, old_size > size ? size : old_size);
            MEM_PREF(mem_free(ptr));
        }
        return new_ptr;
    }
}

/**
 * \brief           Free previously allocated memory using one of allocation functions
 * \note            Function declaration is in-line with standard C function `free`
 * \param[in]       ptr: Memory to free. `NULL` pointer is valid input
 */
void
MEM_PREF(mem_free)(void* ptr) {
    mem_block_t* block;

    if (ptr == NULL) {
        return;
    }

    /* Remove offset from input pointer */
    block = (void *)((uint8_t *)ptr - MEM_BLOCK_META_SIZE);

    /* Check if block is valid */
    if (block->size & mem_alloc_bit && block->next == NULL) {
        block->size &= ~mem_alloc_bit;          /* Clear allocated bit indication */
        mem_available_bytes += block->size;     /* Increase available bytes */
        insert_free_block(block);               /* Put block back to list of free block */
    }
}