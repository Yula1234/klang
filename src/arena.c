#include "arena.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>

#if defined(__GNUC__) || defined(__clang__)
#   define ARENA_LIKELY(x)   __builtin_expect(!!(x), 1)
#   define ARENA_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#   define ARENA_LIKELY(x)   (x)
#   define ARENA_UNLIKELY(x) (x)
#endif

static inline uintptr_t arena_align_forward(uintptr_t ptr, size_t alignment) {
    assert((alignment & (alignment - 1)) == 0 && "Alignment must be a power of two");

    return (ptr + (alignment - 1)) & ~(uintptr_t)(alignment - 1);
}

static ArenaChunk* arena_chunk_create(size_t capacity) {
    size_t total_size = sizeof(ArenaChunk) + capacity;

    ArenaChunk* chunk = (ArenaChunk*)malloc(total_size);
    
    if (ARENA_UNLIKELY(!chunk)) {
        fprintf(stderr, "arena: fatal out of memory error (failed to allocate %zu bytes)\n", total_size);
        abort();
    }
    
    chunk->prev     = NULL;
    chunk->capacity = capacity;
    chunk->offset   = 0;
    
    return chunk;
}

static void arena_chunk_free(ArenaChunk* chunk) {
    free(chunk);
}

void arena_init(Arena* arena, size_t chunk_size) {
    assert(arena != NULL);

    arena->chunk_size   = (chunk_size == 0) ? ARENA_DEFAULT_CHUNK_SIZE : chunk_size;
    arena->current      = arena_chunk_create(arena->chunk_size);
    arena->free_chunks  = NULL;
    arena->total_alloc  = 0;
    arena->total_cap    = sizeof(ArenaChunk) + arena->chunk_size;
}

Arena* arena_create(size_t chunk_size) {
    Arena* arena = (Arena*)malloc(sizeof(Arena));

    if (ARENA_UNLIKELY(!arena)) {
        abort();
    }

    arena_init(arena, chunk_size);

    return arena;
}

void* arena_alloc_aligned(Arena* arena, size_t size, size_t alignment) {
    assert(arena != NULL);
    
    if (ARENA_UNLIKELY(size == 0)) {
        return NULL;
    }

    if (alignment == 0) {
        alignment = ARENA_DEFAULT_ALIGNMENT;
    }

    ArenaChunk* chunk = arena->current;
    
    uintptr_t base_ptr = (uintptr_t)(chunk + 1);
    uintptr_t curr_ptr = base_ptr + chunk->offset;
    uintptr_t alig_ptr = arena_align_forward(curr_ptr, alignment);
    
    size_t padding     = alig_ptr - curr_ptr;
    size_t needed      = size + padding;

    if (ARENA_LIKELY(chunk->offset + needed <= chunk->capacity)) {
        chunk->offset += needed;
        arena->total_alloc += size;

        return (void*)alig_ptr;
    }

    ArenaChunk* next_chunk = NULL;
    size_t alloc_cap = (size > arena->chunk_size) ? (size + alignment) : arena->chunk_size;

    if (arena->free_chunks && arena->free_chunks->capacity >= alloc_cap) {
        next_chunk = arena->free_chunks;
        arena->free_chunks = next_chunk->prev;
        next_chunk->offset = 0;
    } else {
        next_chunk = arena_chunk_create(alloc_cap);
        arena->total_cap += sizeof(ArenaChunk) + alloc_cap;
    }

    next_chunk->prev = arena->current;
    arena->current   = next_chunk;

    base_ptr = (uintptr_t)(next_chunk + 1);
    alig_ptr = arena_align_forward(base_ptr, alignment);
    padding  = alig_ptr - base_ptr;

    next_chunk->offset = size + padding;
    arena->total_alloc += size;

    return (void*)alig_ptr;
}

void* arena_alloc(Arena* arena, size_t size) {
    return arena_alloc_aligned(arena, size, ARENA_DEFAULT_ALIGNMENT);
}

void* arena_alloc_zero(Arena* arena, size_t size) {
    void* ptr = arena_alloc(arena, size);
    
    if (ARENA_LIKELY(ptr)) {
        memset(ptr, 0, size);
    }

    return ptr;
}

void* arena_realloc(Arena* arena, void* old_ptr, size_t old_size, size_t new_size) {
    if (!old_ptr) {
        return arena_alloc(arena, new_size);
    }
    
    if (new_size <= old_size) {
        return old_ptr;
    }

    ArenaChunk* chunk = arena->current;

    uintptr_t base_ptr = (uintptr_t)(chunk + 1);
    uintptr_t last_alloc_end = base_ptr + chunk->offset;
    uintptr_t old_ptr_addr   = (uintptr_t)old_ptr;

    if (old_ptr_addr + old_size == last_alloc_end) {
        size_t diff = new_size - old_size;

        if (chunk->offset + diff <= chunk->capacity) {
            chunk->offset += diff;
            arena->total_alloc += diff;
            return old_ptr;
        }
    }

    void* new_ptr = arena_alloc(arena, new_size);

    memcpy(new_ptr, old_ptr, old_size);
    
    return new_ptr;
}

