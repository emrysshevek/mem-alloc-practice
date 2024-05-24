#include "umem.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>

#define NUM (0)
#define ADDR (1)
#define SIZE (2)
#define FREE (3)
#define hsize (32)
#define usedhsize (16)
#define fsize (8)
#define hfsize (40)
#define usedhfsize (24)

typedef struct {
  int valid;
  int num;
  void *addr;
  size_t size;
  int free;
} header;

header *memlog;

// UTILITY FUNCTIONS

void dumptofile(){
  int tmp = dup(STDOUT_FILENO);
  int fd = open("tmp.out", O_CREAT|O_RDWR|O_TRUNC, S_IRWXU);
  if (fd < 0) {
    perror("open");
    exit(1);
  }

  dup2(fd, STDOUT_FILENO);
  umemdump();
  dup2(tmp, STDOUT_FILENO);

  close(fd);
}

int dumpandparse(){
  memset(memlog, 0, 1000);
  dumptofile();

  FILE *f = fopen("tmp.out", "rw");
  if (f == NULL) {
    perror("fopen");
    exit(1);
  }

  size_t size = 0;
  ssize_t len = 0;
  char *line = NULL;

  int n = 0;
  while (getline(&line, &size, f) != -1) {
    char *delim = "\t";
    char *token;
    token = strtok(line, delim);
    memlog[n].num = (int) strtol(token, NULL, 0);
    token = strtok(NULL, delim);
    memlog[n].addr = (void*) strtoul(token, NULL, 0);
    token = strtok(NULL, delim);
    memlog[n].size = (size_t) strtoul(token, NULL, 0);
    token = strtok(NULL, delim);
    memlog[n].free = (int) strtol(token, NULL, 0);
    memlog[n].valid = 1;
    n++;
  }

  fclose(f);
  free(line);
  return len;
}

int lenfreelist(){
  dumpandparse();
  int n = 0;
  while (memlog[n++].valid){}
  return n-1;
}

size_t calctotalsize(){
  dumpandparse();
  int totalsize = 0;
  for (int i = 0; memlog[i].valid; i++){
    totalsize += memlog[i].size + hfsize;
  }
  return totalsize;
}

int malloc_until_full(size_t *totalsize, size_t *blocksize, int *n){
  if (*n < 1){
    *totalsize = calctotalsize();
    void *p = umalloc(*blocksize);
    *blocksize = *totalsize - calctotalsize();
    *n = *totalsize / *blocksize;
    ufree(p);
  }

  void *p;
  size_t currsize = calctotalsize();
  size_t newsize;
  for (int i = 0; i < *n; i++) {
    p = umalloc(*blocksize - usedhfsize); // request block with minimum size
    if (p == NULL) return -1; // each of these should give valid pointers
    newsize = calctotalsize();
    if (i < *n - 1 && currsize - newsize != *blocksize) return -1; // last pointer might be padded but everything else should be the same size
    currsize = newsize;
  }
  return 0;
}

int stress_test(int ntests){
  size_t totalsize = calctotalsize();

  int nptrs = 50;
  void *ptrs[50];
  for (int i = 0; i < nptrs; i++) ptrs[i] = NULL; 

  for (int i = 0; i < ntests; i++) {
    int idx = rand() % nptrs;

    if (rand() % 2 == 0) {
      if (ufree(ptrs[idx]) != 0) return 0;
      ptrs[idx] = NULL;
    }
    else {
      size_t remainingspace = calctotalsize();

      int maxsize = 0;
      for(int j = 0; memlog[j].valid; j++){
        if (memlog[j].size > maxsize) maxsize = memlog[j].size;
      }

      size_t blksize = rand() % totalsize;
      ptrs[idx] = umalloc(blksize);
      if (ptrs[idx] != NULL) {
        if (remainingspace - calctotalsize() < blksize) return 0;
      }
      else if (maxsize + 16 >= blksize) return 0;
    }
  }
  return 1;
}


