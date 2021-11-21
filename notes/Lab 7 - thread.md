# Lab: Multithreading

实验指导：[Lab: Multithreading (mit.edu)](https://pdos.csail.mit.edu/6.828/2020/labs/thread.html)

通过全部测试的参考代码：https://github.com/ArmeriaWang/xv6-labs-2020/tree/thread

## 前置知识

### 线程 Threads

线程被称作 CPU 调度的最小单元。它也可以认为是一种在有多个任务时简化编程的抽象：在这里，我们认为线程就是单个串行执行代码的单元，它只占用一个 CPU 并且以普通的方式一个接一个的执行指令。

线程还具有状态，我们可以随时保存线程的状态并暂停它的运行。这里所说的状态包括：

- 程序计数器 PC。
- 保存变量的寄存器。
- 线程栈。

操作系统中线程系统的工作就是管理多个线程的运行。一般来说，线程的数量是远多于 CPU 核心数的。因此，如何让众多线程轮流、有序地运行在有限的 CPU 核心上，就成为 OS 的重要课题。6.S081 中对线程调度的讲解主要集中在「线程是如何切换的」这一问题上，而对线程的调度策略和算法则少有涉及。下面，先对课程中的知识点进行简要总结，然后简单介绍 Linux 中 PCB 和 TCB 的概念。至于任务调度策略，则是非常庞大的话题，一篇 Lab 博客是完全无法面面俱到的。在更系统地学习相关知识之后，我会再单独为这个话题写一些分享。

### Xv6 线程切换机制

#### 解除用户线程的 CPU 控制权

线程切换是本课程着重讲解的问题。内核中的线程系统主要面临以下 3 个挑战。

1. 如何实现线程间的切换。这里，停止一个线程的运行并启动另一个线程的过程，通常被称为线程调度（Scheduling）。我们将会看到 xv6 为每个 CPU 核都创建了一个线程调度器（Scheduler）。
2. 当从一个线程切换到另一个线程时，需要保存并恢复线程的状态，所以需要决定线程的哪些信息是必须保存的，以及在哪保存它们。
3. 如何处理运算密集型线程（Compute bound threads）。对于线程切换，很多直观的实现是由线程自己自愿的保存自己的状态，然后让其他的线程运行。但是如果我们有一些程序正在执行一些可能要花费数小时的长时间计算任务，这样的线程并不能自愿的出让 CPU 给其他的线程运行。所以这里需要能从长时间运行的运算密集型线程撤回对于CPU的控制，将其放置于一边，稍后再运行它。

对于第 3 个挑战，解决方案通常是硬件层面的：用 CPU 核上的定时器中断。定时器中断能将控制权从用户空间代码切换到内核中的中断处理程序。哪怕这些用户空间进程一直占用 CPU，内核也可以强行解除用户空间线程的 CPU 控制权。

别忘了，xv6 的内核是单进程多线程的，且不支持用户多线程。每个用户进程（线程）有一个专属的内核线程。我们说内核强行解除 CPU 的控制权，在 xv6 中其实是大致走了这样的流程：

1. 定时器发送中断，调用 `STVEC` 中的代码，从用户模式切换到内核模式。
2. 进入 `usertrap()` 函数。
3. `usertrap()` 发现进入本函数的原因是定时器中断。
4. 通过 `yield()` 函数（后文会讲到）放弃本线程（这里既指用户线程，也指它对应的内核线程，二者是一体的）的控制权。
5. 线程调度器 Scheduler 获得控制权，进行线程状态的保存和恢复，实现线程切换。

#### 线程分类

对于线程调度器来说，往往需要区分以下几类线程（粗略的分类）：

1. 当前在 CPU 上运行的线程。
2. 一旦 CPU 有空闲时间就想要运行在 CPU 上的线程。
3. 暂时不想运行的线程（可能在等待 I/O 等事件）。

为此，我们需要定义 3 种「状态分类」：`RUNNING`、`RUNNABLE` 和 `SLEEPING`。这里我们主要关注前两种，第 3 种会在 Lab lock 中详细介绍。

#### 调度器

现在，假设 xv6 要让 CPU 核心 0 需要从 CC 程序的内核线程切换到 LS 程序的内核线程，那么这一过程主要经过了如下步骤：

1. 定时器中断强迫 CPU-0 从用户空间进程 CC 切换到其内核线程，trampoline 代码将用户寄存器保存于用户进程对应的 trapframe 中。
2. 在内核中运行 `usertrap()`，来实际执行相应的中断处理程序。
3. 中断处理程序决定出让 CPU-0，调用 `yield()` 函数，并最终调用 `swtch` 函数（这是汇编代码编写的函数）。
4. `swtch` 函数保存用户进程 CC 对应内核线程的寄存器至对应的 context 结构体。

到这里暂停一下：`swtch` 函数并不会直接切换到另一个内核线程。xv6中，**一个 CPU 核心上运行的内核线程可以直接切换到的是这个 CPU 核心对应的调度器线程。**（每个 CPU 核心各有一个调度器线程——别忘了，我们调度线程的最终载体是 CPU 核心。）例如，如果 CC 当前运行在 CPU-0，那么`swtch` 函数会恢复之前 CPU-0 调度器线程的 context，之后就在调度器线程的 context 下执行 `scheduler()` 函数。所以接着上面的来。

5. `swtch` 函数恢复 CPU 核心调度器线程的 context。
6. 执行 `scheduler()` 函数。该函数会选择一个可运行的（`RUNNABLE`）线程，假设选择了 LS。
7. 保存 CPU-0 调度器线程的 context。
8. 找到进程 LS 的内核线程的 context，并恢复之。
9. PC 会返回到 LS 对 `swtch` 函数的调用里，然后返回到 LS 内核线程的 `usertrap()` 函数。
10. 最后从 `usertrap()` 函数返回，恢复用户进程的 trapframe，用户进程从中断的位置继续运行。

### Linux 的 PCB 和 TCB

Linux 中全面地支持了线程。不同于 xv6，Linux 支持多个用户线程从属于一个用户进程。因此，我们需要对 xv6 中的   `struct trapframe` 和 `struct context` 重新设计。而与 xv6 相同的是，Linux 也是「一对一模型」，即用户线程与内核线程是一一对应的，一个内核负责一个用户线程。

在概念上，Linux 对进程与线程用不同的数据结构保存状态。进程是用进程控制块（Process Control Block，PCB）保存的；线程用线程控制块（Thread Control Block，TCB）保存。而代码实现中，PCB 和 内核线程 TCB 实际上用的是同一个东西：`struct task_struct`，源代码[见此](https://elixir.bootlin.com/linux/v4.0/source/include/linux/sched.h#L1278)。这是一个相当丰富的结构体，里面可储存进程或线程状态的所有必要信息。至于用户线程 TCB，则取决于具体的库实现，如 POSIX 线程库的 `pthread` 结构体。可以认为，用户 TCB 是内核 TCB 的拓展。

由于都是一对一模型，Linux 中线程切换的步骤与 xv6 是非常相似的，自底向上也依次是调度器、内核线程和用户线程。当然，因为支持了单个用户进程中包含多个线程，因此在具体的实现上是有很多区别的，需要对 Linux 有更多研究后再来讨论这个话题。

## 参考做法

### Uthread: switching between threads

本任务是给 xv6 提供用户多线程切换支持。按照内核线程切换的方法写就可以了。

1. 在 `uthead.c` 中添加 `struct ut_context` 结构体，用于保存用户线程的上下文。同内核线程的 context 一样，除了 `ra` 和 `sp`，我们只需记下被调用者保存寄存器（callee saved registers）。

   ```c
   // user thread context
   struct ut_context {
     uint64 ra;
     uint64 sp;
   
     // callee-saved
     uint64 s0;
     uint64 s1;
     uint64 s2;
     uint64 s3;
     uint64 s4;
     uint64 s5;
     uint64 s6;
     uint64 s7;
     uint64 s8;
     uint64 s9;
     uint64 s10;
     uint64 s11;
   };
   
   struct thread {
     char   stack[STACK_SIZE];  /* the thread's stack */
     int    state;              /* FREE, RUNNING, RUNNABLE */
     struct ut_context context;
   };
   ```

2. 用类似于 `kernel/switch.S` 中的方法，向 `uthread_switch.S` 中添加 `thread_switch` 函数，并在 `thread_schedule()` 函数中添加对 `thread_switch()` 函数的调用。

   以下是 `thread_switch.S` 的内容。

   ```assembly
   .text
   
   	/*
       * save the old thread's registers,
       * restore the new thread's registers.
       */
   
   .globl thread_switch
   thread_switch:
   	/* YOUR CODE HERE */
   	sd ra, 0(a0)
   	sd sp, 8(a0)
   	sd s0, 16(a0)
   	sd s1, 24(a0)
   	sd s2, 32(a0)
   	sd s3, 40(a0)
   	sd s4, 48(a0)
   	sd s5, 56(a0)
   	sd s6, 64(a0)
   	sd s7, 72(a0)
   	sd s8, 80(a0)
   	sd s9, 88(a0)
   	sd s10, 96(a0)
   	sd s11, 104(a0)
   
   	ld ra, 0(a1)
   	ld sp, 8(a1)
   	ld s0, 16(a1)
   	ld s1, 24(a1)
   	ld s2, 32(a1)
   	ld s3, 40(a1)
   	ld s4, 48(a1)
   	ld s5, 56(a1)
   	ld s6, 64(a1)
   	ld s7, 72(a1)
   	ld s8, 80(a1)
   	ld s9, 88(a1)
   	ld s10, 96(a1)
   	ld s11, 104(a1)
   	/* END OF MY CODE */
   	ret    /* return to ra */
   
   ```

   以下是对 `thread_schedule()` 函数的修改。

   ```c
   void
   thread_schedule(void) {
       struct thread* t, * next_thread;
       ...
       if (current_thread != next_thread) {         /* switch threads?  */
           next_thread->state = RUNNING;
           t = current_thread;
           current_thread = next_thread;
           /* YOUR CODE HERE
            * Invoke thread_switch to switch from t to next_thread:
            * thread_switch(??, ??);
            */
           thread_switch((uint64)&t->context, (uint64)&next_thread->context);
           // END OF MY CODE
       }
       else
           next_thread = 0;
   }
   ```
   
3. 修改 `thread_create()` 函数，在创建线程时初始化它的线程栈和上下文为全 0，也要初始化 `context` 中的 `ra` 和 `sp` 寄存器。其中，`ra` 寄存器就赋成传入的函数指针；而 `sp` 则要指向 `stack` 的栈底。

   ```c
   void
   thread_create(void (*func)()) {
       struct thread* t;
   
       for (t = all_thread; t < all_thread + MAX_THREAD; t++) {
           if (t->state == FREE) break;
       }
       t->state = RUNNABLE;
       // YOUR CODE HERE
       memset(t->stack, 0, sizeof(t->stack));
       memset(&t->context, 0, sizeof(t->context));
       t->context.ra = (uint64)func;
       t->context.sp = (uint64)t->stack + STACK_SIZE;
       // END OF MY CODE
   }
   ```

### Using threads

本任务要求在自己的机器上实现支持多线程的 hash table。

首先需要回答一个小问题：为什么 2 个线程时会出现 missing keys，而 1 个线程时不会呢？举例说明。

> Thread 1:
> put(1, 1);
> put(2, 2);
>
> Thread 2:
> put(3, 3);
>
> Thread 1 first inserted (1, 1) to the map, and then inserts (2, 2). 
> Simultaneously, thread 2 inserts (3, 3). 
> If thread 1 executes line 36 `*p = e` earlier than thread 2, pair (2, 2) will be override and lost. 
> When someone wants to get the value of key 2, the map will return 0.

然后修改一下 `put()` 函数，加个锁即可。

```c
// 添加一个全局大锁
pthread_mutex_t lock;

static
void put(int key, int value) {
  int i = key % NBUCKET;

  // is the key already present?
  struct entry* e = 0;
  for (e = table[i]; e != 0; e = e->next) {
    if (e->key == key)
      break;
  }
  if (e) {
    // update the existing key.
    e->value = value;
  }
  else {
    // the new is new.
    pthread_mutex_lock(&lock);
    insert(key, value, &table[i], table[i]);
    pthread_mutex_unlock(&lock);
  }
}
```

### Barrier

本任务的场景是，有若干个线程，每个线程都是跑一个 20000 轮的循环。现在要求写一个线程同步屏障 `barrier`，使得没有任何线程在所有线程都进入第 `i` 轮之前进入第 `i + 1` 轮。

搞明白 [pthread_cond_wait](https://pubs.opengroup.org/onlinepubs/007908799/xsh/pthread_cond_wait.html) 和 [pthread_cond_broadcast](https://pubs.opengroup.org/onlinepubs/007908799/xsh/pthread_cond_broadcast.html) 的用法，结合线程锁 `pthread_mutex` 即可轻松解决。

```c
static void
barrier() {
    // YOUR CODE HERE
    //
    // Block until all threads have called barrier() and
    // then increment bstate.round.
    pthread_mutex_lock(&bstate.barrier_mutex);
    bstate.nthread++;
    if (bstate.nthread == nthread) {
        bstate.nthread = 0;
        bstate.round++;
        pthread_mutex_unlock(&bstate.barrier_mutex);
        pthread_cond_broadcast(&bstate.barrier_cond);
    }
    else {
        pthread_cond_wait(&bstate.barrier_cond, &bstate.barrier_mutex);
        pthread_mutex_unlock(&bstate.barrier_mutex);
    }
    // END OF MY CODE
}

static void*
thread(void* xa) {
    long n = (long)xa;
    long delay;
    int i;

    for (i = 0; i < 20000; i++) {
        int t = bstate.round;
        assert(i == t);
        barrier();
        usleep(random() % 100);
    }

    return 0;
}
```

### 测试结果

测试结果为 60/60 分。

![image-20210621215146800](https://i.loli.net/2021/06/21/zhNXFpeOLoaIA5K.png)

### 附加挑战

本 Lab 的附加挑战很有意思。第一个任务中，对用户线程的支持是很有限的，例如，我们甚至无法完成同一进程的两个线程在不同的 CPU 核上并行运行。解决方案中，就有一条是我们在前置知识中提到的，Linux 所用的「一对一模型」。

附加挑战就是要求我们在 `uthread` 的基础上，进一步扩展用户线程包的功能。这里抄个题，以后有时间填上。

>The user-level thread package interacts badly with the operating system in several ways. For example, if one user-level thread blocks in a system call, another user-level thread won't run, because the user-level threads scheduler doesn't know that one of its threads has been descheduled by the xv6 scheduler. As another example, two user-level threads will not run concurrently on different cores, because the xv6 scheduler isn't aware that there are multiple threads that could run in parallel. Note that if two user-level threads were to run truly in parallel, this implementation won't work because of several races (e.g., two threads on different processors could call `thread_schedule` concurrently, select the same runnable thread, and both run it on different processors.)
>
>There are several ways of addressing these problems. One is using [scheduler activations](http://en.wikipedia.org/wiki/Scheduler_activations) and another is to use one kernel thread per user-level thread (as Linux kernels do). Implement one of these ways in xv6. This is not easy to get right; for example, you will need to implement TLB shootdown when updating a page table for a multithreaded user process.
>
>Add locks, condition variables, barriers, etc. to your thread package.
