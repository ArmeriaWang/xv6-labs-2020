# Lab: page tables

实验指导：[Lab: page tables (mit.edu)](https://pdos.csail.mit.edu/6.828/2020/labs/pgtbl.html)

通过全部测试的参考代码：https://github.com/ArmeriaWang/xv6-labs-2020/tree/pgtbl

## 前置知识

页表用于虚拟地址到物理地址的转换。更准确地说，是虚拟页地址到物理页地址的转换（因为虚拟地址和它的物理地址的 offset 是相同的）。

<img src="https://i.loli.net/2021/06/19/zS8Tfk9coDxQO4A.png" style="zoom:67%;" />

内核自己有一张页表，也会为各个用户进程单独维护一个页表。xv6 中，一级页表头的地址存在寄存器 `satp` 中。在 xv6 中，简单起见，内核页表映射是直接映射（即虚拟地址 `x` 就映射到物理地址 `x`）。内核虚拟地址空间到物理地址空间的转换关系如下图所示。

![image-20210619203657530](https://i.loli.net/2021/06/19/XkBew1Dg2zQu9Vt.png)

自底向上地，虚拟地址空间中，

-  `0x0` ~ `0x80000000` 是 各类硬件的映射（硬件的地址是主板协议所规定的，可视作固定不变）。

- `0x80000000` 到 `0x86400000` 是对 RAM 的映射，这部分是「真正的」物理内存的所在地，其中 `0x86400000` 被称作 `PHYSTOP`，即物理内存的终点。

-  `PHYSTOP` 到 `MAXVA` 这部分比较特殊，它包含每个内核线程的内核栈 `kstack` 及其保护页 guard page，还有内核态的入口代码 `trampoline`（之前所说的 `ecall` 就是通过调用 `trampoline` 的代码，实现用户态和内核态之间的切换）。

需要注意，`kstack` 和 `trampoline` 等页都是一对多的映射：既有相等的直接映射，也有到 RAM 区域的映射：`kstack` 映射到 kernel data 对应的 RAM 中，`trampoline` 映射到 kernel text 对应的 RAM 中。当然，实际使用的时候，还是用上方的有 guard page 保护的地址，而不是 kernel data 和 kernel text 中的那些地址。

这里需要补充一点超前的知识：xv6 的内核是**单进程多线程**的，每个用户进程有一个专属的内核线程。这是因为每个用户进程需要一个内核线程来负责它的系统调用和中断处理等操作，而这些操作的数据对于用户进程来说是需要隔离的。同时，内核中的诸多数据结构又是需要共享的。因此，最适合于 xv6 内核的模型是单进程多线程。我们经常使用的「用户态」和「内核态」，前者的意思就是运行在用户进程中，后者则是运行在内核线程中。

## 参考做法

### Print a page table

本任务要求编写函数 `vmprint`，功能是打印页表。这里给出递归打印的做法。

```c
void
vmprint(pagetable_t pagetable) {
    printf("page table %p\n", pagetable);
    vmprintlev(pagetable, 0);
}

// 打印第 level 层的页表
static void
vmprintlev(pagetable_t pagetable, int level) {
    if (level > 2) return;
    for (int i = 0; i < 512; i++) {
        pte_t pte = pagetable[i];
        if ((pte & PTE_V)) {
            uint64 child = PTE2PA(pte);
            printf("..");
            for (int i = 0; i < level; i++) {
                printf(" ..");
            }
            printf("%d: pte %p pa %p\n", i, pte, child);
            // 递归打印第level + 1层
            vmprintlev((pagetable_t) child, level + 1);
        }
    }
}
```

### A kernel page table per process

前面提到，xv6 中，用户进程的页表与内核页表是分离的，二者互不包含。这意味着用户态的虚拟地址在内核中是不可直接解引用的，必须先在用户页表中用 `walk` 函数模拟一遍，找到对应的物理地址后才能对其读写。这一过程的效率是比较地下的。本任务和下一个任务的目标就是，让内核能够直接对用户的虚拟地址解引用。

对于本任务，我们需要完成的是，让每个进程在内核中运行时（即内核线程）都独立拥有一份内核页表的拷贝。做法如下。

1. 在 `struct proc` 中添加变量 `kpagetable`，表示本进程的内核页表拷贝。
    ```c
    struct proc {
        struct spinlock lock;
        ...
        pagetable_t kpagetable;      // A copy of kernel pagetable
    };
    ```

2. 添加为 `kpagetable` 初始化的函数。这部分仿照 `kvminit` 函数进行编写。

   ```c
   // 为新进程新建 kpagetable，完成各种区域的映射
   pagetable_t
   make_kpagetable4proc() {
       pagetable_t kernel_pagetable = (pagetable_t)kalloc();
       if (kernel_pagetable == 0) {
           return 0;
       }
       memset(kernel_pagetable, 0, PGSIZE);
   
       kvmmap4proc(kernel_pagetable, UART0, UART0, PGSIZE, PTE_R | PTE_W);
       kvmmap4proc(kernel_pagetable, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);
       kvmmap4proc(kernel_pagetable, CLINT, CLINT, 0x10000, PTE_R | PTE_W);
       kvmmap4proc(kernel_pagetable, PLIC, PLIC, 0x400000, PTE_R | PTE_W);
       kvmmap4proc(kernel_pagetable, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);
       kvmmap4proc(kernel_pagetable, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);
       kvmmap4proc(kernel_pagetable, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
   
       return kernel_pagetable;
   }
   
   void
   kvmmap4proc(pagetable_t kernel_pagetable, uint64 va, uint64 pa, uint64 sz, int perm) {
       if (mappages(kernel_pagetable, va, sz, pa, perm) != 0)
           panic("kvmmap");
   }
   ```

3. 修改 `allocproc` 函数，使其具备初始化 `kpagetable`、映射自己的 `kstack` 的功能。同时，把 `procinit` 函数中的相关功能删除。

    ```c
    static struct proc*
    allocproc(void) {
        struct proc* p;
    
        ...
    	if((p->trapframe = (struct trapframe *)kalloc()) == 0){
            release(&p->lock);
            return 0;
      	}
        
        // add from here
        // 创建并分配 kpagetable
        p->kpagetable = make_kpagetable4proc();
        if (p->kpagetable == 0) {
            freeproc(p);
            release(&p->lock);
            return 0;
        }
    
        // 向内存申请 kstack 页，获取物理地址
        char* pa = kalloc();
        if (pa == 0) {
            freeproc(p);
            release(&p->lock);
            return 0;
        }
        // 将 kstack 页的虚拟地址映射到刚申请到的物理地址
        kvmmap4proc(p->kpagetable, p->kstack, (uint64)pa, PGSIZE, PTE_R | PTE_W);
    
        ...
        return p;
    }
    
    // 删除 procinit 的相关功能后，代码如下
    void
    procinit(void) {
        struct proc* p;
        initlock(&pid_lock, "nextpid");
        for (p = proc; p < &proc[NPROC]; p++) {
            initlock(&p->lock, "proc");
            // 计算 kstack 的虚拟地址
            p->kstack = KSTACK((int)(p - proc));
        }
        kvminithart();
    }
    ```

4. 修改 `scheduler` 函数，使其在切换进程时要切换内核页表。

    ```c
    void
    scheduler(void) {
        struct proc* p;
        struct cpu* c = mycpu();
    
        c->proc = 0;
        for (;;) {
            // Avoid deadlock by ensuring that devices can interrupt.
            intr_on();
    
            int found = 0;
            for (p = proc; p < &proc[NPROC]; p++) {
                acquire(&p->lock);
                if (p->state == RUNNABLE) {
                    // Switch to chosen process.  It is the process's job
                    // to release its lock and then reacquire it
                    // before jumping back to us.
                    p->state = RUNNING;
                    c->proc = p;
                    
                    // 页表切换到即将运行的进程的 kpagetable
                    w_satp(MAKE_SATP(p->kpagetable));
                    // 刷新 TLB
                    sfence_vma();
                    // 切换上下文（寄存器等），这是一段汇编代码，自这一行后就开始执行进程 p 了
                    swtch(&c->context, &p->context);
                    // 现在 p 执行中断，切换回调度器了，因此要把页表再切回来
                    kvminithart();
    
                    // Process is done running for now.
                    // It should have changed its p->state before coming back.
                    c->proc = 0;
    
                    found = 1;
                }
                release(&p->lock);
            }
    		...
        }
    }
    ```
    
5. 在 `freeproc` 函数中释放进程的内核页表。注意，释放内核页表不同于释放用户页表，前者在释放时不应同时释放其对应的物理页。以下代码中，`proc_freekpagetable` 函数就是做这个工作的。
   
   ```c
   static void
   freeproc(struct proc* p) {
       if (p->trapframe)
           kfree((void*)p->trapframe);
       p->trapframe = 0;
   
       // 释放 kstack 页
       pte_t* pte = walk(p->kpagetable, p->kstack, 0);
       if (pte == 0)
           panic("freeproc :: kstack");
       kfree((void*)PTE2PA(*pte));
       // 注意不要重置 p->kstack，因为这是 kstack 的虚拟地址，
       // 回忆上面的那张映射图，这个地址是个常量（在 procinit
       // 中预处理的），不需要变化
   
       if (p->pagetable)
           proc_freepagetable(p->pagetable, p->sz);
       p->pagetable = 0;
   
       // 释放 kpagetable
       if (p->kpagetable)
           proc_freekpagetable(p->kpagetable, p);
       p->kpagetable = 0;
       // p->kpagetable 需要重置，因为每次 allocproc 时
       // 都会重新创建 kpagetable。
   
       p->sz = 0;
       p->pid = 0;
       p->parent = 0;
       p->name[0] = 0;
       p->chan = 0;
       p->killed = 0;
       p->xstate = 0;
       p->state = UNUSED;
   }
   
   // 释放内核页表，不释放其对应的物理页
   void
   proc_freekpagetable(pagetable_t kpagetable, struct proc* p) {
       freewalk4proc_kpagetable(kpagetable, 0, p);
   }
   
   // 递归遍历进程 p 的内核页表的第 level 层，只释放其非叶子结点
   static void
   freewalk4proc_kpagetable(pagetable_t kpagetable, int level, struct proc* p) {
       // 如果是叶子结点（即物理页），直接返回
       if (level > 2) return;
       for (int i = 0; i < 512; i++) {
           pte_t pte = kpagetable[i];
           kpagetable[i] = 0;
           if (pte & PTE_V) {
               uint64 child = PTE2PA(pte);
               // 递归遍历下一层页表
               freewalk4proc_kpagetable((pagetable_t)child, level + 1, p);
           }
       }
       // 释放本结点
       kfree((void*)kpagetable);
   }
   ```

### Simplify `copyin/copyinstr`

为了使内核能够直接对用户虚拟地址解引用，下面我们进行第二步：把上一步中拷贝的内核页表映射关系做一些改动。这个改动需要满足以下的条件：

- 用户虚拟地址空间被包含在内核虚拟地址空间中。
- 用户虚拟地址空间与内核需要使用的数据段和代码段没有重叠。

注意到系统启动后，内核虚拟地址空间中，最底端的 `0 ~ 0x0C000000` 这一段是空闲的。因此，一种简单可行的方案就是，把用户虚拟地址空间 `0 ~ MAXVA` 直接映射到内核空间的`0 ~ 0x0C000000` 上（这里需要在系统中限制用户虚拟内存地址最大不能超过 `0x0C000000`。

于是，我们需要在有创建或修改用户页表映射的地方，同步地修改该进程内核页表的对应位置。

本任务做法如下。

1. 用直接解引用用户地址的 `copyin_new` 和 `copyinstr_new` 替换需要调用 `walkaddr` 的  `copyin` 和 `copyinstr`。

   ```c
   // Copy from user to kernel.
   // Copy len bytes to dst from virtual address srcva in a given page table.
   // Return 0 on success, -1 on error.
   int
   copyin_new(pagetable_t pagetable, char* dst, uint64 srcva, uint64 len) {
       struct proc* p = myproc();
   
       if (srcva >= p->sz || srcva + len >= p->sz || srcva + len < srcva)
           return -1;
       memmove((void*)dst, (void*)srcva, len);
       stats.ncopyin++;
       return 0;
   }
   
   // Copy a null-terminated string from user to kernel.
   // Copy bytes to dst from virtual address srcva in a given page table,
   // until a '\0', or max.
   // Return 0 on success, -1 on error.
   int
   copyinstr_new(pagetable_t pagetable, char* dst, uint64 srcva, uint64 max) {
       struct proc* p = myproc();
       char* s = (char*)srcva;
   
       stats.ncopyinstr++;   // XXX lock
       for (int i = 0; i < max && srcva + i < p->sz; i++) {
           dst[i] = s[i];
           if (s[i] == '\0')
               return 0;
       }
       return -1;
   }
   ```

2. 在 `vm.c` 中添加用于把用户页表包含到内核页表中的 `uvmmap2kvm` 函数。注意，这里不能简单地将 PTE 中的内容拷贝，而要将 `PTE_U` 这个 flag 给移除掉，否则内核将无法访问。

    ```c
    // 把用户页表 upagetable 的 begin 到 end 这一段地址的映射包含到内核页表中
    void
    uvmmap2kvm(pagetable_t kpagetable, pagetable_t upagetable, uint64 begin, uint64 end) {
        if (end < begin) {
            return;
        }
        if (end >= PLIC) {
            panic("uvmmap2kvm :: new sz is too large");
        }
        begin = PGROUNDUP(begin);
        for (uint64 i = begin; i < end && i < PLIC; i += PGSIZE) {
            pte_t* upte = walk(upagetable, i, 0);
            if (upte == 0) {
                panic("uvmmap2kvm :: upte not exist");
            }
            if (((*upte) & PTE_V) == 0) {
                printf("pa = %p\n", PTE2PA(*upte));
                panic("uvmmap2kvm :: PTE_V of upte is unexpected not set");
            }
            pte_t* kpte = walk(kpagetable, i, 1);
            if (kpte == 0) {
                panic("uvmmap2kvm :: kpte not exist");
            }
            uint uflags = PTE_FLAGS(*upte);
            // 注意要把 PTE_U 这个 flag 取消掉
            *kpte = PA2PTE(PTE2PA(*upte)) | (uflags & (~PTE_U));
        }
    }
    ```

3. 修改 `fork` 函数，在创建用户页表后，在内核页表中建立对应的用户地址空间映射。

   ```c
   // Create a new process, copying the parent.
   // Sets up child kernel stack to return as if from fork() system call.
   int
   fork(void) {
       ...
       // Copy user memory from parent to child.
       if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0) {
           freeproc(np);
           release(&np->lock);
           return -1;
       }
       np->sz = p->sz;
       // add the following line
       uvmmap2kvm(np->kpagetable, np->pagetable, 0, np->sz);
       ...
   }
   ```

4. 修改 `exec` 函数，在重新设置进程的用户页表后，也同步地修改其内核页表。

   ```c
   int
   exec(char* path, char** argv) {
     	...
       safestrcpy(p->name, last, sizeof(p->name));
       
       // 在重新映射前需要先解除映射
       // uvmunmap 也同样适用于 kpagetable 的用户内存部分
     	uvmunmap(p->kpagetable, 0, oldsz / PGSIZE, 0);
     	...
     	p->trapframe->sp = sp; // initial stack pointer
       
     	// 重新映射
       uvmmap2kvm(p->kpagetable, p->pagetable, 0, sz);
       ...
   }
   ```
   
5. 修改 `growproc` 函数，在用户地址空间增长（或减小）时，不仅要修改用户页表，也要修改内核页表。

   ```c
   // Grow or shrink user memory by n bytes.
   // Return 0 on success, -1 on failure.
   int
   growproc(int n) {
       uint sz;
       struct proc* p = myproc();
   
       sz = p->sz;
       if (n > 0) {
           // 地址空间增长，因此申请空间并修改用户页表
           if ((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
               return -1;
           }
           // 修改内核页表
           uvmmap2kvm(p->kpagetable, p->pagetable, sz - n, sz);
       }
       else if (n < 0) {
           // 地址空间减小，因此返还空间并在两个页表中解除映射
           sz = uvmdealloc(p->pagetable, sz, sz + n);
   		uvmunmap(p->kpagetable, PGROUNDUP(sz), (-n)/PGSIZE, 0);
       }
       p->sz = sz;
       return 0;
   }
   ```
   
6. 思考题及其解答。

    > **Question** - Explain why the third test `srcva + len < srcva` is necessary in `copyin_new()`: give values for srcva and len for which the first two test fail (i.e., they will not cause to return `-1`) but for which the third one is true (resulting in returning `-1`).

   ```
   Solution
   
   The third statement is to prevent overflow.
   
   For example, srcva = 1, len = 18446744073709551615 (2^64-1), p->sz = 5,
   these values will case:
       srcva < p->sz, 1st statement NOT satisfied;
       srcva+len = 0 < p->sz, 2st statement NOT satisfied;
       srcva+len = 0 < srcva, 3st statement satisfied,
   the third statement leads to the function returning -1.

### 测试结果

测试结果为 66/66 分。

![image-20210620223430426](https://i.loli.net/2021/06/20/X6vaFyeV1Z95fIg.png)

###  附加挑战

#### Super-pages

> Use super-pages to reduce the number of PTEs in page tables. 

意思是让我们用更大的 `PGSIZE` 代替当前的 4096 KB。实现它本身并不难，只需要修改一个常数即可，重点在于这么修改的动机。这里给出一篇非常不错的参考资料：[为什么 HugePages 可以提升数据库性能 - 面向信仰编程 (draveness.me)](https://draveness.me/whys-the-design-linux-hugepages/)。原文说的是数据库，但底层机制然仍是基于 Linux（Linux 中被称作 Huge page，大页）。这里引用一下原文的总结部分。

>随着单机内存越来越大、服务消耗的内存越来越多，Linux 和其他操作系统都引入了类似 HugePages 的功能，该功能可以从以下两个方面提升数据库等占用大量内存的服务的性能：
>
>- HugePages 可以降低内存页面的管理开销，它可以减少进程中的页表项、提高 TLB 缓存的命中率和内存的访问效率；
>- HugePages 可以锁定内存，禁止操作系统的内存交换和释放，不会被交换到磁盘上为其它请求让出内存。

对于用户来说，如果要运行耗费内存巨大、访存随机而且访存是瓶颈的程序，大页内存会带来明显的性能提升。

#### Support larger user programs

这个挑战是要我们支持尽可能大的用户内存空间，不要局限在 PLIC 的 `0x0C000000` 之下。一个可行的方案是，把用户地址空间整个挪到 `PHYSTOP` 之上。这样，除开最上方的内核栈等，限制用户地址空间大小的就只要 `MAXVA` 了。在内核对用户空间地址解引用时，就不能用原本数值，而需要加上 `PHYSTOP` 这个基址。

#### Unmap the first page

对于虚拟地址空间来说，位于`0x0` 的起始页最好保持干净，因为很多场合都把 `null` 定义为 `0`，如果 `0x0` 处有数据，就很可能引发未知错误。因此，这个任务需要我们把起始页的映射解除，让用户空间从地址 4096 开始。可以看到，如果我们完成了上一个挑战任务，这个任务其实就已经完成了。