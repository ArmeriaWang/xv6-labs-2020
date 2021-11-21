# Lab: xv6 lazy page allocation

实验指导：[Lab: xv6 lazy page allocation (mit.edu)](https://pdos.csail.mit.edu/6.828/2020/labs/lazy.html)

通过全部测试的参考代码：https://github.com/ArmeriaWang/xv6-labs-2020/tree/lazy

## 前置知识

本 Lab 中在 xv6 中完整地实现了 lazy allocation。下面是 Lab 前言的 Google translation。

>OS 可以使用页表硬件的许多巧妙技巧之一是用户空间堆内存的延迟分配。 Xv6 应用程序使用 `sbrk()` 系统调用向内核请求堆内存。在我们提供给您的内核中，`sbrk()` 分配物理内存并将其映射到进程的虚拟地址空间。内核为大型请求分配和映射内存可能需要很长时间。例如，考虑一个千兆字节由 262, 144 个 4096 字节的页面组成；即使每个分配都很便宜，这也是大量的分配。此外，一些程序分配的内存比实际使用的多（例如，为了实现稀疏数组），或者在使用之前很好地分配内存。为了让 `sbrk()` 在这些情况下更快地完成，复杂的内核会延迟分配用户内存。也就是说，`sbrk()` 不分配物理内存，而只是记住分配了哪些用户地址，并在用户页表中将这些地址标记为无效。当进程第一次尝试使用任何给定的延迟分配内存页面时，CPU 会生成一个页面错误，内核通过分配物理内存、将其归零和映射来处理该错误。在本实验中，您将向 xv6 添加此延迟分配功能。 

## 参考做法

本 Lab 和下一个 Lab，分别实现了内存管理中的两个重要优化：延迟分配（lazy allocation）和写时复制（Copy-on-write，COW）。下面通过 3 个步骤实现 lazy allocation。

### Eliminate allocation from `sbrk()`

这个任务很简单，只需把 `sbrk()` 中分配内存的代码删去，仅保留增加 `p->sz` 的部分即可。

```c
uint64
sys_sbrk(void) {
    int addr;
    int n;

    if(argint(0, &n) < 0)
        return -1;
    addr = myproc()->sz;
    myproc()->sz += n;
    return addr;
}
```

### Lazy allocation

上一步的简单修改是远远不够的，在程序引用新申请的内存时会引发 page fault。所以，我们要在 `usertrap()` 函数中截获这个异常，并进行处理。处理的方法是，`kalloc()` 一个新的物理页，并将其地址与引发 page fault 的用户虚拟地址相关联。下面的代码也包括了非法地址的检查功能。

```c
//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void) {
    ...
    else if ((which_dev = devintr()) != 0) {
        // ok
    }
    // add from here
    else if (r_scause() == 13 || r_scause() == 15) {
        // page fault
        uint64 va = r_stval();
        // if va is an illeagal virtual address, kill the process
        if (va >= myproc()->sz || va < myproc()->trapframe->sp) {
            p->killed = 1;
        }
        else {
            uint64 ka = (uint64)kalloc();
            // if fail on kalloc (ask for a new physical page), kill the process
            if (ka == 0) {
                p->killed = 1;
            }
            else {
                memset((void*)ka, 0, PGSIZE);
                va = PGROUNDDOWN(va);
                // if fail on mappages (map the physical addr to the virtual addr), kill the process
                if (mappages(p->pagetable, va, PGSIZE, ka, PTE_W | PTE_R | PTE_U | PTE_X) != 0) {
                    kfree((void*)ka);
                    p->killed = 1;
                }
            }
        }
    }
    // end
    else {
        printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
        printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
        p->killed = 1;
    }
    ...
}
```

到这里还不够，如果程序结束时存在 lazy allocation 的内存，那么在回收内存时 `uvmunmap()` 函数就会报错 `uvmunmap: not mapped`。因此我们需要把这里的 `panic` 忽略。同理，`uvmunmap: walk` 也需要忽略。

```c
// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free){
	...
    if((pte = walk(pagetable, a, 0)) == 0)
      // panic("uvmunmap: walk");
      continue;
    if((*pte & PTE_V) == 0)
      // panic("uvmunmap: not mapped");
      continue;
	...
}
```

到这里就可以成功 `echo hi` 了。

### Lazytests and Usertests

本任务就是继续完善 lazy allocation 功能。

1. 给 `sbrk()` 添加处理负数参数的功能。减小内存时，必须对该区间整个调用 `uvmdealloc`。

   ```c
   uint64
   sys_sbrk(void) {
       uint64 addr;
       int n;
       struct proc* p = myproc();
   
       if (argint(0, &n) < 0)
           return -1;
       addr = p->sz;
       // add from here
       // 检查增长或减小内存后，新内存大小是否越界或溢出
       if (addr + n < 0) {
           return -1;
       }
       if (n < 0) {
           // 减小内存，调用 dealloc
           p->sz = uvmdealloc(p->pagetable, addr, addr + n);
       }
       // end
       p->sz = addr + n;
       return addr;
   }
   ```

2. 在 `fork` 时会调用 `uvmcopy()` 函数，我们也要把这个函数 `for` 循环中的两个 `panic` 忽略。

   ```c
   int
   uvmcopy(pagetable_t old, pagetable_t new, uint64 sz) {
       ...
       if((pte = walk(old, i, 0)) == 0)
         continue;
       if((*pte & PTE_V) == 0)
         continue;
       ...
   }
   ```

3. 别忘了还有一个场景可能会引用未实际分配的 lazy 内存：系统调用。执行系统调用时是在内核模式，解引用用户虚拟地址时，是通过 `walkaddr()` 函数获得其物理地址，这个过程不涉及 page fault，更不涉及 `usertrap()`。因此，我们需要在 `walkaddr()` 中也添加类似于在 `usertrap()` 中添加的代码。

   ```c
   // Look up a virtual address, return the physical address,
   // or 0 if not mapped.
   // Can only be used to look up user pages.
   uint64
   walkaddr(pagetable_t pagetable, uint64 va) {
       pte_t* pte;
       uint64 pa;
   
       if (va >= MAXVA)
           return 0;
   
       // Here must be modified because of system calls,
       // In usertrap(), we only deal with page faults comes from user space,
       // however in system calls (such as exec, write, etc), it also comes
       // some situations that some va is applied but not mapped yet.
       // In order to solve this problem, we must modify walkaddr()
       pte = walk(pagetable, va, 0);
       struct proc* p = myproc();
       if (pte == 0 || (*pte & PTE_V) == 0) {
           if (va >= p->sz || va < p->trapframe->sp)
               return 0;
           uint64 ka = (uint64)kalloc();
           if (ka == 0) {
               return 0;
           }
           memset((void*)ka, 0, PGSIZE);
           va = PGROUNDDOWN(va);
           if (mappages(p->pagetable, va, PGSIZE, ka, PTE_W | PTE_X | PTE_R | PTE_U) != 0) {
               kfree((void*)ka);
               return 0;
           }
           return ka;
       }
       if ((*pte & PTE_U) == 0)
           return 0;
       pa = PTE2PA(*pte);
       return pa;
   }
   ```

### 测试结果

测试结果为 119/119。

![image-20210621215806534](https://i.loli.net/2021/06/21/SpeYuPhfkFs1xo7.png)

### 附加挑战

#### Make simple `copyin()` work

> Make lazy page allocation work with your simple `copyin()` from the previous lab.

要求把 Lab pgtbl 中简化过的 `copyin()` 和 lazy allocation 合起来。能感觉到这里的工作量不小，因为简化过的 `copyin()` 是把用户页表包含到内核页表之后才能工作的，这里涉及到好几处页表拷贝的操作，都需要一一做更改。有时间我把这个 challenge 落实一下。