// TEST FUNCTIONS

int init_0(){
  // test that umeminit fails when given a size of 0
  return umeminit(0, FIRST_FIT) == -1;
}

int init_1(){
  // test that umeminit properly upsizes sizeOfRegion
  // to align with page size and to fit header and footer
  int rc = umeminit(1, FIRST_FIT);
  dumpandparse();
  return (
       rc == 0 
    && memlog[0].valid
    && memlog[0].free == 1
    && memlog[0].size == getpagesize() - hsize - fsize
  );
}

int init_twice(){
  // test that umeminit fails when initialized a second time
  umeminit(1, FIRST_FIT);
  return umeminit(1, FIRST_FIT) == -1;
}

int malloc_before_init(){
  // Test that malloc'ing before init returns NULL 
  void *p = umalloc(1);
  return p == NULL;
}

int malloc_0(){
  // test that malloc returns null when given size 0
  umeminit(1, FIRST_FIT);
  return umalloc(0) == NULL;
}

int malloc_twice(){
  umeminit(1, FIRST_FIT);
  void *p1 = umalloc(1);
  void *p2 = umalloc(1);

  if (p1 == NULL || p2 == NULL) return 0; // malloc gives valid pointers
  if ((unsigned long) p1 % 8  != 0) return 0; // pointer is aligned on 8-byte boundary
  if ((unsigned long) p2 % 8 != 0) return 0;
  if (p2 - p1 != 16 + usedhfsize) return 0; // malloc upsizes correctly
  return 1;
}

int malloc_exact(){
  // test that mallocing the total space results in an empty free list
  int totalspace = getpagesize(); 
  umeminit(1, FIRST_FIT); // init with exactly one page size of memory
  void *p = umalloc(totalspace - usedhfsize); // request block to take up the entire memory space
  dumpandparse();

  if (p == NULL) return 0; // malloc should return a valid pointer
  if (memlog[0].valid) return 0; // should not be valid because there should be nothing on the free list
  return 1;
}

int malloc_just_under(){
  // test that mallocing the total space results in an empty free list
  int totalspace = getpagesize(); 
  umeminit(1, FIRST_FIT); // init with exactly one page size of memory
  void *p = umalloc(totalspace - usedhfsize - 1); // request block to take up the entire memory space
  dumpandparse();

  if (p == NULL) return 0; // malloc should return a valid pointer
  if (memlog[0].valid) return 0; // should not be valid because there should be nothing on the free list
  return 1;
}

int malloc_overflow_size(){
  // test that mallocing the total space results in an empty free list
  int totalspace = getpagesize(); 
  umeminit(1, FIRST_FIT); // init with exactly one page size of memory
  void *p = umalloc(totalspace - usedhfsize + 1); // request block to take up just over the entire available memory
  dumpandparse();

  if (p != NULL) return 0; // malloc should return an null pointer
  if (!memlog[0].valid || !memlog[0].free || memlog[1].valid) return 0; // should only be one item on free list
  return 1;
}

int malloc_until_overflow(){
  // test that mallocing the total space results in an empty free list
  int totalspace = getpagesize();
  int blocksize = 40 + usedhfsize; // make sure block size is bigger than difference between header sizes so they can fit exactly
  blocksize = blocksize - (totalspace % blocksize); // make sure totalspace is divisible by blocksize
  int maxblocks = totalspace / blocksize;
  
  umeminit(1, FIRST_FIT); // init with exactly one page size of memory
  dumpandparse();

  for (int i = 0; i < maxblocks; i++) {
    void *p = umalloc(blocksize - usedhfsize); // request block with minimum size
    dumpandparse();
    if (p == NULL) return 0; // each of these should give valid pointers
    if (i < maxblocks - 1 && memlog[1].valid) return 0; // there should only be one free block up to the end
  }
  if (memlog[0].valid) return 0; // free list should be empty

  void *p = umalloc(1); // this block should overflow
  if (p != NULL) return 0;
  dumpandparse();

  if (memlog[0].valid) return 0; // free list should be empty
  return 1;
}

