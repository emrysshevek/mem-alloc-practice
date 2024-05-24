#include "umem.h"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <bits/mman-linux.h>

#define LOG (false)
#define logPrint(...) if (LOG) {fprintf(stderr, "[%*.*s]\t", 12, 12, __func__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");}
#define MAGIC (11235813)
#define hsize (sizeof(header))
#define fsize (sizeof(size_t))
#define hfsize (hsize + fsize)
#define usedhsize (hsize - (2 * sizeof(header*)))
#define usedhfsize (usedhsize + fsize)

typedef struct _header {
  size_t sf;              // 8 bytes
  size_t magic;           // 8 bytes
  struct _header *next;   // 8 bytes (only when block is free)
  struct _header *prev;   // 8 bytes (only when block is free)
} header;                 // total: 32 bytes (multiple of 8)

int ALGORITHM = FIRST_FIT;
void *BASE;
size_t TOTAlSIZE;
header *ROOT = NULL;
header *CURR = NULL;

//  UTILITY FUNCTIONS

bool checkmagic(header *h){
  return h->magic == MAGIC;
}

/*
  avail and req are assumed to be the total size of the blocks in bytes including headers/footers
  returns:
    -1 if avail does not fit request
    0 if avail fits the request exactly
    1 if avail fits the request with some padding
    2 if avail fits the request and a new block
*/
int cmpsize(size_t avail, size_t req){
  // avail = avail + (hsize - usedhsize); // account for change in header size
  if (avail < req) return -1; // does not fit request
  else if (avail > req) { // fits the request with extra space...
    if (avail - hfsize < req) return 1; // ...but not enough for a new free block
    else return 2; // ...and a new free block
  }
  else return 0; // fits the request exactly (no padding needed)
}

size_t makefooter(size_t size, int free) {
  return size >> 1 << 1 | free;
}

size_t splitsize(size_t sf) {
  return sf >> 1 << 1;
}

int splitfree(size_t sf) {
  return (int) (sf - splitsize(sf));
}

size_t getsize(header *h) {
  return splitsize(h->sf);
}

int getfree(header *h) {
  return splitfree(h->sf);
}

int gethsize(header *h){
  return getfree(h) ? hsize : usedhsize;
}

/*
  returns the total size in bytes including header/footer
*/
size_t blocksize(header *h){
  assert(checkmagic(h));
  return getsize(h) + gethsize(h) + fsize;
}

size_t *getfooter(header *h) {
  assert(checkmagic(h));
  return (size_t*) (((char*) h + blocksize(h)) - fsize);
}

size_t *getprevfooter(header *h) {
  return ((size_t*)h) - 1;
}

void setfooter(header *h) {
  assert(checkmagic(h));
  *(getfooter(h)) = makefooter(getsize(h), getfree(h));
}

void setsize(header *h, size_t size) {
  h->sf = makefooter(size, splitfree(h->sf));
  *getfooter(h) = h->sf;
}

void setfree(header *h, int free) {
  int changed = getfree(h) != free;
  if (changed) {
    int diff = free ? hsize-usedhsize : usedhsize-hsize;
    size_t sf = makefooter(getsize(h) - diff, free);
    h->sf = sf;
    *getfooter(h) = sf;
  }
}

int gethsizefromfoot(size_t sf){
  return splitfree(sf) ? hsize : usedhsize;
}

size_t blocksizefromfoot(size_t sf){
  return splitsize(sf) + gethsizefromfoot(sf) + fsize;
}

header makeheader(size_t size, int free, header* next, header *prev){
  header h = {
    .sf = makefooter(size, free),
    .magic = MAGIC,
    .next = next,
    .prev = prev
  };
  return h;
} 

header *getheaderfromptr(void *ptr){
  // assumes that the ptr is used
  header *h = (header*) ((char*) ptr - (usedhsize));
  if (checkmagic(h)) return h;
  else return NULL;
}

header *getheaderfromfooter(void *fptr) {
  size_t sf = *((size_t*) fptr);
  header *h = (header*) ((char*)fptr - blocksizefromfoot(sf));
  assert(checkmagic(h));
  return h;
}

