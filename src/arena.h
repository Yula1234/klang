#ifndef KLANG_ARENA_H
#define KLANG_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#   include <stdalign.h>
#elif !defined(alignof)
#   if defined(__GNUC__) || defined(__clang__)
#       define alignof(T) __alignof__(T)
#   elif defined(_MSC_VER)
#       define alignof(T) __alignof(T)
#   else
#       define alignof(T) sizeof(T)
#   endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__)
#   define ARENA_DEFAULT_ALIGNMENT 16
#else
#   define ARENA_DEFAULT_ALIGNMENT 8
#endif

#define ARENA_DEFAULT_CHUNK_SIZE (4 * 1024 * 1024)

typedef struct ArenaChunk ArenaChunk;

struct ArenaChunk {
    ArenaChunk* prev;
    size_t      capacity;
    size_t      offset;
};

typedef struct Arena {
    ArenaChunk* current;
    ArenaChunk* free_chunks;
    size_t      chunk_size;
    size_t      total_alloc;
    size_t      total_cap;
} Arena;

typedef struct ArenaTemp {
    Arena*      arena;
    ArenaChunk* chunk;
    size_t      offset;
} ArenaTemp;

void   arena_init(Arena* arena, size_t chunk_size);
Arena* arena_create(size_t chunk_size);

void   arena_reset(Arena* arena);
void   arena_destroy(Arena* arena);

void* arena_alloc_aligned(Arena* arena, size_t size, size_t alignment);
void* arena_alloc(Arena* arena, size_t size);
void* arena_alloc_zero(Arena* arena, size_t size);
void* arena_realloc(Arena* arena, void* old_ptr, size_t old_size, size_t new_size);

void* arena_memdup(Arena* arena, const void* src, size_t size);
char* arena_strdup(Arena* arena, const char* str);
char* arena_strndup(Arena* arena, const char* str, size_t len);
char* arena_sprintf(Arena* arena, const char* fmt, ...);

ArenaTemp arena_temp_begin(Arena* arena);
void      arena_temp_end(ArenaTemp temp);

ArenaTemp arena_scratch_get(Arena** conflicts, size_t conflict_count);
void      arena_scratch_release(ArenaTemp scratch);

#define ARENA_NEW(arena_ptr, Type) \
    ((Type*)arena_alloc_aligned((arena_ptr), sizeof(Type), alignof(Type)))

#define ARENA_NEW_ZERO(arena_ptr, Type) \
    ((Type*)memset(arena_alloc_aligned((arena_ptr), sizeof(Type), alignof(Type)), 0, sizeof(Type)))

#define ARENA_NEW_ARRAY(arena_ptr, Type, count) \
    ((Type*)arena_alloc_aligned((arena_ptr), sizeof(Type) * (count), alignof(Type)))

#define ARENA_NEW_ARRAY_ZERO(arena_ptr, Type, count) \
    ((Type*)memset(arena_alloc_aligned((arena_ptr), sizeof(Type) * (count), alignof(Type)), 0, sizeof(Type) * (count)))

#define ARENA_DA_PUSH(arena_ptr, arr, count, cap, item) \
    do { \
        if ((count) >= (cap)) { \
            size_t _old_cap = (cap); \
            size_t _new_cap = (_old_cap == 0) ? 4 : (_old_cap * 2); \
            (arr) = arena_realloc((arena_ptr), (arr), \
                                  _old_cap * sizeof(*(arr)), \
                                  _new_cap * sizeof(*(arr))); \
            (cap) = _new_cap; \
        } \
        (arr)[(count)++] = (item); \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif // KLANG_ARENA_H