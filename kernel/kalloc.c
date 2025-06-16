// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run
{
  struct run *next;
};

struct
{
  struct spinlock lock;
  struct run *freelist;
  int refcount[(PHYSTOP - KERNBASE) / PGSIZE]; // 각 물리 페이지마다 count
} kmem;

// 해당 페이지의 refcount 1 증가
void add_count(uint64 pa)
{
  acquire(&kmem.lock);

  if ((uint64)pa >= PHYSTOP || (uint64)pa < KERNBASE)
    panic("add_count: invalid pa");

  uint64 idx = ((uint64)pa - KERNBASE) / PGSIZE;
  ++kmem.refcount[idx];
  release(&kmem.lock);
}

// 해당 페이지의 refcount 1 감소
void sub_count(uint64 pa)
{
  acquire(&kmem.lock);
  if ((uint64)pa >= PHYSTOP || (uint64)pa < KERNBASE)
    panic("sub_count: invalid pa");

  uint64 idx = ((uint64)pa - KERNBASE) / PGSIZE;
  if (kmem.refcount[idx] <= 0)
    panic("refcount가 0인데 sub_count 시도");
  else
  {
    --kmem.refcount[idx];
    if (kmem.refcount[idx] == 0)
    {
      // refcount가 0이 되면 기존 kfree로직
      memset((void *)pa, 1, PGSIZE); // Fill with junk to catch dangling refs.
      struct run *r = (struct run *)pa;
      r->next = kmem.freelist;
      kmem.freelist = r;
    }

    // refcount가 1이 되면 cow bit 해제, write 가능
    else if (kmem.refcount[idx] == 1)
    {
      uint64 pte = PA2PTE(pa);
      // cow bit 해제
      pte &= ~PTE_RSW;
      // write 권한 설정
      pte |= PTE_W;
    }
  }
  release(&kmem.lock);
}

void kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void *)PHYSTOP);
}

void freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(void *pa)
{
  sub_count((uint64)pa); // refcount 감소
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r)
  {
    kmem.freelist = r->next;
    uint64 idx = ((uint64)r - KERNBASE) / PGSIZE;
    if (kmem.refcount[idx] != 0)
      panic("kalloc: page from freelist has non-zero ref_count");
    kmem.refcount[idx] = 1; // 처음 할당되면 count는 1로 초기화
  }
  release(&kmem.lock);

  if (r)
    memset((char *)r, 5, PGSIZE); // fill with junk
  return (void *)r;
}
