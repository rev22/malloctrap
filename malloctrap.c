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
#include <malloc.h>
#include <string.h>

#define MALLOCTRAP_THREAD_SAFE
#ifdef MALLOCTRAP_THREAD_SAFE
#include <pthread.h>
#endif

/* Configuration */

/* Trace malloc calls and total allocated size */
static int malloctrap_trace = 0;

static int reset_mem = 0; /* Reset to zero newly allocated memory */
static int reset_mem_rnd = 0; /* Set random data instead of resetting allocated memory areas */

static size_t malloctrap_max = 400000; /* when >= 0, set a limit for number of malloc'ed pointers */
static size_t malloctrap_max_kill = 1; /* when zero, do not kill but return NULL when pointer limit is reached */
static size_t malloctrap_max_total_size = 150000000; /* when > 0, set a limit for malloc'ed data */
static size_t malloctrap_max_total_size_kill = 1; /* when zero, do not kill but return NULL when size limit is reached */
static size_t malloctrap_max_single_allocation = 0;
static double malloctrap_max_single_allocation_ratio = 0.15;
static int discount_realloc = 0; /* Do not track realloc'ed data */

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
  void *ptr;
  size_t size;
} wrap_t;

static int ptrcmp (const void *a, const void *b) { return (a > b) - (a < b); }
static int cmp(const void *a, const void *b) {
  void*m = *((void**)a);
  void*n = *((void**)b);
  return ptrcmp(m, n);
}

static int malloc_nest = 0;
static void*malloc_tracked_pointers = NULL;
static size_t malloc_tracked_size = 0;
static int malloc_tracked_n = 0;

int initialized = 0;

static void init(void)
{
  if (initialized) return;

  if ((!malloctrap_max_single_allocation) && (malloctrap_max_single_allocation_ratio < 1.0) && malloctrap_max_total_size) {
    malloctrap_max_single_allocation = malloctrap_max_total_size * malloctrap_max_single_allocation_ratio;
  }
   

  real_malloc    = getsym("malloc");
  // real_calloc    = getsym("calloc");
  real_realloc   = getsym("realloc");
  real_free      = getsym("free");
  // real_cfree     = getsym("cfree");
  // real_memalign  = getsym("memalign");
  // real_valloc    = getsym("valloc");
  // real_pvalloc   = getsym("pvalloc");

  initialized = 1;
}

static void check_initialized() {
  if (!initialized) {
    // This may happen if the functions are called in parallel or if getsym uses malloc-family functions
    fputs(stderr, "malloc-family function called while library was initializing\n");
    exit(EXIT_FAILURE);
  }
}

static int check_single(size_t x) {
  int r = malloctrap_max_single_allocation && (x > malloctrap_max_single_allocation);
  if (r) {
    fputs(stderr, "single allocation of %d, greater than maximum %d\n",
	  x, malloctrap_max_single_allocation);
  }
  return r;
}

static void
update_tracked_size(int x) {
  malloc_tracked_size += x;
  if (malloctrap_trace) fprintf(stderr, "%d bytes of malloc'ed data\n", malloc_tracked_size);
}

static void add(void*p, size_t size) {
  wrap_t *w = malloc(sizeof(wrap_t));
  w->ptr = p;
  update_tracked_size(w->size = size);
  malloc_tracked_n++;
  tsearch(w, &malloc_tracked_pointers, cmp);
}

static int del(void*p, size_t*s) {
  wrap_t**wp = tfind(&p, &malloc_tracked_pointers, cmp);
  if (wp != NULL) {
    wrap_t*w = wp[0];
    if (s != NULL) *s = w->size;
    update_tracked_size( -(w->size) );
    malloc_tracked_n--;
    tdelete(&p, &malloc_tracked_pointers, cmp);
    free(w);
    return 1;
  }
  return 0;
}

