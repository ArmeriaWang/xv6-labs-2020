# Lab: file system

实验指导：[Lab: file system (mit.edu)](https://pdos.csail.mit.edu/6.828/2020/labs/fs.html)

通过全部测试的参考代码：https://github.com/ArmeriaWang/xv6-labs-2020/tree/fs

## 前置知识

### inode

文件系统中核心的数据结构是 inode 和 file descriptor。后者主要与用户进程进行交互，在 Lab util 中已经介绍过。本章节主要涉及的是 inode。

我们在使用操作系统时，看到的「文件」其实是「文件名」，而真正存储文件的数据结构是 inode，每个文件名会关联一个 inode。一个 inode 存储一个文件对象，并且它不依赖于文件名。实际上，inode 是通过自身的编号来进行区分的。每个 inode 通过自身的 link count 来跟踪指向这个 inode 的文件名的数量。一个文件（inode）只能在 link count 为 0 的时候被删除。xv6 中的 `struct inode` 是一个 64 字节的数据结构，如下图所示。

![image-20210624130358279](https://i.loli.net/2021/06/24/1j7JVw6zxLbrlIW.png)

图中 inode 主要字段说明如下。

- `type` 字段，表明 inode 是文件还是目录。
- `nlink` 字段，也就是 link count 计数器，用来跟踪究竟有多少文件名指向了当前的 inode。
- `size` 字段，表明了文件数据有多少个字节。
- `address[1~12]` 字段，存储 12 个 direct blocks 的序号，这 12 个 block 按顺序存储了文件的前 `12 * BSIZE` 字节的内容。
- 最后的 `indirect` 字段类似于一级页表指针，存储了 indirect block 的序号。由于 xv6 每个 block 是 1024 字节，且一个 block 序号都是 `uint`（4 字节），所以这个 indirect block 中包含 256 个 data block 序号。再加上 12 个 direct blocks， 总共有 268 个 data blocks。这就是 xv6 所支持的最大文件的 block 数。

### 文件系统使用磁盘的方法

文件系统的工作就是将所有的数据结构以一种**能够在重启之后重新构建文件系统**的方式，存放在磁盘上。xv6 中的磁盘布局如下图所示。

![img](https://i.loli.net/2021/06/24/4lfmqDIni6QAvRK.png)

通常来说：

- block0 要么没有用，要么被用作 boot sector 来启动操作系统。
- block1 通常被称为 super block，它描述了文件系统。它可能包含磁盘上有多少个 block 共同构成了文件系统这样的信息。我们之后会看到 xv6 在里面会存更多的信息，你可以通过 block1 构造出大部分的文件系统信息。
- 在 xv6 中，log 从 block2 开始，到 block32 结束。实际上 log 的大小可能不同，这里在 super block 中会定义 log 就是 30 个 block。
- 接下来在 block32 到 block45 之间，xv6 存储了 inode。我之前说过多个 inode 会打包存在一个 block 中，一个 inode 是 64 字节。
- 之后是 bitmap block，这是我们构建文件系统的默认方法，它只占据一个 block。它记录了各个数据 block 否空闲。
- 之后就全是数据 block 了，数据 block 存储了文件的内容和目录的内容。

## 参考做法

### Large files

本任务要求给 xv6 添加对大文件的支持。具体来说，就是要把 inode 中 12 个 direct block + 1 个 singly indirect 的组合，改为 11 个 direct block + 1 个 singly indirect + 1 个 doubly indirect 的组合。其中，doubly indirect block 存储的是 256 个额外 singly indirect block 的序号（类似于二级页表）。修改后的 inode 结构如下图所示（图片[来源见此](https://blog.csdn.net/RedemptionC/article/details/108410259)）。

![img](https://i.loli.net/2021/06/24/5d78MbrIeipjLSO.png)

这样，最大文件的 block 数量就增加到 `$11 + 1 \times 256 + 1\times 256\times 256 = 65803$ `个。 

1. 修改代表 direct block 数量的常数 `NDIRECT` 和代表最大文件 block 数的常数 `MAXFILE`，别忘了同时修改 `struct inode` 的 `addrs` 数组的长度表达式。

   ```c
   // direct block 数量
   #define NDIRECT 11  
   // singly indirect block 所链接的 block 数量
   #define NINDIRECT (BSIZE / sizeof(uint))
   // doubly indirect block 所链接的 block 数量
   #define NINDIRECT2 (NINDIRECT * NINDIRECT)
   // 所支持的最大文件的 block 数量
   #define MAXFILE (NDIRECT + NINDIRECT + NINDIRECT2)
   
   
   struct inode {
   	...
       // +1 修改为 +2
     	uint addrs[NDIRECT + 2];
   };
   ```

2. 然后修改 `bmap()` 函数，添加对 doubly indirect block 的支持。

   ```c
   // Inode content
   //
   // The content (data) associated with each inode is stored
   // in blocks on the disk. The first NDIRECT block numbers
   // are listed in ip->addrs[].  The next NINDIRECT blocks are
   // listed in block ip->addrs[NDIRECT].
   
   // Return the disk block address of the nth block in inode ip.
   // If there is no such block, bmap allocates one.
   static uint
   bmap(struct inode* ip, uint bn) {
       uint addr, * a;
       struct buf* bp;
   
       if (bn < NDIRECT) {
           if ((addr = ip->addrs[bn]) == 0)
               ip->addrs[bn] = addr = balloc(ip->dev);
           return addr;
       }
       bn -= NDIRECT;
   
       if (bn < NINDIRECT) {
           // Load indirect block, allocating if necessary.
           if ((addr = ip->addrs[NDIRECT]) == 0)
               ip->addrs[NDIRECT] = addr = balloc(ip->dev);
           bp = bread(ip->dev, addr);
           a = (uint*)bp->data;
           if ((addr = a[bn]) == 0) {
               a[bn] = addr = balloc(ip->dev);
               log_write(bp);
           }
           brelse(bp);
           return addr;
       }
       bn -= NINDIRECT;
   
       if (bn < NINDIRECT2) {
           // Load doubly-indirect block, allocating if necessary
           uint addr;
           uint idx_bno = bn / (BSIZE / sizeof(uint));
           uint data_bno = bn % (BSIZE / sizeof(uint));
           if ((addr = ip->addrs[NDIRECT + 1]) == 0)
               ip->addrs[NDIRECT + 1] = addr = balloc(ip->dev);
           bp = bread(ip->dev, addr);
           a = (uint*)bp->data;
           if ((addr = a[idx_bno]) == 0) {
               a[idx_bno] = addr = balloc(ip->dev);
               log_write(bp);
           }
           brelse(bp);
           bp = bread(ip->dev, addr);
           a = (uint*)bp->data;
           if ((addr = a[data_bno]) == 0) {
               a[data_bno] = addr = balloc(ip->dev);
               log_write(bp);
           }
           brelse(bp);
           return addr;
       }
   
       panic("bmap: out of range");
   }
   ```
   
3. 最后修改 `itrunc()` 函数（该函数用于清除 inode 信息）。

   ```c
   // Truncate inode (discard contents).
   // Caller must hold ip->lock.
   void
   itrunc(struct inode* ip) {
       int i, j;
       struct buf* bp;
       uint* a;
   
       for (i = 0; i < NDIRECT; i++) {
           if (ip->addrs[i]) {
               bfree(ip->dev, ip->addrs[i]);
               ip->addrs[i] = 0;
           }
       }
   
       if (ip->addrs[NDIRECT]) {
           bp = bread(ip->dev, ip->addrs[NDIRECT]);
           a = (uint*)bp->data;
           for (j = 0; j < NINDIRECT; j++) {
               if (a[j])
                   bfree(ip->dev, a[j]);
           }
           brelse(bp);
           bfree(ip->dev, ip->addrs[NDIRECT]);
           ip->addrs[NDIRECT] = 0;
       }
   
       if (ip->addrs[NDIRECT + 1]) {
           bp = bread(ip->dev, ip->addrs[NDIRECT + 1]);
           a = (uint*)bp->data;
           for (j = 0; j < NINDIRECT; j++) {
               if (a[j]) {
                   struct buf* bp1 = bread(ip->dev, a[j]);
                   uint* a1 = (uint*)bp1->data;
                   for (int k = 0; k < NINDIRECT; k++) {
                       if (a1[k])
                           bfree(ip->dev, a1[k]);
                   }
                   brelse(bp1);
                   bfree(ip->dev, a[j]);
               }
           }
           brelse(bp);
           bfree(ip->dev, ip->addrs[NDIRECT + 1]);
           ip->addrs[NDIRECT + 1] = 0;
       }
   
       ip->size = 0;
       iupdate(ip);
   }
   ```


### Symbolic links

本任务要求在 xv6 中添加对软连接（Soft Links，或称符号链接， Symbolic Links）的支持。下面简单介绍硬链接（Hard Links）和软连接的区别。假设目录 A 中有一个文件 `f0`，其 inode 编号为 2333。现在我们希望在另一目录 B 中创建 `f0` 的链接。

- 创建硬链接后，相当于在 B 目录下建立了一个新的文件名，且关联的 inode 编号为 2333。这就是说，inode 2333 的 `nlink` 字段变成了 2，除非两个文件（名）同时删除，这个 inode 是不会从磁盘上抹去的。
- 创建软链接后，文件系统会为这个软链接新建一个文件名，并关联一个新建的 inode，其中保存的是 `f0` 的路径。换句话说，软链接就是一个保存路径的特殊的文本文件。至于路径所指向的文件的具体情况，甚至那个是否存在，软链接本身都是不知道的。同样的，被链接文件 inode 也不知道软链接的存在（即不会计入到 `nlink`）。

本任务中，我们通过新建系统调用 `symlink`，实现软链接。其核心代码如下。

```c
// def.h
int symlink(const char*, const char*);

// param.h
// 最大循环链接计数，如果
#define MAXLOOPCNT 23

// sysproc.c
uint
sys_symlink(void) {
    char target[MAXPATH], path[MAXPATH];
    memset(target, 0, sizeof(target));
    struct inode *ip;

    if (argstr(0, target, MAXPATH) < 0 || argstr(1, path, MAXPATH) < 0)
        return -1;

    //开始事务
    begin_op();

    if((ip = create(path, T_SYMLINK, 0, 0)) == 0) {
        end_op();
        return -1;
    }

    if (writei(ip, 0, (uint64)target, 0, MAXPATH) != MAXPATH) {
        return -1;
    }

    iunlockput(ip);

    // 结束事务
    end_op();

    return 0;
}

uint64
sys_open(void) {
    ...
    if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
        iunlockput(ip);
        end_op();
        return -1;
    }
    
	// add from here
    if (ip->type == T_SYMLINK) {
        if (!(omode & O_NOFOLLOW)) {
            int loop_cnt = 0;
            char target[MAXPATH];
            // 沿着软链接走，如果循环超过 MAXLOOPCNT 次，就返回 -1
            while (ip->type == T_SYMLINK) {
                if (loop_cnt > MAXLOOPCNT) {
                    iunlockput(ip);
                    end_op();
                    return -1;
                }
                loop_cnt++;
                readi(ip, 0, (uint64)target, 0, MAXPATH);
                iunlockput(ip);
                if ((ip = namei(target)) == 0) {
                    end_op();
                    return -1;
                }
                ilock(ip);
            }
        }
    }
	// end

    if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
        if(f)
            fileclose(f);
        iunlockput(ip);
        end_op();
        return -1;
    }
    ...
}
```

### 测试结果

测试结果为 100/100 分。

![image-20210624151208258](https://i.loli.net/2021/06/24/QW5GUVY43yaIpAu.png)

### 附加挑战

#### Triple-indirect blocks

很容易的附加挑战，仿照三级页表即可。
