#ifndef CONFIG_H
#define CONFIG_H

#define HAVE_STDINT_H 1

#ifdef _MSC_VER
#define _USE_MATH_DEFINES
#include <math.h>
#include <malloc.h>
/* MSVC doesn't support C99 VLAs - use _alloca instead */
#define VLA_ARRAY(type, name, size) type *name = (type *)_alloca(sizeof(type) * (size))
#endif

#endif