static void reset(void*p, size_t s) {
  if (reset_mem_rnd) {
    while (s) {
      ((char*)p)[0]=rand()&0xff;
      p++;
      s--;
    }
  } else {
    bzero(p, s);
  }
}

#ifdef MALLOCTRAP_THREAD_SAFE

static pthread_mutex_t malloctrap_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

static void malloctrap_enter() {
  pthread_mutex_lock(&malloctrap_mutex);
}

static void malloctrap_leave() {
  pthread_mutex_unlock(&malloctrap_mutex);
}

#endif

static int check_total() {
  int max;
  max = (malloctrap_max && (malloc_tracked_n > malloctrap_max));
  if (max && malloctrap_max_kill) {
    fprintf(stderr, "Malloc allocation limit reached: %d > %d pointers\n", malloc_tracked_n, malloctrap_max);
    malloctrap_leave();
    exit(EXIT_FAILURE);
  }
  if (max) {
    return max;
  }
  max = (malloctrap_max_total_size && (malloc_tracked_size > malloctrap_max_total_size));
  if (max && malloctrap_max_total_size_kill) {
    fprintf(stderr, "Malloc allocation limit reached: %d > %d bytes\n", malloc_tracked_size, malloctrap_max_total_size);
    malloctrap_leave();
    exit(EXIT_FAILURE);
  }
  return max;
}

void *malloc(size_t size)
{
#ifdef MALLOCTRAP_THREAD_SAFE
  malloctrap_enter();
#endif
  void *p;
  if (malloc_nest) {
    check_initialized();
    p = real_malloc(size);
  } else {
    malloc_nest = 1;
    init();

    p = NULL;
    if (malloctrap_trace) fprintf(stderr, "malloc(%d) -> ", size);
    if (check_single(size)) { p = NULL; goto fail; }
    p = malloc(size);
    if (malloctrap_trace) fprintf(stderr, "%p\n", p);
    
    add(p, size);
    if (reset_mem) reset(p, size);
    
    if (check_total()) {
      del(p, NULL);
      free(p);
      p = NULL;
    }
  fail:
    malloc_nest = 0;
  }
#ifdef MALLOCTRAP_THREAD_SAFE
  malloctrap_leave();
#endif
  return p;
}

void *realloc(void*ptr, size_t size)
{
#ifdef MALLOCTRAP_THREAD_SAFE
  malloctrap_enter();
#endif
  void *p;
  if (malloc_nest) {
    check_initialized();
    p = real_realloc(ptr, size);
  } else {
    malloc_nest = 1;
    init();

    size_t old_size;
    
    if (!del(ptr, &old_size)) {
      if ((size > old_size) && check_single(size - old_size)) {
	p = NULL;
	goto fail;
      }
    } else {
      old_size = size;
    }
  
    if (malloctrap_trace) fprintf(stderr, "realloc(%p, %d) -> ", ptr, size);
    p = realloc(ptr, size);
    if (malloctrap_trace) fprintf(stderr, "%p\n", p);

    if (!discount_realloc) {
      add(p, size);

      if (check_total()) {
	del(p, NULL);
	free(p);
	p = NULL;
      }

      if (reset_mem && (size > old_size)) {
	reset(p + old_size, size - old_size);
      }
    }
  fail:
    malloc_nest = 0;
  }
#ifdef MALLOCTRAP_THREAD_SAFE
  malloctrap_leave();
#endif
  return p;
}

void free(void*ptr) {
#ifdef MALLOCTRAP_THREAD_SAFE
  malloctrap_enter();
#endif
  if (malloc_nest) {
    check_initialized();
    real_free(ptr);
  } else {
    malloc_nest = 1;
    init();

    del(ptr, NULL);
  
    if (malloctrap_trace) fprintf(stderr, "free(%p)\n", ptr);
    free(ptr);
  
    malloc_nest = 0;
  }
#ifdef MALLOCTRAP_THREAD_SAFE
  malloctrap_leave();
#endif
}
