#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "kalloc.c"
#include "x86.h"

// Define swapped page flag
#define PTE_SWAPPED (1 << 7)  // Page is swapped to disk
#define PTE_A 0x020 

struct run {
  struct run *next;
};


// Define swap slot structure
struct swapslot {
  int page_perm;  // Page permissions
  int is_free;    // 1 if slot is free, 0 if occupied
};

// Define swap table
#define NSWAPSLOTS 800  // 800 swap slots as specified
struct {
  struct spinlock lock;
  struct swapslot slots[NSWAPSLOTS];
} swaptable;

// First blo    ck for swap space (after superblock)
#define SWAPBLOCK 2  // Start right after the superblock

// Adaptive page replacement strategy parameters
#ifndef ALPHA
#define ALPHA 25
#endif

#ifndef BETA
#define BETA 10
#endif

int threshold = 100;     // Initial threshold
int npages_to_swap = 2;  // Initial number of pages to swap
int alpha = ALPHA;       // Alpha value
int beta = BETA;         // Beta value
#define LIMIT 100        // Maximum number of pages to swap

// Initialize swap table
void swapinit(void)
{
  int i;
  initlock(&swaptable.lock, "swaptable");
  
  // Initialize all swap slots to free
  for(i = 0; i < NSWAPSLOTS; i++){
    swaptable.slots[i].is_free = 1;
    swaptable.slots[i].page_perm = 0;
  }
  
  cprintf("Swap initialization done: %d slots\n", NSWAPSLOTS);
}

// Find a free swap slot
int allocswapslot(void)
{
  int i;
  
  acquire(&swaptable.lock);
  for(i = 0; i < NSWAPSLOTS; i++){
    if(swaptable.slots[i].is_free){
      swaptable.slots[i].is_free = 0;
      release(&swaptable.lock);
      return i;
    }
  }
  release(&swaptable.lock);
  return -1;  // No free slot found
}

// Free a swap slot
void freeswapslot(int slot)
{
  if(slot < 0 || slot >= NSWAPSLOTS)
    panic("freeswapslot: invalid slot");
    
  acquire(&swaptable.lock);
  swaptable.slots[slot].is_free = 1;
  swaptable.slots[slot].page_perm = 0;
  release(&swaptable.lock);
}

// Get the disk block number for a swap slot
uint swapslotblockno(int slot)
{
  return SWAPBLOCK + slot * 8;
}

// Read a disk block directly (bypass buffer cache)
static void
readdiskblock(uint dev, uint blockno, void *buf)
{
  struct buf *b = bread(dev, blockno);
  memmove(buf, b->data, BSIZE);
  brelse(b);
}

// Write a disk block directly (bypass buffer cache)
static void
writediskblock(uint dev, uint blockno, void *buf)
{
  struct buf *b = bread(dev, blockno);
  memmove(b->data, buf, BSIZE);
  bwrite(b);
  brelse(b);
}

// Write a page to swap
int swapout(pte_t *pte, char *va)
{
  int slot;
  uint blockno;
  int i;
  
  // Make sure the page is present
  if(!(*pte & PTE_P))
    return -1;
  
  // Get physical address of the page
  char *pa = P2V(PTE_ADDR(*pte));
  
  // Allocate a swap slot
  if((slot = allocswapslot()) < 0)
    return -1;
    
  // Get starting block number
  blockno = swapslotblockno(slot);
  
  // Store page permission
  swaptable.slots[slot].page_perm = PTE_FLAGS(*pte);
  
  // Write page contents to 8 consecutive disk blocks (4096 bytes / 512 bytes = 8 blocks)
  for(i = 0; i < 8; i++){
    writediskblock(1, blockno + i, pa + i*BSIZE);
  }
  
  // Update page table entry to mark page as swapped
  // Clear present bit, set swapped bit
  *pte = (*pte & ~PTE_P) | PTE_SWAPPED;
  
  // Store swap slot index in the page table entry
  // Use bits 12-31 which normally store the physical address
  *pte = (*pte & 0xFFF) | ((uint)slot << 12);
  
  // Invalidate TLB entry for this virtual address
  lcr3(V2P(myproc()->pgdir));
  
  return slot;
}

