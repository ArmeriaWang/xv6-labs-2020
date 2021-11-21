# Lab: traps

实验指导：[Lab: Traps (mit.edu)](https://pdos.csail.mit.edu/6.828/2020/labs/traps.html)

通过全部测试的参考代码：https://github.com/ArmeriaWang/xv6-labs-2020/tree/traps

## 前置知识

Trap 机制是用于用户模式和内核模式之间切换的一套流程。xv6 中的步骤如下图所示。

![img](https://i.loli.net/2021/06/21/gZReK3EXNx4htmM.png)

区别于 interrupts （中断），traps 是用户程序 「主动」向内核模式的切换（如进行系统调用等）；而前者则是由硬件机制（如 CPU 每隔一段时间就会发出中断信号）要求暂停当前的用户程序，切换到内核模式。另外还有异常机制（如缺页异常 page fault），也会引发用户模式到内核模式的切换。在 xv6 中，三者最终都会落入到 `usertrap()` 函数中进行处理。

以系统调用 `write` 为例，具体来说，分为如下几步。

### `ECALL`
`write` 通过执行 `ECALL` 指令来执行系统调用。`ECALL` 指令会把控制权切换到内核中。在这个过程中，内核中执行的第一个指令是一个由汇编语言写的函数，叫做 `uservec`，它可以帮我们完成寄存器的切换等工作。这个函数是内核代码`trampoline.s` 文件的一部分。那么内核如何知道 `uservec` 在哪里呢？事实上，CPU 的 `STVEC` 寄存器事先保存了这个函数的地址（系统启动时就已设置好）。

   这里就有一个问题：`STVEC` 保存的地址是用户地址还是内核地址？注意到`ECALL` 的最开始是没有切换页表的，也就是说我们必须在用户页表下找到 `uservec`。这就要求我们必须把 `trampoline.s`  映射到每个用户进程的地址空间上，而且映射的位置还都必须相同且固定。事实上，xv6 也确实是这么做的，如下图所示（最上方的即是 trampoline 页）。trampoline 页的下方是 trapframe 页，进程的寄存器、页表指针等都保存在其 `p->trapframe` 结构体中。

   ![image-20210621015120955](https://i.loli.net/2021/06/21/dob9N1wzHfl62GO.png)

   回到 `ECALL` 本身，这个指令只改变三件事：

   - ~~确立了社会主义市场经济。~~ 将代码从 user mode 改到 supervisor mode。
   - 将程序计数器的值保存在了 `SEPC` 寄存器。
   - 跳转到 STVEC 寄存器指向的指令（即 `uservec`）。

   可以看到 `ECALL` 完成的事情很少。因为 RISC-V 秉持的观点是，`ECALL` 只完成尽量少必须要完成的工作，其他的工作都交给软件完成，从而为软件和操作系统的程序员提供最大的灵活性。

### `uservec` 函数

`uservec` 做了如下几件事。

- 保存用户进程的寄存器。
- 恢复内核线程的寄存器。
- 切换到内核页表。
- 跳转到 kernel code 中的 `usertrap()` 函数。

此外，为了合理安排寄存器，这里面用到了一系列技巧，详见[`trampoline.S` 源代码](https://github.com/mit-pdos/xv6-riscv/blob/riscv//kernel/trampoline.S)。

### `usertrap` 函数

`usertrap()` 函数是 C 语言代码，它做了下面这些事。

- 更改 `STVEC` 寄存器。这是因为内核中同样可以发生 trap，如果 trap 从内核中发起，处理流程是完全不同的，`STVEC` 的值也不例外。
- 把 `SEPC` 中的用户程序计数器到 `p->trapframe->epc`。这是因为 `usertrap()` 接下来可能要通过 `yield()` 切换到别的进程（[kernel/trap.c:80](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trap.c#L80)），那个进程也可能执行系统调用，从而覆盖掉 `SEPC` 的值。所以，需要把这个程序计数器的值保存到进程专属的 `trapframe` 中。
- 根据 `SCAUSE` 寄存器的值，判断引发 trap 的原因，并做相应的处理。
- 调用 `usertrapret()` 函数，切换回用户模式。

### `usertrapret` 函数

`usertrapret()` 同样是 C 语言代码，做了如下的工作。
- 关闭中断。这里关闭中断是因为我们将要更新 `STVEC` 寄存器来指向用户空间的 trap 处理代码，这之后如果在内核空间发生中断，就会产生错误。
- 向 `p->trapframe` 中填入 `kernel` 相关的值，以便下次从用户空间转向内核空间时使用。
- 设置 `SSTATUS` 寄存器，为 trampoline 代码的执行做准备。该寄存器的 SSP bit 位控制了 `sret` 指令的行为：该 bit 为 0 表示下次执行 `sret` 的时候，我们想要返回 user mode 而不是 supervisor mode。
- 根据用户页表地址生成相应的 `SATP` 值。
- 调用 trampoline 中的 `userret` 函数。

### `userret` 函数

`userret` 函数是汇编代码 `trampoline.S` 中的另半部分。它完成了切换回用户模式的剩余工作。
- 切换页表至用户页表。
- 恢复用户寄存器。
- 执行 `sret` 指令，这也是我们在内核中的最后一条指令。当执行完这条指令：
  - 程序会切换回 user mode。
  - `SEPC` 寄存器的数值会被拷贝到PC寄存器（程序计数器）。
  - 重新打开中断。

最后总结一下，系统调用被刻意设计的看起来像是函数调用，但是背后的 user/kernel 转换比函数调用要复杂的多。之所以这么复杂，很大一部分原因是**要保持 user/kernel 之间的隔离性，内核不能信任来自用户空间的任何内容**。

## 参考做法

### RISC-V assembly

要回答一系列小问题。解答如下。

```
(1) Registers for function arguments: a0, a1, ..., a7.
    Register a2 stores 13 for printf.

(2) Function main inlines function f (instructions 0x1c ~ 0x22), and similarly f inlines g (simply copys g, 0xe ~ 0x1a).

(3) Function printf locates at 0x630

(4) Register ra holds 0x38 (the address of next instruction) after jalr.

(5) Output: He110 World
    If RISC-V is big-endian, variable i should be set to 0x726c6400, and the value 57616 should holds.

(6) The value in register a2 (an unexpected value, maybe the third argument of the caller of printf) will be outputed.
    That's because function printf tries to read the third argument which should be stored in a2 (but the caller doesn't provide it).
```

### Backtrace

本任务要求实现调试中常用 `backtrace` 功能，即打印当前的函数调用栈。进程的栈空间结构参考下图。

![image-20210621000818738](https://i.loli.net/2021/06/21/Gatyed1TfcqBhlS.png)

做法如下。

1.  在 `printf.c` 中添加 `backtrace()` 函数。

   ```c
   void
   backtrace() {
       uint64 fp = r_fp(), pgbottom = PGROUNDUP(fp);
       while (fp >= 0 && fp < pgbottom) {
           // 不断在栈中向上跳，并输出每个函数调用的地址
           uint64 retv = *((uint64 *) fp - 1);
           printf("%p\n", retv - 4);
           fp = *((uint64 *) fp - 2);
       }
   }
   ```
   
   其中 `r_fp()` 是 `riscv.h` 中的函数，用于获取当前 `fp` 指针的值（在寄存器 `s0` 中）。
   
   ```c
   static inline uint64
   r_fp() {
       uint64 x;
       asm volatile("mv %0, s0" : "=r" (x) );
       return x;
   }
   ```
   
2. 在 `sys_sleep()` 中添加对 `backtrace()` 的调用。

   ```c
   uint64
   sys_sleep(void) {
       int n;
       uint ticks0;
   
       if (argint(0, &n) < 0)
           return -1;
       // add the following line
       backtrace();
       
       acquire(&tickslock);
       ... 
   }
   ```

### Alarm

要求为 xv6 增加一个 alarm 功能。CPU 会周期性定时引发硬件中断，每次中断就增加一个 CPU tick。用户程序通过调用 `sigalarm(interval, handler)`，就可以要求  xv6 周期性地每经过 `interval` 个该进程使用的 CPU ticks 就调用 `handler` 中断处理程序；中断处理完成后，用户程序应当接着原先的位置继续运行。如果程序调用 `sigalarm(0, 0)` ，就表明程序取消上述 alarm 要求。

这个任务最 tricky 的地方在于，我们在中断时需要调用 `handler` 这个用户程序定义的中断处理程序。毫无疑问，`handler` 需要运行在用户模式下，但用户中断却是在内核模式的 `usertrap()` 中进行处理的。如何做到这一点呢？答案是，当从 `usertrap()`  中返回时，更改 `p->trapframe->epc` 的值，将其设定为 `handler` 的地址。这样，调用 `usertrapret()` 后，系统切回了用户模式，程序 PC 也变成了 `handler`。

但做到这里还不够：中断处理程序运行完后，还要返回到触发中断前的用户程序位置继续运行。这就是说，我们要从内核模式「切换回」内核模式。没有办法，这个切换必须要借 `handler` 的一臂之力，否则无法完成：规定所有 `sigalarm()` 中注册的 `handler` 都必须在最后调用 `sigreturn()` 这个系统调用。`sigreturn()` 就是用来完成最后的切换的。如何切换呢？我们发现另一个问题：通过第二次 `usertrap()` 到达 `sigreturn()` 时，`p->trapframe` 已经被  `handler` 「污染」了，其中的 `epc` 和寄存器已经不再是引发 `handler` 之前保存的值了。因此，我们需要在内核中用一个额外的 `trapframe` 数组，用于各进程引发 `handler` 之前对 `trapframe` 进行备份。

思路到这里就基本顺下来了，下面是具体的代码实现。

1. 在 `struct proc` 中添加若干变量。

   ```c
   // Per-process state
   struct proc {
     struct spinlock lock;
     ...
     // add from here
     int ticks;                   // Alarm interval
     void (*handler)();           // Pointer to the handler function
     int tick_cnt;                // Count for ticks
   };
   ```

2. 按照常规流程，添加 `sigalarm` 和 `sigreturn` 两个系统调用。下面给出 `sys_sigalarm()` 和 `sys_sigreturn()` 的代码。

   ```c
   uint64
   sys_sigalarm(void) {
       int ticks;
       uint64 handler;
       if (argint(0, &ticks) < 0)
           return -1;
       if (argaddr(1, &handler) < 0)
           return -1;
       struct proc* p = myproc();
       p->ticks = ticks;
       // 初始化 ticks 计数
       p->tick_cnt = 0;
       p->handler = (void(*)()) handler;
       return 0;
   }
   
   uint64
   sys_sigreturn(void) {
       struct proc* p = myproc();
       p->tick_cnt = 0;
       // 恢复原先的 p->trapframe
       resume_trapframe(p);
       return 0;
   }
   ```

3. 在 `proc.c` 中添加 `trapframe` 的备份数组 `backup_tf[NPROC]`，以及用于备份和恢复的一对函数。

   ```c
   struct trapframe backup_tf[NPROC];
   void
   backup_trapframe(struct proc *p) {
   	memmove(&backup_tf[p->pid], p->trapframe, sizeof(struct trapframe));
   }
   
   void
   resume_trapframe(struct proc *p) {
   	memmove(p->trapframe, &backup_tf[p->pid], sizeof(struct trapframe));
   }
   ```

4. 在 `procinit()` 中补充初始化代码。

   ```c
   void
   procinit(void)
       memset(&p->context, 0, sizeof(p->context));
       p->context.ra = (uint64)forkret;
       p->context.sp = p->kstack + PGSIZE;
       ...
       // add from here
       p->tick_cnt = 0;
       p->ticks = 0;
       p->handler = 0;
       
       return p;
   }
   ```
   
5. 在 `trap.c` 的 `usertrap()` 中添加对 `alarm` 特性的处理。

   ```c
   void
   usertrap(void) {
       ...
       if (p->killed)
           exit(-1);
   	
       // add from here
       if (which_dev == 2 && !(p->ticks == 0 && p->handler == 0)) {
           // 处理 alarm
           if (p->tick_cnt >= 0) {
               p->tick_cnt++;
               if (p->tick_cnt == p->ticks) {
                   // tick 计数到达指定书目，调用 handler
                   backup_trapframe(p);
                   p->trapframe->epc = (uint64)p->handler;
                   p->tick_cnt = -1;
                   usertrapret();
             	}
         	}
     	}
   
     	// give up the CPU if this is a timer interrupt.
     	if(which_dev == 2)
       	yield();
   
     	usertrapret();
   }
   ```

### 附加挑战

> Print the names of the functions and line numbers in `backtrace()` instead of numerical addresses.

完全没有头绪。挖个坑在这吧。

