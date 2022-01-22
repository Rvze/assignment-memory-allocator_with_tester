#include <stdarg.h>

#define _DEFAULT_SOURCE

#include <stddef.h>
#include <stdio.h>
#include <unistd.h>


#include "mem.h"
#include "mem_internals.h"
#include "util.h"

static const void *HEAP_START = NULL;

void debug_block(struct block_header *b, const char *fmt, ...);

void debug(const char *fmt, ...);

extern inline block_size
size_from_capacity(block_capacity
                   cap);

extern inline block_capacity
capacity_from_size(block_size
                   sz);

static bool block_is_big_enough(size_t query, struct block_header *block) { return block->capacity.bytes >= query; }

static size_t pages_count(size_t mem) { return mem / getpagesize() + ((mem % getpagesize()) > 0); }

static size_t round_pages(size_t mem) { return getpagesize() * pages_count(mem); }

static void block_init(void *restrict addr, block_size block_sz, void *restrict next) {
    *((struct block_header *) addr) = (struct block_header) {
            .next = next,
            .capacity = capacity_from_size(block_sz),
            .is_free = true
    };
}

static size_t region_actual_size(size_t query) { return size_max(round_pages(query), REGION_MIN_SIZE); }

extern inline bool

region_is_invalid(const struct region *r);


static void *map_pages(void const *addr, size_t length, int additional_flags) {
    return mmap((void *) addr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | additional_flags, -1, 0);
}

#define BLOCK_MIN_CAPACITY 24


/*  аллоцировать регион памяти и инициализировать его блоком */
static struct region alloc_region(void const *addr, size_t query) {
    bool status = true;
    query = region_actual_size(query);
    void *reg_adr = map_pages(addr, query, MAP_FIXED_NOREPLACE);
    if (HEAP_START == NULL) HEAP_START = reg_adr;
    if (reg_adr == MAP_FAILED || reg_adr == NULL) {
        status = false;
        reg_adr = map_pages(addr, query, 0);
        if (reg_adr == MAP_FAILED || reg_adr == NULL) {
            return REGION_INVALID;
        }
    }
    const struct region new_region = {.addr = reg_adr, .size = query, .extends = status};
    block_init(reg_adr, (block_size) {.bytes = query}, NULL);
    return new_region;
}

static void *block_after(struct block_header const *block);

void *heap_init(size_t initial) {
    const struct region region = alloc_region(HEAP_START, initial);
    if (region_is_invalid(&region)) return NULL;

    return region.addr;
}


/*  --- Разделение блоков (если найденный свободный блок слишком большой )--- */

static bool block_splittable(struct block_header *restrict block, size_t query) {
    return block->is_free &&
           query + offsetof(
                   struct block_header, contents) + BLOCK_MIN_CAPACITY <= block->capacity.bytes;
}

static void *split_block_addr(struct block_header *restrict block, block_size new_size) {
    return (void *) ((uint8_t *) block + new_size.bytes);
}


static bool split_if_too_big(struct block_header *block, size_t query) {
    const size_t capacity_query = size_max(BLOCK_MIN_CAPACITY, query);
    if (block_splittable(block, capacity_query)) {
        const block_capacity occupied_block_capacity = {query + offsetof(struct block_header, contents)};
        const block_size remaining_space = {size_from_capacity(block->capacity).bytes - occupied_block_capacity.bytes};

        void *next_block_header = split_block_addr(block, remaining_space);


        block_init(next_block_header, remaining_space, block->next);
        block->next = next_block_header;
        return true;
    }
    return false;
}


/*  --- Слияние соседних свободных блоков --- */

static void *block_after(struct block_header const *block) {
    return (void *) (block->contents + block->capacity.bytes);
}

static bool blocks_continuous(
        struct block_header const *fst,
        struct block_header const *snd) {
    return (void *) snd == block_after(fst);
}

static bool mergeable(struct block_header const *restrict fst, struct block_header const *restrict snd) {
    return fst->is_free && snd->is_free && blocks_continuous(fst, snd);
}

static bool try_merge_with_next(struct block_header *block) {
    if (!block->next || !mergeable(block, block->next))
        return false;
    block->capacity.bytes += size_from_capacity(block->next->capacity).bytes;
    block->next = block->next->next;
    return true;
}


/*  --- ... ecли размера кучи хватает --- */

struct block_search_result {
    enum {
        BSR_FOUND_GOOD_BLOCK, BSR_REACHED_END_NOT_FOUND, BSR_CORRUPTED
    } type;
    struct block_header *block;
};


static struct block_search_result find_good_or_last(struct block_header *restrict block, size_t sz) {
    if (!block) {
        return (struct block_search_result) {.type = BSR_CORRUPTED};
    }
    struct block_header *current = block;
    struct block_header *last = NULL;

    while (current) {
        if (block_is_big_enough(sz, block) && current->is_free) {
            return (struct block_search_result) {.type = BSR_FOUND_GOOD_BLOCK, .block = current};
        }
        last = current;
        current = current->next;
    }
    return (struct block_search_result) {.type = BSR_REACHED_END_NOT_FOUND, .block = last};

}

/*  Попробовать выделить память в куче начиная с блока `block` не пытаясь расширить кучу
 Можно переиспользовать как только кучу расширили. */
static struct block_search_result try_memalloc_existing(size_t query, struct block_header *block) {

    const struct block_search_result result = find_good_or_last(block, query);

    if (result.type == BSR_FOUND_GOOD_BLOCK)
        split_if_too_big(block, query);

    return result;
}


static struct block_header *grow_heap(struct block_header *restrict last, size_t query) {
    block_size size = size_from_capacity(last->capacity);
    void *new_address = (void *) ((uint8_t *) last + size.bytes);
    struct block_header *new_block = alloc_region(new_address, query).addr;

    last->next = new_block;
    if (try_merge_with_next(last)) {
        return last;
    }
    return last->next;
}


/*  Реализует основную логику malloc и возвращает заголовок выделенного блока */
static struct block_header *memalloc(size_t query, struct block_header *heap_start) {
    query = size_max(query, BLOCK_MIN_CAPACITY);
    struct block_search_result result = try_memalloc_existing(query, heap_start);

    switch (result.type) {
        case BSR_FOUND_GOOD_BLOCK:
            break;
        case BSR_REACHED_END_NOT_FOUND:
            if ((result.block = grow_heap(result.block, query)) == NULL) {
                return NULL;
            }
            split_if_too_big(result.block, query);
            break;
        default:
            return NULL;
    }
    result.block->is_free = false;
    return result.block;

}

void *_malloc(size_t query) {
    struct block_header *const addr = memalloc(query, (struct block_header *) START_HEAP);
    if (addr) return addr->contents;
    else return NULL;
}

static struct block_header *block_get_header(void *contents) {
    return (struct block_header *) (((uint8_t *) contents) - offsetof(
            struct block_header, contents));
}

void _free(void *mem) {
    if (!mem) return;
    struct block_header *header = block_get_header(mem);
    header->is_free = true;
    while (try_merge_with_next(header));
}
