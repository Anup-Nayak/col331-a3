#ifndef PAGESWAP_H
#define PAGESWAP_H

#define NSWAPSLOTS 800  // Number of swap slots
#define SWAP_START 2    // Swap blocks start after boot block and superblock
#define ROOTDEV 1
struct swapslot {
  int page_perm;  // Permission of the swapped memory page
  int is_free;    // Availability of this swap slot
};

extern struct swapslot swapslots[NSWAPSLOTS];

int swap_out_page(struct proc *p, pde_t *pgdir, uint va);
int swap_in_page(struct proc *p, pde_t *pgdir, uint va);
void swap_init(void);
int find_free_slot(void);
struct proc* find_victim_proc(void);
uint find_victim_page(struct proc *p);
void update_swap_threshold(void);
int count_free_pages(void);

#endif