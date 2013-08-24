/* Copyright (c) 2013 Michele Bini
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the version 3 of the GNU General Public License
 * as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>. */


#define __USE_GNU
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <search.h>

static void*  (*real_malloc)(size_t)            = NULL;
static void*  (*real_calloc)(size_t, size_t)    = NULL;
static void*  (*real_realloc)(void*,size_t)     = NULL;
static void   (*real_free)(void*)               = NULL;
static void   (*real_cfree)(void*)              = NULL;
static void*  (*real_memalign)(size_t, size_t)  = NULL;
static void*  (*real_valloc)(size_t)            = NULL;
static void*  (*real_pvalloc)(size_t)           = NULL;

static void*getsym(char*name) {
  void*f = dlsym(RTLD_NEXT, name);
  if (NULL == f) {
    fprintf(stderr, "Error in `dlsym(RTLD_NEXT, \"%s\")`: %s\n", name, dlerror());
    exit(EXIT_FAILURE);
  }
  return f;
}

typedef struct {
  void *raw;
  size_t size;
} pointer_wrap_t;

static int ptrcmp (const void *a, const void *b) { return (a > b) - (a < b); }
static int cmp(const void *a, const void *b) {
  void*m = *((void**)a);
  void*n = *((void**)b);
  return ptrcmp(m, n);
}

static int malloc_nest = 0;
static void*malloc_tracked_pointers = NULL;
static size_t malloc_tracked_size;
static void*malloc_tracked_pointer_unique;

static void init(void)
{
  static int initialized = 0;
  if (initialized) return;
  real_malloc    = getsym("malloc");
  // real_calloc    = getsym("calloc");
  real_realloc   = getsym("realloc");
  real_free      = getsym("free");
  // real_cfree     = getsym("cfree");
  // real_memalign  = getsym("memalign");
  // real_valloc    = getsym("valloc");
  // real_pvalloc   = getsym("pvalloc");

  // Add unique element that is never going to be matched and removed so the size of the tree will never become zero and tdelete will always return meaningful values
  malloc_tracked_pointer_unique = &malloc_tracked_pointers;
  tsearch(&malloc_tracked_pointer_unique, &malloc_tracked_pointers, cmp);

  initialized = 1;
}

void *malloc(size_t size)
{
  init();
  if (malloc_nest) {
    return real_malloc(size);
  }
  malloc_nest = 1;
  
  void *p = NULL;
  fprintf(stderr, "malloc(%d) -> ", size);
  p = real_malloc(size);
  fprintf(stderr, "%p\n", p);

  malloc_nest = 0;
  return p;
}

void *realloc(void*ptr, size_t size)
{
  init();
  if (malloc_nest) {
    return real_malloc(size);
  }
  malloc_nest = 1;
  
  fprintf(stderr, "realloc(%p, %d) -> ", ptr, size);
  void*newptr = real_realloc(ptr, size);
  fprintf(stderr, "%p\n", newptr);

  malloc_nest = 0;
  return newptr;
}

void free(void*ptr) {
  init();
  if (malloc_nest) {
    return real_free(ptr);
  }
  malloc_nest = 1;

  tdelete(&ptr, &malloc_pointers, cmp);

  fprintf(stderr, "free(%p)\n", ptr);
  real_free(ptr);
  
  malloc_nest = 0;
}
