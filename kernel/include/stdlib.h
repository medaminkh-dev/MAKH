/**
 * MakhOS - stdlib.h
 * Standard Library header
 * Provides standard C library functionality for the kernel
 */

#ifndef MAKHOS_STDLIB_H
#define MAKHOS_STDLIB_H

#include <types.h>

/* Include kheap for memory allocation functions */
#include <mm/kheap.h>

/* Map standard malloc/calloc/realloc/free to kheap functions */
#define malloc(size) kmalloc(size)
#define calloc(num, size) kcalloc(num, size)
#define realloc(ptr, size) krealloc(ptr, size)
#define free(ptr) kfree(ptr)

/* POSIX exit codes */
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

/* String conversion functions */
int atoi(const char* str);
long atol(const char* str);

/* Integer absolute value */
#define abs(x) ((x) < 0 ? -(x) : (x))
#define labs(x) ((x) < 0 ? -(x) : (x))

/* Random number generation (simple LCG) */
#define RAND_MAX 32767
int rand(void);
void srand(unsigned int seed);

/* Program termination */
void exit(int status);
void abort(void);

/* Searching and sorting */
void* bsearch(const void* key, const void* base, size_t nmemb, size_t size,
              int (*compar)(const void*, const void*));
void qsort(void* base, size_t nmemb, size_t size,
           int (*compar)(const void*, const void*));

#endif /* MAKHOS_STDLIB_H */
