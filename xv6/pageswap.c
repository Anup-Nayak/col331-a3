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
#include "pageswap.h"

struct swapslot swapslots[NSWAPSLOTS];

// Initialize swap slots
void swap_init(void)
{
  int i;
  for (i = 0; i < NSWAPSLOTS; i++) {
    swapslots[i].page_perm = 0;
    swapslots[i].is_free = 1;
  }
  cprintf("Swap initialization: %d slots created\n", NSWAPSLOTS);
}

// Find a free swap slot
int find_free_slot(void)
{
  int i;
  for (i = 0; i < NSWAPSLOTS; i++) {
    if (swapslots[i].is_free) {
      return i;
    }
  }
  return -1;  // No free slots available
}

// Write page to disk (bypassing log)
int write_page_to_disk(uint pa, int slot_num)
{
  int i;
  int blockno = SWAP_START + slot_num * 8;  // Each slot is 8 blocks
  struct buf *b;
  
  for (i = 0; i < 8; i++) {  // 8 blocks per page (4096/512 = 8)
    b = bget(ROOTDEV, blockno + i);
    memmove(b->data, (char*)(pa + i * BSIZE), BSIZE);
    bwrite(b);
    brelse(b);
  }
  
  return 0;
}

// Read page from disk
int read_page_from_disk(uint pa, int slot_num)
{
  int i;
  int blockno = SWAP_START + slot_num * 8;
  struct buf *b;
  
  for (i = 0; i < 8; i++) {
    b = bget(ROOTDEV, blockno + i);
    memmove((char*)(pa + i * BSIZE), b->data, BSIZE);
    brelse(b);
  }
  
  return 0;
}

// Adaptive page replacement parameters
int threshold = 100;  // Initial threshold
int npages_to_swap = 2;  // Initial number of pages to swap
int alpha = 25;  // Alpha parameter
int beta = 10;   // Beta parameter
#define LIMIT 100  // Maximum number of pages to swap at once

// Update threshold and number of pages to swap
void update_swap_threshold(void)
{
  threshold = threshold * (100 - beta) / 100;  // T_h = T_h(1 - β/100)
  npages_to_swap = npages_to_swap * (100 + alpha) / 100;  // N_pg = N_pg(1 + α/100)
  if(npages_to_swap > LIMIT)
    npages_to_swap = LIMIT;
  
  cprintf("Current Threshold = %d, Swapping %d pages\n", threshold, npages_to_swap);
}

// Count free pages in memory
int count_free_pages(void)
{
  // This is a placeholder. You'll need to implement the function 
  // that counts free pages in memory based on how kalloc/kfree work
  extern struct run *kmem;
  struct run *r;
  int count = 0;
  
  acquire(&kmem.lock);
  r = kmem.freelist;
  while(r) {
    count++;
    r = r->next;
  }
  release(&kmem.lock);
  
  return count;
}

// Find a victim process - the one with highest RSS
struct proc* find_victim_proc(void)
{
  struct proc *p;
  struct proc *victim = 0;
  int max_rss = 0;
  
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if((p->state == RUNNING || p->state == RUNNABLE || p->state == SLEEPING) && 
       p->pid >= 1 && p->rss > max_rss) {
      max_rss = p->rss;
      victim = p;
    } else if((p->state == RUNNING || p->state == RUNNABLE || p->state == SLEEPING) && 
              p->pid >= 1 && p->rss == max_rss && victim && p->pid < victim->pid) {
      // If same RSS, choose the process with lower PID
      victim = p;
    }
  }
  release(&ptable.lock);
  
  return victim;
}

// Find a victim page from a process - with P flag set and A flag unset
uint find_victim_page(struct proc *p)
{
  pde_t *pgdir = p->pgdir;
  uint i;
  
  // Iterate through all possible virtual addresses
  for(i = 0; i < KERNBASE; i += PGSIZE) {
    pte_t *pte = walkpgdir(pgdir, (char*)i, 0);
    if(pte && (*pte & PTE_P) && !(*pte & PTE_A)) {
      // Found a page that's present but not accessed
      return i;
    }
  }
  
  // If we couldn't find an ideal victim, look for any present page
  for(i = 0; i < KERNBASE; i += PGSIZE) {
    pte_t *pte = walkpgdir(pgdir, (char*)i, 0);
    if(pte && (*pte & PTE_P)) {
      // Reset the accessed flag for next time
      *pte &= ~PTE_A;
      return i;
    }
  }
  
  return 0;  // Couldn't find a victim page
}

// Swap out a page
int swap_out_page(struct proc *p, pde_t *pgdir, uint va)
{
  pte_t *pte;
  uint pa;
  int slot_num;
  
  pte = walkpgdir(pgdir, (char*)va, 0);
  if(!pte || !(*pte & PTE_P))
    return -1;  // Page not present
  
  pa = PTE_ADDR(*pte);
  
  // Find a free swap slot
  slot_num = find_free_slot();
  if(slot_num < 0)
    return -1;  // No free slots
  
  // Save page permissions
  swapslots[slot_num].page_perm = *pte & 0xFFF;  // Lower 12 bits contain flags
  swapslots[slot_num].is_free = 0;
  
  // Write page to disk
  if(write_page_to_disk(pa, slot_num) < 0)
    return -1;
  
  // Update page table entry to mark as swapped
  // Store slot number in PTE in place of physical address
  *pte = (*pte & 0xFFF) | ((slot_num) << 12);
  
  // Mark page as not present but as swapped
  *pte &= ~PTE_P;  // Clear present bit
  *pte |= PTE_S;   // Set swapped bit (we need to define this in mmu.h)
  
  // Free the physical page
  kfree((char*)pa);
  
  // Update RSS count
  p->rss--;
  
  return 0;
}

// Swap in a page
int swap_in_page(struct proc *p, pde_t *pgdir, uint va)
{
  pte_t *pte;
  uint pa;
  int slot_num;
  
  pte = walkpgdir(pgdir, (char*)va, 0);
  if(!pte || (*pte & PTE_P))
    return -1;  // Page already present or invalid
  
  if(!(*pte & PTE_S))
    return -1;  // Not a swapped page
  
  // Extract slot number from PTE
  slot_num = (*pte >> 12) & 0xFFFFF;  // Get slot number
  
  if(slot_num >= NSWAPSLOTS || swapslots[slot_num].is_free)
    return -1;  // Invalid slot or slot is free
  
  // Allocate a new page in memory
  char *mem = kalloc();
  if(mem == 0)
    return -1;  // Out of memory
  
  pa = (uint)mem;
  
  // Read page from disk
  if(read_page_from_disk(pa, slot_num) < 0) {
    kfree(mem);
    return -1;
  }
  
  // Restore page permissions and mark as present
  *pte = PA2PTE(pa) | swapslots[slot_num].page_perm | PTE_P;
  
  // Mark slot as free
  swapslots[slot_num].is_free = 1;
  swapslots[slot_num].page_perm = 0;
  
  // Update RSS count
  p->rss++;
  
  // Flush TLB
  lcr3(V2P(pgdir));
  
  return 0;
}

// Check if we need to swap out pages and do so if needed
void check_swap(void)
{
  int free_pages = count_free_pages();
  if(free_pages < threshold) {
    update_swap_threshold();
    
    int i;
    for(i = 0; i < npages_to_swap; i++) {
      struct proc *victim_proc = find_victim_proc();
      if(!victim_proc) break;  // No victim process found
      
      uint victim_va = find_victim_page(victim_proc);
      if(!victim_va) break;  // No victim page found
      
      swap_out_page(victim_proc, victim_proc->pgdir, victim_va);
    }
  }
}