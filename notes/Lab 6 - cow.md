# Lab: Copy-on-Write Fork for xv6

实验指导：[Lab: Copy-on-Write Fork for xv6 (mit.edu)](https://pdos.csail.mit.edu/6.828/2020/labs/cow.html)

通过全部测试的参考代码：https://github.com/ArmeriaWang/xv6-labs-2020/tree/cow

## 前置知识

本 Lab 在 xv6 中完整地实现了 copy-on-write。下面是 Lab 前言的 Google translation。

>Xv6 中的 `fork()` 系统调用将父进程的所有用户空间内存复制到子进程中。 如果父级很大，复制可能需要很长时间。 更糟糕的是，复制来的映射经常被浪费掉。例如，子进程中的 `fork()` 后跟 `exec()` 将导致子进程丢弃复制的内存，可能从未使用过大部分内存。 另一方面，如果父子进程都使用一个页面，并且一个或两个进程都写了它，则确实需要一个副本。
>
>写时复制 COW `fork()` 的目标是推迟为子进程分配和复制物理内存页面，直到真正需要副本（如果有的话）。COW `fork()` 只为子进程创建一个分页表，用户内存的 PTE 指向父进程的物理页面。COW `fork()` 将父和子中的所有用户 PTE 标记为不可写。当任一进程尝试写入这些 COW 页之一时，CPU 将强制发生页错误。内核页面错误处理程序检测到这种情况，为故障进程分配一个物理内存页面，将原始页面复制到新页面中，并修改故障进程中的相关 PTE 以引用新页面，这次使用 PTE 标记为可写。当页面错误处理程序返回时，用户进程将能够写入它的页面副本。 
>
>COW `fork()` 使得释放实现用户内存的物理页面变得有点棘手：一个给定的物理页可能被多个进程的页表引用，并且只有当最后一个引用消失时才应该释放它。  

## 参考做法

### Implement copy-on write

1. 修改 `uvmcopy()`，在复制内存的时候不申请物理内存，而是直接把拷贝父页表的 PTE，同时将两个页表中的相应 PTE 的可写标记 `PTE_W` 取消，并加上写时复制标记 `PTE_COW`（在 `riscv.h` 中定义为 `1L << 8`）。

   ```c
   // Given a parent process's page table, copy
   // its memory into a child's page table.
   // Copies both the page table and the
   // physical memory.
   // returns 0 on success, -1 on failure.
   // frees any allocated pages on failure.
   int
   uvmcopy(pagetable_t old, pagetable_t new, uint64 sz) {
       pte_t* pte;
       uint64 pa, i;
       uint flags;
       // char *mem;
   
       for (i = 0; i < sz; i += PGSIZE) {
           if ((pte = walk(old, i, 0)) == 0)
               panic("uvmcopy: pte should exist");
           if ((*pte & PTE_V) == 0)
               panic("uvmcopy: page not present");
           pa = PTE2PA(*pte);
           // 处理 PTE 的标志位
           flags = PTE_FLAGS(*pte);
           if (*pte & PTE_W) {
               // 取消 PTE_W，加上 PTE_COW
               *pte = (*pte & (~PTE_W)) | PTE_COW;
               flags = PTE_FLAGS(*pte);
           }
           // 在新表中进行映射
           if (mappages(new, i, PGSIZE, (uint64)pa, flags) != 0) {
               goto err;
           }
           // 增加 PTE 引用计数
           pte_ref_cnt[pa / PGSIZE]++;
       }
       return 0;
   
   err:
       uvmunmap(new, 0, i / PGSIZE, 1);
       return -1;
   }
   ```
   
2. 修改 `usertrap()`，如果合法的虚拟地址引发了 page fault ，就调用 `cow_alloc()` 函数，申请一个写时复制的页。

   ```c
   //
   // handle an interrupt, exception, or system call from user space.
   // called from trampoline.S
   //
   void
   usertrap(void) {
       ...
       if (r_scause() == 8) {
           ...
       }
       // add from here
       else if (r_scause() == 13 || r_scause() == 15) {
           uint64 va = r_stval();
           
           if (va >= MAXVA || (va <= PGROUNDDOWN(p->trapframe->sp) && va >= PGROUNDDOWN(p->trapframe->sp) - PGSIZE)) {
               // 非法地址：高于 MAXVA，低于 sp，或在 guard page 中
               p->killed = 1;
           }
           else if (cow_alloc(p->pagetable, va) != 0) {
               // 申请 cow 页失败
               p->killed = 1;
           }
       }
       // end
       else if ((which_dev = devintr()) != 0) {
           // ok
       }
       ...
   }
   ```
   
3. 在 `vm.c` 中添加  `cow_alloc()` 函数。

   ```c
   // 申请一页物理内存，并关联到指定 pagetable 的 va 地址处
   int
   cow_alloc(pagetable_t pagetable, uint64 va) {
       uint64 pa;
       pte_t* pte;
       uint flags;
   
       if (va >= MAXVA) {
           return -1;
       }
       va = PGROUNDDOWN(va);
       pte = walk(pagetable, va, 0);
       if (pte == 0) {
           return -1;
       }
       pa = PTE2PA(*pte);
       if (pa == 0) {
           return -1;
       }
   
       flags = PTE_FLAGS(*pte);
   
       if (flags & PTE_COW)   {
           // cow 页
           char* ka = kalloc();
           if (ka == 0) return -1;
           // 复制父页的内容
           memmove(ka, (char*)pa, PGSIZE);
           // 释放父页
           kfree((void*)pa);
           // 处理标志位
           flags = (flags & ~PTE_COW) | PTE_W;
           *pte = PA2PTE((uint64)ka) | flags;
       }
   
       return 0;
   }
   ```

4. 在 `kalloc.c` 中修改 `kinit()`、`kfree()` 和 `kalloc()` 函数，添加页引用计数的相关操作。

      ```c
      uint pte_ref_cnt[PGROUNDUP(PHYSTOP) / PGSIZE]; // 页引用计数数组

      void
      kinit() {
          // add the following line
          memset(pte_ref_cnt, sizeof(pte_ref_cnt), 0);
          initlock(&kmem.lock, "kmem");
          freerange(end, (void*)PHYSTOP);
      }

      void
      kfree(void *pa)
      {
          struct run *r;

          if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
              panic("kfree");

          // add from here
          if (pte_ref_cnt[(uint64)pa / PGSIZE] > 0) {
              pte_ref_cnt[(uint64)pa / PGSIZE]--;
          }
          if (pte_ref_cnt[(uint64)pa / PGSIZE]) {
              return;
          }
          // end
          ...
      }

      void *
      kalloc(void) {
          ...
          if(r) {
              memset((char*)r, 5, PGSIZE); // fill with junk
              // add the following line
              pte_ref_cnt[(uint64) r / PGSIZE] = 1;
          }
          return (void*)r;
      }
      ```

5. 最后修改 `copyout()`。这里的修改理由与 lazy allocation 中修改 `walkaddr()` 的理由一样。
    ```c
    // Copy from kernel to user.
    // Copy len bytes from src to virtual address dstva in a given page table.
    // Return 0 on success, -1 on error.
    int
    copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
        uint64 n, va0, pa0;
    
        while (len > 0) {
            va0 = PGROUNDDOWN(dstva);
            // add from here
            if (cow_alloc(pagetable, va0) != 0) {
                return -1;
            }
            // end
            pa0 = walkaddr(pagetable, va0);
            ...
        }
        return 0;
    }
    ```

### 测试结果

测试结果为 110/110 分。
![image-20210621215504815](https://i.loli.net/2021/06/21/uyBdAjFpkKLURqb.png)

### 附加挑战

#### Support both lazy allocation & COW

要求同时支持 lazy allocation 和 COW。事实上刚才实现的 `cow_alloc()` 函数已经可以同时解决 lazy page 和 cow page 的申请问题了，因此两个功能的合并是容易做的。

#### Further reduce copying bytes & allocating physical pages

这里要求进一步优化拷贝的字节数以及申请的物理页数。

1. 首先，可以做课堂中说过的 [Zero Fill On Demand](https://mit-public-courses-cn-translatio.gitbook.io/mit6-s081/lec08-page-faults-frans/8.3-zero-fill-on-demand)。

2. 复制 page table 似乎也有优化的余地。例如，`fork()` 进程后，不是立即拷贝页表，而是让子进程与父进程共用页表，直到子进程发生页表大小变化或页表映射更改时，再进行拷贝。 
3. 以后想到更多再来补充。