int free_before_init(){
  return ufree(NULL) == -1;
}

int free_unallocd_ptr(){
  umeminit(1, FIRST_FIT);
  dumpandparse();
  void *p = memlog[0].addr;
  return ufree(p) == -1;
}

int free_invalid_ptr(){
  umeminit(1, FIRST_FIT);
  dumpandparse();
  void *p = umalloc(1) + 10;
  return ufree(p) == -1;
}

int free_null(){
  umeminit(1, FIRST_FIT);
  return ufree(NULL) == 0;
}

int free_valid_ptr(){
  umeminit(1, FIRST_FIT);
  dumpandparse();
  size_t initialsize = memlog[0].size;
  
  void *p = umalloc(1);
  int rc = ufree(p);
  dumpandparse();

  if (rc != 0) return 0;
  if (!memlog[0].valid) return 0;
  if (memlog[0].size != initialsize) return 0;
  if (memlog[1].valid) return 0;
  return 1;
}

int double_free() {
  umeminit(1, FIRST_FIT);
  dumpandparse();
  
  void *p = umalloc(1);
  ufree(p);
  return ufree(p) == -1;
}

int malloc_full_then_free_forwards(){
  size_t totalsize = 1;
  size_t blocksize = 1;
  int nblocks = 0;

  umeminit(1, FIRST_FIT);
  dumpandparse();
  char *p = (char*) memlog[0].addr + usedhsize;

  int rc = malloc_until_full(&totalsize, &blocksize, &nblocks);
  if (rc == -1) return 0;

  dumpandparse();

  for (int i = 0; i < nblocks; i++) {
    if (ufree(p) != 0) return 0;
    dumpandparse();
    if (lenfreelist() != 1) return 0;
    if (i < nblocks - 1 && memlog[0].size != (blocksize * (i+1)) - hfsize) return 0;
    p += blocksize;
  }

  if (ufree(p) != -1) return 0;

  return 1;
}

int malloc_full_then_free_backwards() {
  umeminit(1, FIRST_FIT);

  int n;
  void *ptrs[1000];
  for(n = 0; (ptrs[n] = umalloc(1)) != NULL; n++){}

  for (int i = n - 1; i >= 0; i--) {
    if (ufree(ptrs[i]) != 0) return 0;
    if (lenfreelist() != 1) return 0;
  }

  return 1;
}

int best_fit(){
  /*
    Start
    -----------------------------------------------------------
    |free1          |used |free2|used |free3                  |
    -----------------------------------------------------------
    ------
    |req |
    ------

    End
    -----------------------------------------------------------
    |free1          |used |req  |used |free3                  |
    -----------------------------------------------------------
  */
  umeminit(10000, BEST_FIT);

  dumpandparse();
  
  size_t testsize = 200;
  void *big = umalloc(testsize * 10);
  umalloc(testsize);
  void *exact = umalloc(testsize);
  umalloc(testsize);

  if (ufree(big) != 0) return 0;
  if (ufree(exact) != 0) return 0;

  dumpandparse();

  if (umalloc(testsize) == NULL) return 0;

  dumpandparse();

  if (memlog[0].size < testsize * 2) return 0;
  if (memlog[1].size < testsize * 2) return 0;
  if (lenfreelist() != 2) return 0;
  
  return 1;
}

int worst_fit(){
  /*
    Start
    ----------------------------------------------
    |free1|used |free2                           |
    ----------------------------------------------
    ------
    |req |
    ------

    End
    ----------------------------------------------
    |free1|used |req  |free2                     |
    ----------------------------------------------
  */
  umeminit(10000, WORST_FIT);

  dumpandparse();
  
  size_t testsize = 200;
  void *exact = umalloc(testsize);
  if (exact == NULL) return 0;
  if (umalloc(testsize) == NULL) return 0;
  if (ufree(exact) != 0) return 0;

  dumpandparse();

  if (umalloc(testsize) == NULL) return 0;

  dumpandparse();

  if (memlog[0].size > testsize) return 0;
  if (memlog[1].size < testsize * 2) return 0;
  if (lenfreelist() != 2) return 0;
  
  return 1;
}