void *getptr(header *h){
  assert(checkmagic(h));
  return (void*) ((char*) h + gethsize(h));
}

header *getnextbyptr(header *h){
  assert(checkmagic(h));
  header *hnext = h->next;
  assert(h->next == NULL || checkmagic(hnext));
  return hnext;
}

header *getnextbysize(header *h){
  assert(checkmagic(h));
  header *hnext = (header*)((char*)h + blocksize(h));
  assert((char*)hnext <= (char*)BASE + TOTAlSIZE); // should not ever go outside the bounds
  if ((char*)hnext == (char*) BASE + TOTAlSIZE) {
    return NULL; // h was at the very end of the memory space
  }
  return hnext;
}

header *getprevbyptr(header *h){
  assert(checkmagic(h));
  header *hprev = h->prev;
  assert(hprev == NULL || checkmagic(hprev));
  return hprev;
}

header *getprevbysize(header *h) {
  assert(checkmagic(h));
  if (h == BASE) {
    return NULL;
  }
  size_t prevsf = *getprevfooter(h);
  size_t prevsize = splitsize(prevsf);
  size_t prevfree = splitfree(prevsf);
  prevsize = prevfree ? prevsize + hfsize : prevsize + usedhfsize;
  header *hprev = (header*) ((char*)h - prevsize);
  assert(checkmagic(hprev));
  return hprev;
}

void updatenextblock(header *h, header *new){
  assert(checkmagic(h));
  header *hnext = getnextbyptr(h);
  if (hnext != NULL) {
    assert(checkmagic(hnext));
    hnext->prev = new;
  }
}

void updateprevblock(header *h, header *new){
  assert(checkmagic(h));
  header *hprev = getprevbyptr(h);
  if (hprev != NULL){
    assert(checkmagic(hprev));
    hprev->next = new;
  } 
}

header *getroot(header *h){
  header *node = h;
  while (getprevbyptr(node) != NULL) {
    node = getprevbyptr(node);
  }
  return node;
}

int alignbytes(int n, int bytes) {
  int diff = (bytes - (n % bytes)) % bytes;
  return n + diff;
}

void updateroot(header *h) {
  assert(checkmagic(h));
  if (h < ROOT) {
    ROOT = h;
  }
}

void insertafter(header *h, header *hprev) {
  header *hnext = getnextbyptr(hprev);
  h->next = hnext;
  if (hnext != NULL) hnext->prev = h;

  hprev->next = h;
  h->prev = hprev;
}

void insertbefore(header *h, header *hnext) {
  header *hprev = getprevbyptr(hnext);
  h->prev = hprev;
  if (hprev != NULL) hprev->next = h;

  hnext->prev = h;
  h->next = hnext;
  if (ROOT == hnext) ROOT = h;
}

void addtofree(header *h) {
  if (ROOT == NULL) {
    ROOT = h;
    h->next = NULL;
    h->prev = NULL;
    return;
  }
  // h->prev and h->next might be invalid at this point 
  // so we can't use them here
  header *hprev = getprevbysize(h);
  if (hprev != NULL && getfree(hprev)) {
    insertafter(h, hprev);
    return;
  }
  header *hnext = getnextbysize(h);
  if (hnext != NULL && getfree(hnext)){
    insertbefore(h, hnext);
    return;
  }

  while (hprev != NULL) {
    if (getfree(hprev)) {
      insertafter(h, hprev);
      return;
    }
    hprev = getprevbysize(hprev);
    assert(hprev == NULL || checkmagic(hprev));
  }
  while (hnext != NULL) {
    if (getfree(hnext)) {
      insertbefore(h, hnext);
      return;
    }
    hnext = getnextbysize(hnext);
    assert(hnext == NULL || checkmagic(hnext));
  }

  // If we get here then something went very wrong...
  // There is a free list with a ROOT but we are unable to find
  // it for some reason
  assert(false);
}