void* arena_memdup(Arena* arena, const void* src, size_t size) {
    if (!src || size == 0) {
        return NULL;
    }

    void* dst = arena_alloc(arena, size);
    
    memcpy(dst, src, size);
    
    return dst;
}

char* arena_strdup(Arena* arena, const char* str) {
    if (!str) {
        return NULL;
    }
    
    size_t len = strlen(str);
    
    return arena_strndup(arena, str, len);
}

char* arena_strndup(Arena* arena, const char* str, size_t len) {
    if (!str) {
        return NULL;
    }

    char* dst = (char*)arena_alloc(arena, len + 1);
    
    memcpy(dst, str, len);
    
    dst[len] = '\0';
    
    return dst;
}

char* arena_sprintf(Arena* arena, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list args_copy;
    va_copy(args_copy, args);

    int len = vsnprintf(NULL, 0, fmt, args_copy);

    va_end(args_copy);

    if (ARENA_UNLIKELY(len < 0)) {
        va_end(args);
        return NULL;
    }

    char* buf = (char*)arena_alloc(arena, (size_t)len + 1);

    vsnprintf(buf, (size_t)len + 1, fmt, args);

    va_end(args);

    return buf;
}

void arena_reset(Arena* arena) {
    assert(arena != NULL);
    
    if (!arena->current) {
        return;
    }

    while (arena->current->prev) {
        ArenaChunk* prev = arena->current->prev;
        
        if (arena->current->capacity > arena->chunk_size) {
            arena->total_cap -= (sizeof(ArenaChunk) + arena->current->capacity);
            arena_chunk_free(arena->current);
        } else {
            arena->current->prev = arena->free_chunks;
            arena->free_chunks   = arena->current;
        }
        arena->current = prev;
    }

    arena->current->offset = 0;
    arena->total_alloc     = 0;
}

void arena_destroy(Arena* arena) {
    if (!arena) {
        return;
    }

    ArenaChunk* chunk = arena->current;

    while (chunk) {
        ArenaChunk* prev = chunk->prev;
        arena_chunk_free(chunk);

        chunk = prev;
    }

    ArenaChunk* free_chunk = arena->free_chunks;

    while (free_chunk) {
        ArenaChunk* prev = free_chunk->prev;
        arena_chunk_free(free_chunk);

        free_chunk = prev;
    }

    arena->current     = NULL;
    arena->free_chunks = NULL;
    arena->total_alloc = 0;
    arena->total_cap   = 0;
}

ArenaTemp arena_temp_begin(Arena* arena) {
    assert(arena != NULL);

    return (ArenaTemp){
        .arena  = arena,
        .chunk  = arena->current,
        .offset = arena->current ? arena->current->offset : 0
    };
}

void arena_temp_end(ArenaTemp temp) {
    Arena* arena = temp.arena;
    assert(arena != NULL);

    while (arena->current != temp.chunk) {
        ArenaChunk* prev = arena->current->prev;
        
        if (arena->current->capacity > arena->chunk_size) {
            arena->total_cap -= (sizeof(ArenaChunk) + arena->current->capacity);
            arena_chunk_free(arena->current);
        } else {
            arena->current->prev = arena->free_chunks;
            arena->free_chunks   = arena->current;
        }
        arena->current = prev;
    }

    if (arena->current) {
        arena->current->offset = temp.offset;
    }
}

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
#   include <threads.h>
#   define ARENA_THREAD_LOCAL thread_local
#elif defined(__GNUC__) || defined(__clang__)
#   define ARENA_THREAD_LOCAL __thread
#elif defined(_MSC_VER)
#   define ARENA_THREAD_LOCAL __declspec(thread)
#else
#   define ARENA_THREAD_LOCAL
#endif

static ARENA_THREAD_LOCAL Arena  tl_scratch_arenas[2];
static ARENA_THREAD_LOCAL bool   tl_scratch_inited = false;

ArenaTemp arena_scratch_get(Arena** conflicts, size_t conflict_count) {
    if (ARENA_UNLIKELY(!tl_scratch_inited)) {
        arena_init(&tl_scratch_arenas[0], ARENA_DEFAULT_CHUNK_SIZE);
        arena_init(&tl_scratch_arenas[1], ARENA_DEFAULT_CHUNK_SIZE);
        tl_scratch_inited = true;
    }

    for (size_t i = 0; i < 2; ++i) {
        bool has_conflict = false;
        
        for (size_t j = 0; j < conflict_count; ++j) {
            if (&tl_scratch_arenas[i] == conflicts[j]) {
                has_conflict = true;
                break;
            }
        }

        if (!has_conflict) {
            return arena_temp_begin(&tl_scratch_arenas[i]);
        }
    }

    return arena_temp_begin(&tl_scratch_arenas[0]);
}

void arena_scratch_release(ArenaTemp scratch) {
    arena_temp_end(scratch);
}