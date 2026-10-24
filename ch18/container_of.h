// SPDX-License-Identifier: MIT
/*
 * container_of.h - get from a pointer to a member back to the structure
 * that contains it.  A teaching version of the kernel's macro; needs GCC
 * or Clang (statement expressions, typeof, __builtin_types_compatible_p).
 */
#ifndef CONTAINER_OF_H
#define CONTAINER_OF_H

#include <stddef.h>	/* offsetof */

/* True at compile time if a and b have the same type. */
#define same_type(a, b) \
	__builtin_types_compatible_p(typeof(a), typeof(b))

/*
 * container_of(ptr, type, member): ptr points at the field @member inside
 * some @type; return a pointer to that @type.  The static assertion makes
 * the compiler reject a @ptr whose type does not match the field.
 */
#define container_of(ptr, type, member) ({				\
	_Static_assert(same_type(*(ptr), ((type *)0)->member),		\
		       "pointer type mismatch in container_of()");	\
	(type *)((char *)(ptr) - offsetof(type, member)); })

/*
 * min(x, y) that evaluates each argument once and refuses to compare
 * values of different types.  (The kernel's rule is more permissive: it
 * rejects only comparisons that mix signed and unsigned values.)
 */
#define min(x, y) ({							\
	typeof(x) _x = (x);						\
	typeof(y) _y = (y);						\
	_Static_assert(same_type(_x, _y), "min() of different types");	\
	_x < _y ? _x : _y; })

#endif