// Read a page from swap
int swapin(char *va, pte_t *pte)
{
  int slot;
  uint blockno;
  int i;
  char *mem;
  
  // Make sure the page is swapped
  if(!(*pte & PTE_SWAPPED))
    return -1;
  
  // Extract slot number from page table entry
  slot = *pte >> 12;
  
  if(slot < 0 || slot >= NSWAPSLOTS || swaptable.slots[slot].is_free)
    panic("swapin: invalid swap slot");
    
  // Allocate a new physical page
  if((mem = kalloc()) == 0)
    return -1;
    
  // Get starting block number
  blockno = swapslotblockno(slot);
  
  // Read page contents from 8 consecutive disk blocks
  for(i = 0; i < 8; i++){
    readdiskblock(1, blockno + i, mem + i*BSIZE);
  }
  
  // Retrieve original page permissions
  int perm = swaptable.slots[slot].page_perm;
  
  // Update page table entry
  *pte = V2P(mem) | perm;
  
  // Free the swap slot
  freeswapslot(slot);
  
  // Increment RSS counter
  myproc()->rss++;
  
  // Invalidate TLB entry for this virtual address
  lcr3(V2P(myproc()->pgdir));
  
  return 0;
}

// Count free memory pages
int count_free_pages(void)
{
  extern struct {
    struct spinlock lock;
    int use_lock;
    struct run *freelist;
  } kmem;
  
  struct run *r;
  int count = 0;
  
  acquire(&kmem.lock);
  r = kmem.freelist;
  while(r){
    count++;
    r = r->next;
  }
  release(&kmem.lock);
  
  return count;
}

// Find victim process with highest RSS
struct proc* find_victim_process(void)
{
  extern struct {
    struct spinlock lock;
    struct proc proc[NPROC];
  } ptable;

  struct proc *p;
  struct proc *victim = 0;
  int max_rss = 0;
  
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == SLEEPING || p->state == RUNNING || p->state == RUNNABLE){
      if(p->pid >= 1 && p->rss > max_rss){
        max_rss = p->rss;
        victim = p;
      }
      else if(p->pid >= 1 && p->rss == max_rss && victim && p->pid < victim->pid){
        victim = p;
      }
    }
  }
  release(&ptable.lock);
  
  return victim;
}

// Forward declaration of walkpgdir
pte_t *walkpgdir(pde_t *pgdir, const void *va, int alloc);

// Find victim page in a process
pte_t* find_victim_page(struct proc *p, uint *va_out)
{
  pte_t *pte;
  uint i;
  
  for(i = 0; i < p->sz; i += PGSIZE){
    pte = walkpgdir(p->pgdir, (void*)i, 0);
    
    if(pte && (*pte & PTE_P) && !(*pte & PTE_A)){
      // Found a page that is present but not accessed
      *va_out = i;
      return pte;
    }
  }
  
  // If no page with unset A flag, clear all A flags and try again
  for(i = 0; i < p->sz; i += PGSIZE){
    pte = walkpgdir(p->pgdir, (void*)i, 0);
    
    if(pte && (*pte & PTE_P)){
      *pte &= ~PTE_A;  // Clear the accessed bit
    }
  }
  
  // Try again to find a page with unset A flag
  for(i = 0; i < p->sz; i += PGSIZE){
    pte = walkpgdir(p->pgdir, (void*)i, 0);
    
    if(pte && (*pte & PTE_P) && !(*pte & PTE_A)){
      *va_out = i;
      return pte;
    }
  }
  
  return 0;  // No suitable victim page found
}

// Check free memory and perform swapping if needed
void check_memory(void)
{
  // Count free pages in memory
  int free_pages = count_free_pages();
  
  if(free_pages < threshold){
    cprintf("Current Threshold = %d, Swapping %d pages\n", threshold, npages_to_swap);
    
    // Swap out npages_to_swap pages
    for(int i = 0; i < npages_to_swap; i++){
      struct proc *victim = find_victim_process();
      if(victim){
        uint va;
        pte_t *pte = find_victim_page(victim, &va);
        if(pte){
          swapout(pte, (char*)va);
          // Decrease RSS count for the victim process
          victim->rss--;
        }
      }
    }
    
    // Update threshold and npages_to_swap
    threshold = (threshold * (100 - beta)) / 100;
    if(threshold < 1) threshold = 1;  // Ensure threshold is at least 1
    
    int new_npages = (npages_to_swap * (100 + alpha)) / 100;
    npages_to_swap = (new_npages > LIMIT) ? LIMIT : new_npages;
    if(npages_to_swap < 1) npages_to_swap = 1;  // Ensure at least 1 page is swapped
  }
}

// Free all swap slots used by a process
void freeswap(struct proc *p)
{
  pte_t *pte;
  uint i, slot;
  
  if(p->pgdir == 0)
    return;
    
  for(i = 0; i < p->sz; i += PGSIZE){
    pte = walkpgdir(p->pgdir, (void*)i, 0);
    if(pte && (*pte & PTE_SWAPPED)){
      // Free the swap slot
      slot = *pte >> 12;
      if(slot < NSWAPSLOTS)
        freeswapslot(slot);
    }
  }
}