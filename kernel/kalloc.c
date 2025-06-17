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
void add_count(void *pa)
{

  if ((uint64)pa >= PHYSTOP || (uint64)pa < KERNBASE)
    panic("add_count: invalid pa");

  // printf("acquire: add_count\n");
  acquire(&kmem.lock);
  uint64 idx = ((uint64)pa - KERNBASE) / PGSIZE;
  // kalloc에서 이미 1로 만들었으므로, add_count는 0인 페이지에 대해 호출되면 안 됨
  if (kmem.refcount[idx] == 0)
  {
    release(&kmem.lock);
    panic("add_count: incrementing ref_count of a free page or page not yet set to 1 by kalloc");
  }
  ++kmem.refcount[idx];
  release(&kmem.lock);
}

// 해당 페이지의 refcount 1 감소
void sub_count(void *pa)
{
  if ((uint64)pa >= PHYSTOP || (uint64)pa < KERNBASE)
    panic("sub_count: invalid pa");

  acquire(&kmem.lock);
  uint64 idx = ((uint64)pa - KERNBASE) / PGSIZE;
  if (kmem.refcount[idx] <= 0)
    panic("refcount가 0인데 sub_count 시도");
  else
  {
    --kmem.refcount[idx];
    if (kmem.refcount[idx] == 0)
    {
      memset(pa, 1, PGSIZE); // Fill with junk to catch dangling refs.
      struct run *r = (struct run *)pa;
      r->next = kmem.freelist;
      kmem.freelist = r;
    }
    else if (kmem.refcount[idx] == 1)
    {
      uint64 pte = PA2PTE((uint64)pa);
      pte &= ~PTE_RSW;
      pte |= PTE_W;
      sfence_vma();
    }
  }
  release(&kmem.lock);
}

void kinit()
{
  initlock(&kmem.lock, "kmem");
  acquire(&kmem.lock);
  // 참조 카운트 배열 초기화
  for (uint i = 0; i < ((PHYSTOP - KERNBASE) / PGSIZE); i++)
  {
    kmem.refcount[i] = 0;
  }
  release(&kmem.lock);
  freerange(end, (void *)PHYSTOP);
}

void freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
  {
    // kfree(p) 대신 직접 처리
    acquire(&kmem.lock);
    // printf("acquire: freerange");
    uint idx = ((uint64)p - KERNBASE) / PGSIZE;

    // 초기화 시 이 페이지의 참조 카운트는 0이어야 함
    if (kmem.refcount[idx] != 0)
    {
      printf("freerange: warning, page %p already had ref count %d\n", p, kmem.refcount[idx]);
    }
    kmem.refcount[idx] = 0; // 참조 카운트를 0으로 명시

    memset(p, 1, PGSIZE); // 페이지를 특정 값으로 채움
    struct run *r = (struct run *)p;
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(void *pa)
{
  sub_count((void *)pa); // refcount 감소
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  // printf("kalloc: called\n");
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r)
  {
    kmem.freelist = r->next;

    uint64 idx = ((uint64)r - KERNBASE) / PGSIZE;
    if (kmem.refcount[idx] != 0)
    {
      release(&kmem.lock);
      panic("kalloc: page from freelist has non-zero ref_count");
    }
    // add_count((uint64)r);
    kmem.refcount[idx] = 1;
    // printf("kalloc: allocated page at %p, refcount set to 1\n", r);
  }
  release(&kmem.lock);

  if (r)
    memset((char *)r, 5, PGSIZE); // fill with junk

  return (void *)r;
}
