# Lab: locks

实验指导：[Lab: locks (mit.edu)](https://pdos.csail.mit.edu/6.828/2020/labs/lock.html)

通过全部测试的参考代码：https://github.com/ArmeriaWang/xv6-labs-2020/tree/lock

## 前置知识

### 使用锁的动机及其带来的挑战

锁是为了解决多线程所带来的并行性问题。当并行的访问数据结构时，例如一个核在读取数据，另一个核在写入数据，我们需要使用锁来协调对于共享数据的更新，以确保数据的一致性。所以，我们需要锁来控制并确保共享的数据是正确的。锁的特性就是只有一个进程可以获取锁，在任何时间点都不能有超过一个锁的持有者。

所带来的挑战主要有 3 类。

- **死锁（Dead Locks）**。老生常谈的问题，也非常复杂，这里不多说。

- **破坏程序的模块化**。这个问题某种程度上是由死锁带来的。为了避免死锁，我们必须对各个模块或函数获取和释放锁的流程进行规划，这很难做到黑盒。

- **锁与性能之间的权衡**。锁实际上就是把一部分本来是并行的代码串行化，从而降低性能。如果你想要性能随着CPU的数量增加而增加，你需要将数据结构和锁进行拆分（这个 Lab 的两个任务就都是做这个事的）。这个拆分往往意味着大量的重构工作，既费脑又费体力。

  通常来说，开发的流程是：

  - 先以 coarse-grained lock（注，也就是大锁）开始。
  - 再对程序进行测试，来看一下程序是否能使用多核（即测试是否具有良好的并行性）。
  - 如果可以的话，那么工作就结束了，你对于锁的设计足够好了；如果不可以的话，那意味着锁存在竞争，多个进程会尝试获取同一个锁，因此它们将会序列化的执行，性能也上不去，之后你就需要重构程序。

### 自旋锁 Spinlock

所谓「自旋」，就是用一个 `while` 循环，不断判断锁是否被获取。如果没有被获取，就获取之；否则继续循环判断等待，代码如下所示。

```c
struct spinlock {
	int locked;
};

// 当 acquire 返回时，就意味着获取到了锁
void acquire(struct spinlock *lk) {
	while (1) {
		if (!lk->locked) {
			lk->locked = 1;
			return;
		}
	}
}
```

这个设计思路是正确的，但仍然存在竞争：两个进程可能同时读到 `lock->locked` 为 0，同时把锁拿到。这违背了「只有一个进程可以获取锁」的约束。这个问题的解决一般是硬件层面的：通过一个特殊的 CPU 指令（例如 RISC-V 中的 `amoswap`），保证一次 test-and-set 操作的原子性。xv6 中的自旋锁 `acquire()` 函数如下所示。

```c
// Acquire the lock.
// Loops (spins) until the lock is acquired.
void
acquire(struct spinlock* lk) {
    push_off(); // disable interrupts to avoid deadlock.
    if (holding(lk))
        panic("acquire");

    // On RISC-V, sync_lock_test_and_set turns into an atomic swap:
    //   a5 = 1
    //   s1 = &lk->locked
    //   amoswap.w.aq a5, a5, (s1)
    while (__sync_lock_test_and_set(&lk->locked, 1) != 0) {
        ;
    }

    // Tell the C compiler and the processor to not move loads or stores
    // past this point, to ensure that the critical section's memory
    // references happen strictly after the lock is acquired.
    // On RISC-V, this emits a fence instruction.
    __sync_synchronize();

    // Record info about lock acquisition for holding() and debugging.
    lk->cpu = mycpu();
}
```

同理，为了保证原子性，释放锁的 `release()` 函数也是借助特殊 CPU 指令完成的。

```c
// Release the lock.
void
release(struct spinlock* lk) {
    if (!holding(lk))
        panic("release");

    lk->cpu = 0;

    // Tell the C compiler and the CPU to not move loads or stores
    // past this point, to ensure that all the stores in the critical
    // section are visible to other CPUs before the lock is released,
    // and that loads in the critical section occur strictly before
    // the lock is released.
    // On RISC-V, this emits a fence instruction.
    __sync_synchronize();

    // Release the lock, equivalent to lk->locked = 0.
    // This code doesn't use a C assignment, since the C standard
    // implies that an assignment might be implemented with
    // multiple store instructions.
    // On RISC-V, sync_lock_release turns into an atomic swap:
    //   s1 = &lk->locked
    //   amoswap.w zero, zero, (s1)
    __sync_lock_release(&lk->locked);

    pop_off();
}
```

### 睡眠与唤醒 Sleep & Wakeup

自旋锁的思路和实现都简单，但效率比较低下：它浪费了大量的 CPU 资源用于检测锁是否被获取，这导致它的应用场景十分有限。例如，如果进程 A 和 B 需要向屏幕输出字符，为了避免竞争，它们会在调用「屏幕输出」的系统接口之前，就对这个 block 上锁。而屏幕输出这个操作是非常耗时间的（长达数 ms，够现代处理器执行数百万条指令了）。假设进程 A 先抢到了这个锁，那么如果采用自旋锁，就意味着 CPU 把大量的时间花在进程 B 的「检测锁」循环上，这是非常浪费的。

为此，我们希望又有一种机制，让进程能够**等待特定的事件**，而且在这个事件发生之前，调度器会忽略这个进程，不让它执行。继续上面的例子，我们希望在进程 A 开始输出后，让进程 B 等待「屏幕输出完毕」这个事件，而在这个事件发生之前，就通过调用 `sched()` 函数，让 B 放弃 CPU 占用，并「沉沉睡去 Sleep」（也就是，除了 `RUNNING` 和 `RUNNABLE` 的第 3 种进程状态 `SLEEP`），调度器不去执行它。在「屏幕输出完毕」这个事件发生后，进程 A 通过「唤醒 Wakeup」接口，让所有等待该事件的进程苏醒过来（即把 `SLEEP` 变成 `RUNNABLE`）。这些进程中会有一个（例如进程 B）获取到屏幕输出的权限，并进行屏幕输出。

在 xv6 中，`sleep()` 和 `wakeup()` 的实现是非常巧妙的。我们注意到，`sleep` 和 `wakeup` 事实上并不关心事件本身到底是什么。因此，我们可通过一个整数（叫 sleep channel）表明我们等待的特定事件，当调用 `wakeup` 时传入相同的数值来表明想唤醒哪些线程。

以下是 `sleep()` 和 `wakeup()` 的源代码。

```c
// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void* chan, struct spinlock* lk) {
    struct proc* p = myproc();

    // Must acquire p->lock in order to
    // change p->state and then call sched.
    // Once we hold p->lock, we can be
    // guaranteed that we won't miss any wakeup
    // (wakeup locks p->lock),
    // so it's okay to release lk.
    if (lk != &p->lock) {  //DOC: sleeplock0
        acquire(&p->lock);  //DOC: sleeplock1
        release(lk);
    }

    // Go to sleep.
    p->chan = chan;
    p->state = SLEEPING;

    sched();

    // Tidy up.
    p->chan = 0;

    // Reacquire original lock.
    if (lk != &p->lock) {
        release(&p->lock);
        acquire(lk);
    }
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void* chan) {
    struct proc* p;

    for (p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if (p->state == SLEEPING && p->chan == chan) {
            p->state = RUNNABLE;
        }
        release(&p->lock);
    }
}
```

这里之所以用两个自旋锁 `lk` 和 `p->lock`，是为了解决 [lost wakeup](https://mit-public-courses-cn-translatio.gitbook.io/mit6-s081/lec13-sleep-and-wakeup-robert/13.3-lost-wakeup) 的问题。在调用 `sleep()` 之前，必须先获取传入的 `lk`。这里的 `lk` 用于保护进程间交流事件状态的「信号」，被称作条件锁（Condition Lock）。

例如，我们可以用一个整数 `sig` 表示「当前是否有进程在进行屏幕输出」。显然， `sig` 是需要条件锁来保护的，而且不能在睡眠期间持有（否则别的进程无法修改 `sig` ，进入死锁）。那么，我们能否在调用 `sleep()` 之前释放条件锁，从 `sleep()` 返回时取回它呢？答案是否定的。假设有一个发现「别的进程在进行屏幕输出」的进程 A，和正在进行屏幕输出的进程 B。在进程 A 释放条件锁 `lk` 之后，调用 `sleep()` 之前的这个时间点，进程 B 恰好完成了屏幕输出，并取得条件锁 `lk` ，修改 `sig` 为「已完成屏幕输出」，然后调用 `wakeup()` 唤醒所有等待屏幕输出的进程。这时，由于进程 A 的状态还不是 `SLEEPING`，所以并不受这个 `wakeup()` 的影响；但在进入了 `sleep()` 之后，如果没有别的进程再进行屏幕输出，我们会发现它永远也醒不过来了：它把本该用于唤醒自己 `wakeup()` 弄丢了。这就是 lost wakeup。归根结底，这是因为「释放条件锁」和「设置进程为 `SLEEPING` 状态」这两个操作是非原子的。

想要优雅地解决 lost wakeup 是很困难的，xv6 中选择了一种不那么优雅的非黑盒的办法：用条件锁 `lk` 和进程锁 `p->lock` 这两个锁的组合。

- 对于 `sleep()` 的调用者，它需要在调用 `sleep()` 之前获取条件锁 `lk` ，读取「信号」的值，并将 条件锁 `lk` 作为参数传给 `sleep()`。而 `sleep()` 会在释放条件锁 `lk` 之前先获取进程锁 `p->lock`。
- 对于 `wakeup()` 的调用者，它需要在调用 `wakeup()` 之前先获取条件锁 `lk`，修改「信号」的值。而 `wakeup()` 函数在改变进程状态之前需要获取进程锁 `p->lock`。也就是说，`wakeup()` 中，只有同时持有两把锁才能查看和修改进程状态。

这样一来，通过两把锁的组合，就保证了 `sleep()` 中「释放条件锁」和「设置进程为 `SLEEPING` 状态」这两个操作的原子性：因为 `sleep()` 在整个过程的任何时刻都至少持有一把锁，防止了 `wakeup()` 对进程状态的修改。此外，这个设计中，`sleep()` 获取锁的顺序和 `wakeup()` 获取锁的顺序是一致的（都是先条件锁、后进程锁），因此不会出现死锁。

此外，xv6 中还有睡眠锁 Sleep Lock（见 [sleeplock.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sleeplock.c)）的设计，常用于等待磁盘读写等场景。

## 参考做法

### Memory allocator

xv6 中，空闲内存列表 `kmem` 的定义如下。

```c
struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;
```

其中，链表 `freelist` 用于保存空闲内存页，自旋锁 `lock` 用于防止访问冲突。多进程调用 `kalloc()` 、 `kfree()` 等函数访问 `kmem` 时，所有相关操作被 `lock` 串行化了，因此效率是很低的。本任务就是要对上述数据结构进行拆分，提高并行化程度。具体方法是，每个 CPU 核单独维护一个 `kmem` 结构体，各 CPU 核的 `kmem` 的 `freelist` 的并集就是当前空闲物理页的集合。当 CPU `$i$` 上的进程申请一页物理页时，`kalloc` 会优先在 `kmem[i]` 的 `freelist` 中寻找可用页；如果没有，再去别的 CPU 「偷页」。访问哪个 `kmem`，就只对那个 `kmem` 上锁，而不对其它的 `kmem` 上锁。这样，就提高了并行化程度。代码实现如下。

1. 更改 `kmem` 定义，令其为数组。

   ```c
   struct {
       struct spinlock lock;
       struct run* freelist;
   } kmem[NCPU];
   ```

2. 更改 `kinit()` 函数，对每个 `kmem` 都进行初始化。

   ```c
   void
   kinit() {
       const int max_namelen = 10;
       for (int i = 0; i < NCPU; i++) {
           char kmem_name[max_namelen];
           snprintf(kmem_name, max_namelen, "kmem-%d", i);
           initlock(&kmem[i].lock, kmem_name);
       }
       freerange(end, (void*)PHYSTOP);
   }
   ```

3. 更改 `kfree()` 函数。CPU `$i$` 上的进程调用 `kfree()` 释放某一页时，就把它放入 `kmem[i]` 的 `freelist` 中。
    ```c
    void
    kfree(void* pa) {
        struct run* r;
    
        if (((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
            panic("kfree");
    
        // Fill with junk to catch dangling refs.
        memset(pa, 1, PGSIZE);
    
        r = (struct run*)pa;
    
        push_off();
        int cpu_id = cpuid();
        acquire(&kmem[cpu_id].lock);
        r->next = kmem[cpu_id].freelist;
        kmem[cpu_id].freelist = r;
        release(&kmem[cpu_id].lock);
        pop_off();
    }
    ```

4. 更改 `kalloc()` 函数，思路已经在上面叙述过。

   ```c
   void*
   kalloc(void) {
       struct run* r;
   
       push_off();
       int cpu_id = cpuid();
       acquire(&kmem[cpu_id].lock);
       r = kmem[cpu_id].freelist;
       if (r)
           kmem[cpu_id].freelist = r->next;
       release(&kmem[cpu_id].lock);
       pop_off();
   
       if (!r) {
           // steal a free page from another cpu's freelist
           for (int i = 0; i < NCPU; i++) {
               acquire(&kmem[i].lock);
               r = kmem[i].freelist;
               if (r) {
                   kmem[i].freelist = r->next;
                   release(&kmem[i].lock);
                   break;
               }
               release(&kmem[i].lock);
           }
       }
   
       if (r)
           memset((char*)r, 5, PGSIZE);  // fill with junk
       return (void*)r;
   }
   ```


### Buffer cache

如果多个进程集中使用文件系统，它们可能会争用 `bcache.lock`（这个锁保护 `kernel/bio.c` 中的磁盘块缓存池）。 同前面一样，这会串行化对缓存的访问，效率很低。这里，我们同样采用拆分数据结构的方法提高并行化程度。但与 `kmem` 不同的是，`kmem` 中存储的是本质相同的空闲物理页，且只涉及到插入和删除两种操作；而 `bcache` 中存储的则是可区分的键值对（键是 `blockno`，值就是块中的数据），而且涉及到插入、删除、替换和查找等操作，情况更加复杂。

实验指导中给出的解决方案是，把缓存块按 `blockno` 做哈希，缓存池中改用数组（`bcache.buf`）存储缓存块，并把其访问权限拆分到若干个哈希桶中，桶中仅存储指向 `bcache.buf` 数组中相应块的指针。我选用 `blockno % NBUCKET` 作为哈希函数，其中 `NBUCKET` 为质数 13。修改后的数据结构如下（简单起见，哈希桶 `struct bbucket` 中使用数组而非链表进行存储）。

```c
struct bbucket {
    struct spinlock lock;      // 保护本 hash bucket 的小锁
    struct buf *bentry[NBUF];  // 用数组存储指向 bcache.buf 数组中相应对象的指针
    uint entry_num;            // 桶中的缓存块数量
};

struct {
    struct spinlock lock;              // 保护 bcache 的大锁
    struct buf buf[NBUF];              // 用数组存储所有缓存块
    struct bbucket hashbkt[NBUCKET];   // 哈希桶
    uint glob_tstamp;                  // 时间戳，LRU 算法选择 eviction block 时使用
} bcache;

struct buf {
    ...
    // add the following 2 lines
    uint tstamp;  // 本缓存块的最近访问时间戳
    uint bktord;  // 本缓存块在其哈希桶数组中的顺序
    // delete the following 2 lines
    // struct buf *prev;
    // struct buf *next;
};

// 哈希函数，传入 blockno，返回对应的哈希桶 bbucket 指针
struct bbucket*
bhash(uint blockno) {
    return &bcache.hashbkt[blockno % NBUCKET];
}
```

对应于数据结构的修改，我们先修改 `binit()` 函数，初始化 `bcache`、所有 `bbucket` 和数组 `bcache.buf`（即缓存池）中的所有 `struct buf`。注意，简单起见，我们把 `bcache.buf` 数组都填上了缓存块，（`bcache.buf[i]` 就是 `blockno` 为 `i` 的块），并按照设定好的哈希函数分发到了各个哈希桶中了。这样，我们就省略了「把缓冲池填满」这一阶段：需要向缓冲池中加入新块时，一律需要做 eviction。

```c
void
binit(void) {
    struct buf* b;

    // 初始化 bcache
    initlock(&bcache.lock, "bcache");
    bcache.glob_tstamp = 0;
    
    // 初始化所有 bbucket
    struct bbucket* bkt;
    for (bkt = bcache.hashbkt; bkt < bcache.hashbkt + NBUCKET; bkt++) {
        initlock(&bkt->lock, "bbucket");
        bkt->entry_num = 0;
    }

    // 初始化 bcache.buf 数组（即缓存池）中的所有缓存块
    for (int i = 0; i < NBUF; i++) {
        b = &bcache.buf[i];
        initsleeplock(&b->lock, "buffer");
        // bcache.buf[i] 设为 blockno 为 i 的块。
        b->blockno = i;
        b->refcnt = 0;
        b->tstamp = 0;
        // 注意 valid 一定要设为 0
        //表示缓存块中的数据是无效的，bread 到这个块时需要从磁盘重新读取数据
        b->valid = 0;
        // 向对应的哈希桶中分发这个块
        bkt = bhash(b->blockno);
        b->bktord = bkt->entry_num;
        bkt->bentry[bkt->entry_num++] = b;
    }
}
```

由于用数组替换了时序链表，接下来对 `bread()`、`bwrite()` 和 `brelse()` 等函数进行修改。

```c

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno) {
    struct buf* b;
    b = bget(dev, blockno);
    b->tstamp = ++bcache.glob_tstamp;
    if (!b->valid) {
        // 如果 valid 为 0，需要从磁盘读
        virtio_disk_rw(b, 0);
        b->valid = 1;
    }
    return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf* b) {
    if (!holdingsleep(&b->lock))
        panic("bwrite");
    b->tstamp = ++bcache.glob_tstamp;
    virtio_disk_rw(b, 1);
}

void
brelse(struct buf* b) {
    if (!holdingsleep(&b->lock))
        panic("brelse");

    struct bbucket* bkt = bhash(b->blockno);
    releasesleep(&b->lock);

    acquire(&bkt->lock);
    b->refcnt--;
    if (b->refcnt == 0) {
        // no one is waiting for it.
        b->tstamp = 0;
    }

    release(&bkt->lock);
}

void
bpin(struct buf* b) {
    struct bbucket* bkt = bhash(b->blockno);
    acquire(&bkt->lock);
    b->refcnt++;
    b->tstamp = ++bcache.glob_tstamp;
    release(&bkt->lock);
}

void
bunpin(struct buf* b) {
    struct bbucket* bkt = bhash(b->blockno);
    acquire(&bkt->lock);
    b->refcnt--;
    release(&bkt->lock);
}
```

最后是重头戏 `bget()`。这里我们先给出一种直观的算法，但后面会看到这算法可能会出现死锁。算法思路已经写在了注释中。

```c
// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
// 注意 bget 的归约：只要求返回一个缓存块指针
// 不要求缓存块中的实际数据是磁盘块 blockno 中的内容
// 可能出现死锁的 bget
static struct buf*
bget_ddlock(uint dev, uint blockno) {
    // 找到 blockno 对应的哈希桶 bkt
    struct bbucket* bkt = bhash(blockno);
    // 获得这个 bucket 的小锁 bkt->lock
    acquire(&bkt->lock);
    // 在桶中寻找 blockno，如果找到就直接返回
    for (int i = 0; i < bkt->entry_num; i++) {
        struct buf* b = bkt->bentry[i];
        if (b->dev == dev && b->blockno == blockno) {
            b->refcnt++;
            // 找到 blocknum，释放 bkt->lock
            release(&bkt->lock);
            acquiresleep(&b->lock);
            return b;
        }
    }
    
    // 未找到 blockno，需要在缓存池中进行 eviction & replacement
    // 这个过程需要串行化，先获得 bcache 大锁
    acquire(&bcache.lock);
    
    // 这里使用 while 循环的原因在循环体中解释
    while (1) {
        struct buf* evict_buf = 0;
        uint min_tstamp = 0xffffffff;
        // 寻找缓存池中时间戳最小的缓存块 evict_buf 进行驱逐
        for (struct buf* b = bcache.buf; b < bcache.buf + NBUF; b++) {
            if (b->refcnt == 0 && b->tstamp < min_tstamp) {
                min_tstamp = b->tstamp;
                evict_buf = b;
            }
        }
        // 如果未找到引用计数 refcnt 为 0 的块，就 panic 报错
        if (!evict_buf) {
            panic("bget: no buffers");
        }
        // 获取 evict_buf 所在的哈希桶 evict_bkt
        struct bbucket* evict_bkt = bhash(evict_buf->blockno);
        // 只有 bkt 和 evict_bkt 不一样时才获取后者的锁（避免重复获取同一把锁）
        if (evict_bkt != bkt) {
            acquire(&evict_bkt->lock);
        }
        // 这一步很关键：必须重新检查 evict_buf 的 refcnt是否为 0
        // 因为从「找到 evict_buf（32 行）」到「获取 evict_bkt->lock（43 行）」
        // 这段时间里，我们没有任何措施保证 evict_buf 没有被别的进程引用
        if (evict_buf->refcnt != 0) {
            // refcnt 不再是 0 了
            // 需要把 evict_bkt->lock 释放掉（如果两个桶不同）
            // 并回到循环开始处，重新寻找可用的 evict_buf
            if (evict_bkt != bkt) {
                release(&evict_bkt->lock);
            }
            // 这就是最外层要套一个 while 的原因
            continue;
        }
        // evict_buf 可以被驱逐并替换
        // 记 num 为 evict_bkt 中的缓存块数目
        uint num = evict_bkt->entry_num;
        // 把 evict_bkt 中的最后一个缓存块挪到 evict_buf 所在的位置
        if (evict_buf->bktord < num - 1) {
            evict_bkt->bentry[evict_buf->bktord] = evict_bkt->bentry[num - 1];
            evict_bkt->bentry[evict_buf->bktord]->bktord = evict_buf->bktord;
        }
        // 把 evict_bkt 的缓存块数目减 1
        evict_bkt->entry_num--;
        // evict_bkt 已经访问完毕（如果两个桶不同），锁释放掉
        if (evict_bkt != bkt) {
            release(&evict_bkt->lock);
        }
		
        // 牢记所有缓存块都是存储在 cache.buf 数组中的
        // bcache.bbucket 中的只是指针
        // 所以现在我们直接把 evict_buf 的指针塞到 bkt 的最后
        // 然后改改 evict_buf 的信息就能返回了
        // bktord 改为 bkt 的大小
        evict_buf->bktord = bkt->entry_num;
        // bkt 的最后添加 evict_buf
        bkt->bentry[bkt->entry_num++] = evict_buf;
        // 设置 evict_buf 的 dev 和 blockno
        evict_buf->dev = dev;
        evict_buf->blockno = blockno;
        // valid 设为 0，表示缓存块中的数据是无效的，需要重新从磁盘中读取
        evict_buf->valid = 0;
        // 引用计数设为 1（对这个块调用 brelse() 时会减去）
        evict_buf->refcnt = 1;
        acquiresleep(&evict_buf->lock);
        // 释放 bcache 大锁和 bkt 小锁
        release(&bcache.lock);
        release(&bkt->lock);
        return evict_buf;
    }
}
```

这个算法看上去考虑得相当周全了。但是，我们会发现在 12 行、27 行和 48 行，出现了如下顺序的 `acquire()`。

```c
acquire(&bkt->lock);
acquire(&bcache.lock);
acquire(&evict_bkt->lock);
```

这是非常经典的「大小交替」所形成的死锁。例如，如果有 2 个进程 A 和 B，其中进程 A 

1. 先获得了桶 0 的锁，
2. 再获得了 `bcache` 的锁，
3. 然后试图获得桶 1 的锁。

而进程 B 

1. 先获得桶 1 的锁，
2. 试图获得 `bcache` 的锁。

此时，两个进程之间就发生了死锁。因此，我们需要调整 `acquire` / `release` 锁的策略，把死锁给消掉。一个常见的策略是，始终「先大后小」或「先小后大」。对于前者，就是永远不在获取了较小粒度的锁（且未释放）之后获取较大粒度的锁，对于后者亦然。在 `bget()` 中，如果我们遵循「先大后小」，由于获取 `bkt->lock` 和 `bcache->lock` 的顺序是变不了的，因此我们只能在获取大锁 `bcache->lock` 之前先释放小锁 `bkt->lock`，然后再把小锁 `bkt->lock` 拿回来。于是就获得了如下的修改方案。

```c
acquire(&bkt->lock);
...
// 未找到 blockno
// 为了拿大粒度的锁，先释放较小粒度的锁 bkt->lock
release(&bkt->lock);
// 获取较大粒度的锁 bcache->lock
acquire(&bcache.lock);
// 重新把 bkt->lock 拿回来
acquire(&bkt->lock);

// 进入 while 循环...
acquire(&evict_bkt->lock);
```

但这样会带来一个附加的问题：把 `bkt->lock` 释放掉再拿回来的这段时间，可能会有进程「乘虚而入」，修改 缓存池和 `bkt` 中的内容。例如有两个 `blockno` 参数相同的进程 A 和 B，在进程 A 释放掉 `bkt->lock` （假设 A 的 `bkt` 是桶 0）之后，另一已获取 `bcache->lock` 的进程 B 获取了 `evict_bkt->lock` （B 的`evict->bkt` 同样是桶 0），并将 `blockno` 这个磁盘块写入了缓存池中。这时，如果进程 A 拿到大锁和小锁，继续往下执行，就会第二次写入 `blockno` 这个块，造成缓存池中出现重复块。因此，在拿到 `bcache->lock` 和 `bkt->lock` 之后，第一件事应该是再次检查 `bkt` 中是否有 `blockno` 这个块。如果仍然没有，再继续往下执行，否则直接返回那个缓存块地址即可。

你可能会问，既然总是要重复检查 `blockno` 的存在性，为何不直接删去第一次检查呢？删去后确实仍然是正确的，但这样并没有提高效率：这样等于直接退化成了原始的没有分桶的 `bcache` 版本，在 `bget()` 上没有任何并行性了。

最终版本的 `bget()` 代码如下。

```c
// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno) {

    // Is the block already cached?
    struct bbucket* bkt = bhash(blockno);
    acquire(&bkt->lock);
    for (int i = 0; i < bkt->entry_num; i++) {
        struct buf* b = bkt->bentry[i];
        if (b->dev == dev && b->blockno == blockno) {
            b->refcnt++;
            release(&bkt->lock);
            acquiresleep(&b->lock);
            return b;
        }
    }

    // Not cached.
    // Recycle the least recently used (LRU) unused buffer.
    // Release bkt->lock at first, preventing dead locks
    release(&bkt->lock);
    acquire(&bcache.lock);
    acquire(&bkt->lock);

    // Recheck for existance of the block
    for (int i = 0; i < bkt->entry_num; i++) {
        struct buf* b = bkt->bentry[i];
        if (b->dev == dev && b->blockno == blockno) {
            b->refcnt++;
            release(&bcache.lock);
            release(&bkt->lock);
            acquiresleep(&b->lock);
            return b;
        }
    }

    // Loop for the eviction
    while (1) {
        struct buf* evict_buf = 0;
        uint min_tstamp = 0xffffffff;
        for (struct buf* b = bcache.buf; b < bcache.buf + NBUF; b++) {
            if (b->refcnt == 0 && b->tstamp < min_tstamp) {
                min_tstamp = b->tstamp;
                evict_buf = b;
            }
        }
        if (!evict_buf) {
            panic("bget: no buffers");
        }
        struct bbucket* evict_bkt = bhash(evict_buf->blockno);
        if (evict_bkt != bkt) {
            acquire(&evict_bkt->lock);
        }
        // Recheck refcnt after obtaining the lock
        if (evict_buf->refcnt != 0) {
            if (evict_bkt != bkt) {
                release(&evict_bkt->lock);
            }
            // refcnt is not 0, reseek for an available buffer block
            continue;
        }
        // This block can be evicted
        uint num = evict_bkt->entry_num;
        if (evict_buf->bktord < num - 1) {
            evict_bkt->bentry[evict_buf->bktord] = evict_bkt->bentry[num - 1];
            evict_bkt->bentry[evict_buf->bktord]->bktord = evict_buf->bktord;
        }
        evict_bkt->entry_num--;
        if (evict_bkt != bkt) {
            release(&evict_bkt->lock);
        }

        evict_buf->bktord = bkt->entry_num;
        bkt->bentry[bkt->entry_num++] = evict_buf;
        evict_buf->dev = dev;
        evict_buf->blockno = blockno;
        evict_buf->valid = 0;
        evict_buf->refcnt = 1;
        acquiresleep(&evict_buf->lock);
        release(&bcache.lock);
        release(&bkt->lock);
        return evict_buf;
    }
}
```

### 测试结果

测试结果为 70/70 分，且 kalloctest 的 `tot` 和 bcachetest 的 `tot` 都是 0。

![image-20210623221807169](https://i.loli.net/2021/06/23/hbp2Ifv1A9d4wRX.png)

### 附加挑战

#### Lock-free buffer cache

> Make lookup in the buffer cache lock-free. Hint: use gcc's `__sync_*` functions. How do you convince yourself that your implementation is correct?

这也是个有趣的新课题：无锁缓存池。提示使用 gcc 的 `__sync_*` 函数。挖坑待填。