void removefromfree(header *h) {
  assert(checkmagic(h));
  header *hprev = getprevbyptr(h);
  header *hnext = getnextbyptr(h);
  if (hnext != NULL) {
    assert(checkmagic(hnext));
    hnext->prev = hprev;
  }
  if (hprev != NULL) {
    assert(checkmagic(hprev));
    hprev->next = hnext;
  }

  if (ROOT == h) {
    ROOT = hnext;
  }
}

header* coalesce(header *first, header *second) {
  if (first == NULL){
    return second;
  }
  if (second == NULL){
    return first;
  }

  if (getfree(first) && getfree(second)) {
    assert(first->next == second && second->prev == first);
    header new = makeheader(
      blocksize(first) + blocksize(second) - hfsize,
      true,
      second->next,
      first->prev
    );
    *first = new;
    setfooter(first);

    // reconnect following block to coalesced block
    header *hnext = getnextbyptr(first);
    if (hnext != NULL) hnext->prev = first;

    if (ROOT == second) ROOT = first;
    if (ALGORITHM == NEXT_FIT && CURR == second) CURR = first;

  }
  return first;
}

//  MAIN FUNCTIONS

/*
  umeminit:
  - request memory region of specified size and save addr to head
  - create header with size (minus header size)
  - write header to start of memory region
  - save allocation algorithm
*/
int umeminit(size_t sizeOfRegion, int allocationAlgo){
  // Parameter checking
  if (sizeOfRegion == 0){
    logPrint("Error: sizeOfRegion should be > 0.");
    return -1;
  }

  if (ROOT != NULL){
    logPrint("Error: umeminit called but memory has already been allocated.");
    return -1;
  }

  // adjust sizeOfRegion to be a multiple of the page size
  int page_size = getpagesize();
  TOTAlSIZE = sizeOfRegion = alignbytes(sizeOfRegion + hfsize, page_size);


  // Request memory from OS and save ptr at start of free list
  BASE = mmap(NULL, TOTAlSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (BASE == MAP_FAILED) { perror("mmap"); exit(1); }
  ROOT = CURR = (header*) BASE;
  CURR = ROOT;

  // Initialize free list by writing first header
  header headblk = makeheader(
    TOTAlSIZE - hfsize,
    true,
    NULL,
    NULL
  );
  memcpy(ROOT, &headblk, sizeof(header));
  *getfooter(ROOT) = TOTAlSIZE - hfsize;

  // save allocation strategy
  ALGORITHM = allocationAlgo;

  return 0;
}

/*
  umalloc:
  - find open spot in list
  - write two new headers:
    - one for the requested memory
    - one for the shrunken free space
  - count over header to return pointer to empty space
*/
header *getfirstfit(size_t size){
  header *h = ROOT;

  // check for free block until end of free list
  while (h != NULL) {

    // Block should have at least enough space for the requested block
    if (getfree(h) && cmpsize(blocksize(h), size + usedhfsize) > -1){
      return h;
    }

    h = getnextbyptr(h);
  }
  return h;
}

header *getnextfit(size_t size){
  header *h = CURR;

  // check for free block until end of free list
  do  {
    // Block should have at least enough space for the requested block
    if (getfree(h) && cmpsize(blocksize(h), size + usedhfsize) > -1){
      return h;
    }

    h = getnextbyptr(h);

    if (h == NULL) {
      h = ROOT;
    }
  } while (h != CURR);

  return h;
}

header *getbestfit(size_t size){
  header *h = ROOT;

  size_t smallestdiff = TOTAlSIZE;
  header *bestfit = NULL;

  // check for free block until end of free list
  while (h != NULL) {
    size_t diff = blocksize(h) - size + usedhfsize;
    if (diff < smallestdiff){
      smallestdiff = diff;
      bestfit = h;
    }
    h = getnextbyptr(h);
  }
  return bestfit;
}

header *getworstfit(size_t size){
  header *h = ROOT;

  size_t biggestdiff = 0;
  header *worstfit = NULL;

  // check for free block until end of free list
  while (h != NULL) {
    if (cmpsize(blocksize(h), size + usedhfsize) >= 0) {
      size_t diff = blocksize(h) - (size + usedhfsize);
      if (worstfit == NULL || diff > biggestdiff){
        biggestdiff = diff;
        worstfit = h;
      }
      h = getnextbyptr(h);
    }
  }
  return worstfit;
}

void *umalloc(size_t size){
  if (BASE == NULL) {
    return NULL;
  }
  if (size <= 0) {
    return NULL;
  }

  // Minimum size is the width of the prev ptr + next ptr
  // this way a used block can always be free'd without 
  // changing the size of the block.
  if (size < hsize - usedhsize) size = hsize - usedhsize;
  // Also ensure that each pointer is aligned on 8-byte boundaries.
  size = alignbytes(size, 8);
  assert(size % 8 == 0);

  // Get next block based on allocation algorithm
  header *h;
  switch (ALGORITHM)
  {
  case FIRST_FIT:
    h = getfirstfit(size);
    break;
  case NEXT_FIT:
    h = getnextfit(size);
    break;
  case BEST_FIT:
    h = getbestfit(size);
    break;
  case WORST_FIT:
    h = getworstfit(size);
    break;
  default:
    break;
  }

  // NULL indicates there was not enough space for the request
  if (h == NULL){
    return NULL;
  }
  
  // Set requested block as not free FIRST in order to 
  // account for header size changing 
  header *hnext = getnextbyptr(h);
  header *hprev = getprevbyptr(h);

  int cmp = cmpsize(blocksize(h), size + usedhfsize);

  // Block fits the request and a new block 
  if (cmp == 2) {

    // create new requested block header
    header requested = makeheader(
      size,
      false,
      NULL,
      NULL
    );

    // create new free block header
    header newfree = makeheader(
      blocksize(h) - blocksize(&requested) - hfsize,
      true,
      h->next,
      h->prev
    );

    assert(blocksize(h) == blocksize(&requested) + blocksize(&newfree));

    // place headers in memory and add footers
    header *reqptr = h;
    *reqptr = requested;
    setfooter(reqptr);

    header *freeptr = getnextbysize(reqptr);
    *freeptr = newfree;
    setfooter(h);

    // Replace requested block with new block in free list
    if (hnext != NULL) hnext->prev = freeptr;
    if (hprev != NULL) hprev->next = freeptr;
    if (h == ROOT) ROOT = freeptr;
    if (ALGORITHM == NEXT_FIT && reqptr == CURR) {
      CURR = freeptr;
    }
  }
  else { 
    // block fits only the request. If there is extra space, it will 
    // remain as padding

    // set block to used (also updates size to account for change in headers)
    setfree(h, false);

    // remove requested block from free list
    if (hnext != NULL) hnext->prev = hprev;
    if (hprev != NULL) hprev->next = hnext;
    if (h == ROOT) ROOT = hnext;
    if (ALGORITHM == NEXT_FIT && CURR == h){
      CURR = getnextbyptr(h);
    }
  }

  return getptr(h);
}

/*
  ufree:
  - count backwards from pointer to get start of header
  - set free to true
  - look to coalesce with blocks before and after
    - use prev pointer to get block before
    - use size to count to block after
    - update size of earlier block to sum of both
*/
int ufree(void *ptr) {
  if (BASE == NULL) {
    return -1;
  }
  if (ptr == NULL) {
    return 0;
  }

  header *h = getheaderfromptr(ptr);

  if (h == NULL) {
    // pointer is corrupted or not a valid pointer
    logPrint("Invalid ptr");
    return -1;
  }
  if (getfree(h)){
    // pointer is already free
    logPrint("Double free");
    return -1;
  }

  setfree(h, true);
  addtofree(h);

  // coalesce adjacent blocks
  h = coalesce(h, getnextbysize(h));
  h = coalesce(getprevbysize(h), h);
  
  return 0;
}

/*
  umemdump:
  - iterate over linked list (using size info) of blocks
  - print free block sizes and addresses
  format: '[block number]\t[address]\t[size]\t[free]'
*/
void 	umemdump(){
  int n = 0;
  header *block = ROOT;
  while (block != NULL) {
    printf("%d\t%p\t%ld\t%d\n", n++, block, getsize(block), getfree(block));
    block = getnextbyptr(block);
  }
  fflush(stdout);
}