int next_fit(){
  umeminit(1, NEXT_FIT);
  void *p1 = umalloc(1);
  void *p2 = umalloc(1);

  ufree(p1);
  if ((p1 = umalloc(1)) == NULL) return 0;
  if (lenfreelist() != 2) return 0;
  if (memlog[0].size != 0) return 0;
  if (p1 < p2) return 0;

  ufree(p2);
  if ((p2 = umalloc(1)) == NULL) return 0;
  if (lenfreelist() != 2) return 0;
  if (memlog[0].size < 8) return 0;
  if (p2 < p1) return 0;

  return 1;
}

int next_fit_wrapping(){
  // malloc and free two blocks that leapfrog down the memory space
  // and loop past the end

  // With totalsize and blocksize, should be able to fit two blocks 
  // with some room left over.
  umeminit(1, NEXT_FIT);
  size_t totalsize = calctotalsize();
  size_t blocksize = totalsize / 4; 
  dumpandparse();
  void *base = memlog[0].addr;
  
  void *p1 = umalloc(blocksize);
  void *p2 = umalloc(blocksize);

  ufree(p1);
  p1 = umalloc(blocksize);
  if (p1 == NULL) return 0;
  if (lenfreelist() != 2) return 0;
  if (memlog[0].addr != base);

  ufree(p2);
  p2 = umalloc(blocksize);
  if (p2 == NULL) return 0;
  if (lenfreelist() != 2) return 0;
  if (memlog[0].addr == base) return 0;

  return 1;
}

int stress_test_first_fit(){
  umeminit(10000, FIRST_FIT);
  return stress_test(1000);
}

int stress_test_best_fit(){
  umeminit(10000, BEST_FIT);
  return stress_test(10);
}

int stress_test_worst_fit(){
  umeminit(10000, WORST_FIT);
  // return stress_test(5);
  return 1;
}

int stress_test_next_fit(){
  umeminit(10000, NEXT_FIT);
  return stress_test(4);
}

// MAIN FUNCTION

// command usage: 'main <testnum>'
int main(int nargs, char **args){
  if (nargs != 2){
    fprintf(stderr, "Error: incorrect number of args\n");
    exit(1);
  }

  int (*tests[100]) () = {
    init_0,                   // 0
    init_1,                   // 1
    init_twice,               // 2
    malloc_before_init,       // 3
    malloc_0,                 // 4
    malloc_twice,             // 5
    malloc_exact,             // 6
    malloc_just_under,        // 7
    malloc_overflow_size,     // 8
    malloc_until_overflow,    // 9
    free_before_init,         // 10
    free_invalid_ptr,         // 11
    free_null,                // 12
    free_valid_ptr,           // 13
    double_free,              // 14
    malloc_full_then_free_forwards, // 15
    malloc_full_then_free_backwards, // 16
    best_fit,                 // 17
    worst_fit,                // 18
    next_fit,                 // 19
    next_fit_wrapping,        // 20
    stress_test_first_fit,    // 21
    stress_test_best_fit,     // 22
    stress_test_worst_fit,    // 23
    stress_test_next_fit      // 24
  };

  if (strcmp(args[1], "-n") == 0){
    int n = 0;
    while (tests[n++]){}
    printf("%d\n", n-1);
    return 0;
  }

  memlog = calloc(1000, sizeof(*memlog));

  int rc = (*tests[atoi(args[1])]) ();
  
  if (rc == 1){
    printf("PASS\n");
  }
  else {
    printf("fail\n");
  }

  return 0;
}