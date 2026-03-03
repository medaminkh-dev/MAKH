/**
 * MakhOS - types.h
 * Standard type definitions for kernel
 */

#ifndef TYPES_H
#define TYPES_H

/* Fixed-width integer types */
typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;

typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;

/* Size types */
typedef unsigned long       size_t;
typedef long                ssize_t;

/* Pointer-sized integers */
typedef unsigned long       uintptr_t;
typedef long                intptr_t;

/* Boolean type */
typedef enum { false = 0, true = 1 } bool;

/* NULL pointer */
#ifndef NULL
#define NULL ((void*)0)
#endif

/* Memory address types */
typedef uint64_t paddr_t;   /* Physical address */
typedef uint64_t vaddr_t;   /* Virtual address */

#endif /* TYPES_H */
