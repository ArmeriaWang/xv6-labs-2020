# Lab: system calls

实验指导：[Lab: System calls (mit.edu)](https://pdos.csail.mit.edu/6.828/2020/labs/syscall.html)

通过全部测试的参考代码：https://github.com/ArmeriaWang/xv6-labs-2020/tree/syscall

## 前置知识

### 权限模式

操作系统的进程管理需要满足以下三个特性：多路复用、隔离和交互。

RISC-V 有三种指令模式：机器模式（全部权限）、监督模式（特别权限）和用户模式。内核 kernel 在监督模式下运行，或说运行在内核空间 kernel space 中。如果一个普通的用户模式应用程序想要调用内核函数（如系统调用 `read` 等），就必须先转移到内核中。

![img](https://i.loli.net/2021/06/19/amAFk9xtDhWS3Vl.png)

CPU 提供了一种用于从用户模式转移到监督模式的特别指令（如 RISC-V 中的 `ecall` ），它可以从内核指定的一个入口点处进入内核。一旦 CPU 转移进监督模式，内核就会使系统调用的参数生效，然后决定执行该系统调用与否。这里的重要之处在于，控制转移到监督模式的入口点的是内核，而非应用程序。如果应用程序能决定内核的入口点，那么恶意程序就能在一个能跳过参数生效这一步骤的地方进入内核。

关于两个模式之间究竟是如何切换的，在 Lab traps 中会详细介绍。

### 宏内核 v.s. 微内核

对于操作系统来说，内核空间的代码和数据必须是安全的、可信任的（常被称作*可被信任的计算空间 Trusted Computing Base*，TCB）。而随着软件规模的增大，bug 数量会越来越不可控。这个角度上，内核空间负责的工作应该越精简、越基础越好，操作系统的相对高层的功能就放到用户模式下运行，有必要时再切换到内核模式。（微内核）

但另一方面，如果内核实现的功能过于基础，就要在进程通信、模式切换等事务上消耗大量资源，从而降低运行效率。在这个角度上，把更丰富的功能集成到内核空间中，可以更容易地实现信息共享，且避免反复的模式切换，从而提供更好的性能（宏内核）。

这两种不同的思路分别对应于微内核和宏内核的操作系统。

> 在实际中，两种内核设计都会出现，出于历史原因大部分的桌面操作系统是宏内核，如果你运行需要大量内核计算的应用程序，例如在数据中心服务器上的操作系统，通常也是使用的宏内核，主要的原因是Linux提供了很好的性能。但是很多嵌入式系统，例如Minix，Cell，这些都是微内核设计。这两种设计都很流行，如果你从头开始写一个操作系统，你可能会从一个微内核设计开始。但是一旦你有了类似于Linux这样的宏内核设计，将它重写到一个微内核设计将会是巨大的工作。并且这样重构的动机也不足，因为人们总是想把时间花在实现新功能上，而不是重构他们的内核。

## 参考做法

### System call tracing

任务是添加一个用于跟踪系统调用的系统调用 `trace`，程序需要输出每次系统调用的名称以及调用它的进程 `pid`。该系统调用的接口如下。

```c
void trace(uint tmask);
```

其中，`tmask` 的第 `i` 个二进制位表示本进程是否跟踪第 `i` 个系统调用。

实现方法如下。

1. 在进程结构体 `struct proc` 中添加一个 `tmask` 变量。

   ```c
   struct proc {
       struct spinlock lock;
       ... 
       uint64 tmask;                // trace mask
   };=
   ```

2. 在新添加的系统调用 `sys_trace` 中记录本进程的 `tmask`。

   ```c
   uint64
   sys_trace(void) {
       int tmask;
   
       if (argint(0, &tmask) < 0) {
           return -1;
       }
       myproc()->tmask = tmask;
       return 0;
   }
   ```
   
3. 在 `syscall` 系统调用返回时，如果当前进程跟踪了这个系统调用，就将其信息输出。

   ```c
   void
   syscall(void) {
       int num;
       struct proc* p = myproc();
       num = p->trapframe->a7;
       if (num > 0 && num < NELEM(syscalls) && syscalls[num]) {
           p->trapframe->a0 = syscalls[num]();
           // start here
           if ((p->tmask) & (1 << num))
               printf("%d: syscall %s -> %d\n", p->pid, num2syscall[num - 1], p->trapframe->a0);
           // end here
       }
       else {
           ...
       }
   }
   ```
### Sysinfo 

任务是实现系统调用 `sysinfo`，功能是让用户获取剩余内存和剩余可用进程数这两项系统信息。它接收一个 `struct sysinfo` 结构体的指针，在里面填充好信息以后返回。

需要注意的是，用户传入的 `sysinfo` 指针是用户态的地址。由于用户态和内核态的页表不同，所以内核态不能直接在这个地址写入，而是需要用 `copyout` 函数将数据拷贝到用户态内存中。

实现方法如下。

1. 在 `kalloc.c` 中添加函数 `collectfree`，用于统计剩余空闲内存的大小。遍历 `freelist` 即可完成统计。

   ```c
   uint64
   collectfree(void) {
       struct run* r;
       r = kmem.freelist;
       uint64 amt = 0;
       while (r) {
           amt += PGSIZE;
           r = r->next;
       }
       return amt;
   }
   ```
   
2. 在 `proc.c` 中添加函数 `collectproc`，用于统计剩余可用进程数。线性扫描一遍 `proc` 数组即可完成统计。

   ```c
   uint64
   collectproc() {
       struct proc* p;
       uint64 pcnt = 0;
       for (p = proc; p < &proc[NPROC]; p++) {
           if (p->state != UNUSED)
               pcnt++;
       }
       return pcnt;
   }
   ```

3. 添加系统调用 `sys_sysinfo`。调用以上两个函数，并将数据 `copyout` 到用户内存中。

   ```c
   uint64
   sys_sysinfo(void) {
       uint64 sinfo_addr;
       if (argaddr(0, &sinfo_addr) < 0) {
           return -1;
       }
       if (sinfo_addr > 0x3fffffffff) {
           return -1;
       }
       struct sysinfo sinfo;
       struct proc* p = myproc();
       sinfo.freemem = collectfree();
       sinfo.nproc = collectproc();
       copyout(p->pagetable, sinfo_addr, (char*)&sinfo, sizeof(sinfo));
       return 0;
   }
   ```

### 测试结果

测试结果为 35/35 分。

![image-20210620223713137](https://i.loli.net/2021/06/20/lLAIeVzcNGX6OWP.png)

### 附加挑战

难度也都不高，略去。
