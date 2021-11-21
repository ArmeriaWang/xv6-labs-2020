# Lab: Xv6 and Unix utilities

实验指导：[Lab: Xv6 and Unix utilities (mit.edu)](https://pdos.csail.mit.edu/6.828/2020/labs/util.html)

通过全部测试的参考代码：https://github.com/ArmeriaWang/xv6-labs-2020/tree/util

## 前置知识 

### 文件描述符 fd

文件描述符（fd）是操作系统对进程 IO 对象的一个抽象（字节流）。每个 fd 都是一个小整数，但它实际上可能是文件、目录、硬件、管道或对其他fd的复制。fork 出的子进程会复制父进程的 fd 表。每个 fd 实际上维护了三个信息：

- 所指向的 IO 对象（如一个文本文件）
- 文件状态（如可读、可写等）
- 当前的偏移 offset（如第 2 个字节处）

注意，同一个文件可以有多个 fd，且它们的状态和偏移可以不同。

Unix默认的 fd 有：

- fd = 0：`stdin`
- fd = 1：`stdout`
- fd = 2：`stderr`

之所以要把 `fork` 和 `exec` 分成两个系统调用，是因为这样可以在这两个调用之间对子进程的 IO 文件描述符进行重定向，保持子进程对文件描述符表的抽象。如果合成一个 `forkexec` 的话，结果就是，要么父进程连着自己的 fd 表一起改掉，要么在 `forkexec` 中加入 IO 重定向的相关参数，要么就得让子进程（如 `cat` 等）自己写一套 IO 机制（这也是最差的解决方案）。

两个 fd 共享相同的偏移，当且仅当这它们是通过 `fork` 或 `dup` 复制而来。通过两次 `open` 打开相同的文件，它们也不会共享偏移。

```c
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

int main() {
    int fd = open("in.txt", O_WRONLY | O_CREAT | O_TRUNC);
    int fd2 = open("in.txt", O_WRONLY | O_CREAT);
    
    write(fd, "hello ", 6);
    write(fd2, "world\n", 6);
    
    return 0;
}
```

运行以上代码，`in.txt` 中只有一行 `world\n`。这是因为 fd 与 fd2 的偏置互相独立运作，fd 写入 `hello` 后，fd2 的偏置仍为0，后者会覆盖地写入 `world\n`。

### 管道 pipe

pipe 一种进程间通信的机制。从进程角度看，它是一对 fd，一个用于读一个用于写；从内核角度看，它是一小段为进程暴露出来的内核缓存。

```C
#include <fcntl.h>
#include <unistd.h>

int main()
{
    int p[2];
    char *argv[2];
    argv[0] = "wc";
    argv[1] = 0;
    
    pipe(p);  // 获取一对 pipe fd，p[0] 为写入端，p[1] 为读取端
    
    if (fork() == 0) {  // 子进程，先将 stdin 重定向
        close(0);  // 关闭stdin，为重定向作准备
        dup(p[0]);  // 复制管道读取端，且此时读取端共享了 fd = 0
        close(p[0]);  // 关闭原先的读取端 p[0]，只留下 fd = 0
        close(p[1]);  // 关闭管道写入端（仅由父进程使用）。这一步不仅是为了节约系统资源，而且是必须的：如果保留它，那么就相当于有个子进程的 fd（而且 exec 后子进程还不知道它是谁）指向了管道写入端的末尾，导致子进程会始终读不到EOF
        exec("/bin/wc", argv);  // 运行 wc 程序，此时已完成 pipe 重定向
    }
    else {
        close(p[0]);  // 关闭读取端（仅由子进程使用）
        write(p[1], "hello world\n", 12);  // 向管道中写入，此时子进程将会从 fd = 0 处读取到这些数据
        close(p[1]);  // 程序即将结束，关闭写入端。这相当于传入 EOF，读入端由此得知管道中的字节流已结束
    }
}
```

上面的代码与 shell 中管道 `|` 的实现非常相似，只不过 shell 需要 fork 两次，分别执行 `|` 的左边命令 `runcmd(pcmd->left)` 和右边命令 `runcmd(pcmd->right)`：

```c
case PIPE:
pcmd = (struct pipecmd *)cmd;
if (pipe(p) < 0)
    panic("pipe");
if (fork1() == 0) {
    close(1);
    dup(p[1]);
    close(p[0]);
    close(p[1]);
    runcmd(pcmd->left);
}
if (fork1() == 0) {
    close(0);
    dup(p[0]);
    close(p[0]);
    close(p[1]);
    runcmd(pcmd->right);
}
close(p[0]);
close(p[1]);
wait(0);
wait(0);
break;
```

例如，对于以下命令（对一个文本，先分割单词，然后全转成小写，最后排序）：

```shell
$ split | lowercase | sort
```

shell 解析到左边的 `|`，会 fork 出两个子进程 A 和 B，获取管道 `p1`。

- A 把 `p1` 的读取端关闭，把 `p1` 的写入端与 fd = 1 共享。解析左边的 `split`，直接执行 `split` 程序，输出到 `p1` 的写入端中；
- B  把`p1` 的写入端关闭，把 `p1` 的读取端与 fd = 0 共享。解析右边的 `lowercase | sort`。然后发现还有管道 `|`，于是再次 fork 出两个新进程 C 和 D，获取管道 `p2`；
  - C 把 `p2` 的读取端关闭，把 `p2` 的写入端与 fd = 1 共享。解析出左边的 `lowercase`，直接执行 `lowercase` 程序，从 `p1` 的读取端中读入 `split` 的输出，输出到 `p2` 的写入端中；
  - D 把 `p2` 的写入端关闭，把 `p2` 的读入端与 fd = 0 共享。解析出右边的 `sort`，直接执行 `sort` 程序，从 `p2` 的读取端中读入 `lowercase` 的输出，输出到 `stdout` 中。

可以看到，shell 为了实现连续的管道传输，创建出了一个树状的进程结构，所有的叶子节点都是具体的执行程序（如上面的 A、C 和 D）；而非叶子节点则是解析节点，它需要等待左右儿子进程执行完毕（如上面的 B）。

### 文件系统

Unix系统把文件看做一系列无差别的字节序列。文件系统的组织是树状的，根节点是一个特殊的根目录 `/` 。如果一定对文件分类，可以大致分为以下三类：

- 目录 directory，用系统调用 `mkdir` 创建；
- 数据文件 data file，用系统调用 `open` 搭配 `O_CREAT` 标志创建；
- 设备文件 device file，用系统调用 `mknod` 创建。

例如，`mknod("/console", 1, 1);` 就创建了一个名为`console` 的设备文件。`mknod` 创建一个引用设备的特殊文件。 与设备文件相关的是主设备号和次设备号（`mknod` 的后两个参数），它们相组合就能够唯一地标识内核设备。 当进程稍后打开设备文件时，内核会将读写系统调用传递到内核设备实现中，而不是将它们传递到文件系统中。 

Unix 的文件可以看做两层，文件本身和文件名。同一个底层文件 inode 可以有多个文件名，后者被称作链接 links。每个 link 包括一个文件名和一个到 inode 的引用。inode 则维护文件的元数据 metadata，包括其类别（目录、数据文件或设备）、长度、在磁盘上的位置以及引用该 inode 的 link 数量。

几个相关命令：

-  `fstate` 系统调用可以获取一个 fd 所指向的 inode 的信息（以 `state` 结构体形式呈现）；
- `link("a", "b")` 系统调用可以新建 link `b`，并将 b 的读写指向 link `a` 的 inode；
- `unlink("a")`  系统调用可以删除 link `a`，并将对应 inode 的引用计数减1。当这个计数减到0后，inode 文件所占用的磁盘空间才会被释放。

## 参考做法

本实验主要是让学生熟悉 xv6 中系统调用的使用方法。

### sleep

直接进行系统调用 `sleep` 即可。

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    if (argc <= 1) {
        fprintf(2, "usage: sleep seconds...\n");
        exit(1);
    }
    int slp_sec = atoi(argv[1]);
    sleep(slp_sec);
    exit(0);
}
```

### pingpong

要求在父子进程之间利用管道 pipe 相互传递一个字节。利用 `fork` 和  `dup` 即可完成。

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    int pa[2], pb[2];
    char buf[4];
    pipe(pa);
    pipe(pb);
    if (fork() == 0) {  // child process
        close(0);
        dup(pa[0]);
        close(1);
        dup(pb[1]);
        read(0, buf, 4);
        fprintf(2, "%d: received %s\n", getpid(), buf);
        write(1, "pong", 4);
        exit(0);
    }
    else {
        close(0);
        dup(pb[0]);
        close(1);
        dup(pa[1]);
        write(1, "ping", 4);
        read(0, buf, 4);
        fprintf(2, "%d: received %s\n", getpid(), buf);
    }
    exit(0);
}
```

### primes

要求利用 pipe 实现快速筛出 2 ~ 35 之间的质数。基本思想如下图所示，每个方框代表一个进程。

<img src="https://i.loli.net/2021/06/18/lziUfLoRS9p8KF7.png" alt="image-20210618231700027" style="zoom:67%;" />

网上的其他解法基本都用了 `goto`，这里给出一种递归的写法。

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void sieve(int p[]) {
    int num, x;
    close(0);
    dup(p[0]);
    close(p[1]);

    // 记录第一个数为 x，并输出它是质数
    read(p[0], &x, sizeof(x));
    fprintf(2, "prime %d\n", x);
    int len = read(p[0], &num, sizeof(num));
    if (len != sizeof(num)) {
        // 如果无法继续读入（管道写入端关闭），则退出
        close(p[0]);
        exit(0);
    }

    int newp[2];
    // 开一对新的管道，用于本进程及其子进程之间的通信
    pipe(newp);
    if (fork() == 0) {
        // 子进程递归处理，从管道中接收父进程交付的自然数并进行筛选
        sieve(newp);
    }
    else {
        // 父进程，把从祖父进程中收到的数用x筛过后交付给子进程
        close(newp[0]);

        do {
            if (num % x != 0) {
                // 若新读入的数不能被 x 整除，就写入管道
                write(newp[1], &num, sizeof(num));
            }
        } while (read(p[0], &num, sizeof(num)) == sizeof(num));

        close(newp[1]);
        close(p[0]);
        wait(0);
        exit(0);
    }
}

int main(int argc, char* argv[]) {
    int p[2];
    pipe(p);

    if (fork() == 0) {
        // 子进程，从管道中接收父进程交付的自然数并进行筛选
        sieve(p);
    }
    else {
        // 父进程，向管道中写入 2 ~ 35 之间的自然数。
        close(p[0]);

        for (int x = 2; x <= 35; x++) {
            write(p[1], &x, sizeof(x));
        }

        close(p[1]);
        wait(0);
        exit(0);
    }
    return -1;
}
```

### find

要求利用系统调用 `find`，实现 UNIX 系统中的 `find` 应用程序：在给定目录下，递归地寻找具有给定文件名的所有文件。

遵照实验指导的提示一步步做即可。

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

void find(char path[], char fname[], char pre_de_name[], int flag) {
    char buf[512], * p;
    int fd;
    struct dirent de;
    struct stat st;

    if ((fd = open(path, 0)) < 0) {
        fprintf(2, "find: cannot open %s\n", path);
        close(fd);
        return;
    }

    if (fstat(fd, &st) < 0) {
        fprintf(2, "find: cannot stat %s\n", path);
        close(fd);
        return;
    }

    if (st.type == T_FILE || st.type == T_DEVICE) {
        if (!flag) {
            fprintf(2, "find: %s is not a directory\n", path);
            close(fd);
            return;
        }
        if (strcmp(pre_de_name, fname) == 0) {
            fprintf(1, "%s\n", path);
        }
        close(fd);
        return;
    }

    if (strlen(path) + 1 + DIRSIZ + 1 > sizeof(buf)) {
        printf("find: path too long\n");
        close(fd);
        return;
    }

    strcpy(buf, path);
    p = buf + strlen(buf);
    *(p++) = '/';
    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
        if (de.inum == 0) {
            continue;
        }
        memmove(p, de.name, DIRSIZ);
        p[DIRSIZ] = 0;
        if (stat(buf, &st) < 0) {
            printf("find: cannot stat %s\n", buf);
            continue;
        }
        if (strcmp(de.name, ".") != 0 && strcmp(de.name, "..") != 0) {
            // 在当前目录下递归查找
            find(buf, fname, de.name, 1);
        }
    }

    close(fd);
}

int main(int argc, char* argv[])
{
    if (argc <= 2) {
        fprintf(2, "usage: find path filename...\n");
        exit(1);
    }
    find(argv[1], argv[2], 0, 0);
    exit(0);
}
```

### xargs

要求利用 `fork` 和 `exec` 系统调用实现 UNIX 的 `xargs` 应用程序。

`xrgs` 一般用于命令参数很多或待定的情形。例如，我们想在 `file0`、`file1` ... `file9` 这些文件中寻找包含 `keyword` 的行，就可以利用 `xargs`，命令形式为 `xargs [-options] [command] [arg lines]`，即输入命令后，每行输入一个参数。

```bash
$ xargs grep keyword  # 执行命令 grep keyword，以下每行都是后续参数，用 EOF 结束
file0
file1
...
file9
EOF
# 以上为输入，以下为输出
This is the first line of file0, and have a ‘keyword’.
This is the first line of file1, and have a ‘keyword’.
...
This is the thrid line of file9, and have a ‘keyword’.
```

这么看 `xargs` 只是一个分行工具而已，实际上它远不止于此——它还可以用于命令参数「待定」的情形；具体地说，就是命令参数来自其他命令的输出。例如，假设我们想在当前目录下文件名包含 `file` 的文件中，寻找包含 `keyword` 的行，就可以用如下命令。

```bash
$ find . -type f -name "*file*" | xargs grep keyword
```

可以看到，管道和 `xargs` 把两个命令完美地结合到了一起。

这里我们注意区分以下两个命令。

```bash
$ find . -type f -name "*file*" | xargs grep keyword
$ find . -type f -name "*file*" | grep keyword
```

前者是把 `find` 的输出重定向给 `xargs`（也就是作为 `grep` 的参数使用），效果是在 `find` 找到的文件的文件内容中找 `keyword`。后者则是把 `find` 的输出直接重定向给 `grep`，效果是让 `grep` 在 `find` 输出的那一堆文件路径字符串中找 `keyword`；换句话说，就是把 `find` 的输出结果作为一个临时文件交给 `grep`，让 `grep` 在这个临时文件中找 `keyword`。

按照要求，我们的 xargs 的实现只需要简单地对每个输入行重复执行同样的命令。

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"


#define MAXARGLEN 32
#define MAXCMDLEN 256

char argx[MAXARG][MAXARGLEN], *use_argx[MAXARG];
char cmd[MAXCMDLEN];

// 读入一行参数
int read_line(int *arg_cnt) {
    char ch;
    int len = 0, ok = read(0, &ch, 1);
    if (ok < 1) return 0;
    while (ch != '\n' && *arg_cnt < MAXARG - 1) {
        while (ch != ' ' && ch != '\n' && len < MAXARGLEN - 1) {
            argx[*arg_cnt][len++] = ch;
            if (read(0, &ch, 1) < 1) break;
        }
        argx[*arg_cnt][len] = '\0';
        if (len > 0) (*arg_cnt)++;
        len = 0;
        if (ch == '\n') break;
        else if (read(0, &ch, 1) < 1) break;
    }
    return 1;
}

// 读入命令
void get_cmd(int arg_cnt) {
    char *p = cmd;
    p = strcpy(p, argx[0]);
    p += sizeof(argx[0]);
    for (int i = 1; i < arg_cnt; i++) {
        *p = ' ';
        p++;
        strcpy(p, argx[i]);
        p += sizeof(argx[i]);
    }
    *p = '\0';
}

int main(int argc, char *argv[])
{
    if (argc <= 1) {
        fprintf(2, "usage: xarg cmd...\n");
        exit(1);
    }

    for (int i = 1; i < argc; i++) {
        strcpy(argx[i - 1], argv[i]);
    }

    int new_argc = argc - 1;
    while (read_line(&new_argc)) {
        // 对每行参数，重复执行 xargs 后的命令
        if (fork() == 0) {
            for (int i = 0; i < new_argc; i++) {
                use_argx[i] = argx[i];
            }
            use_argx[new_argc] = 0;
            exec(use_argx[0], use_argx);
            exit(1);
        }
        else {
            wait(0);
        }
        new_argc = argc - 1;
    }
    exit(0);
} 
```

### 测试结果

测试结果为 100/100 分。

<img src="https://i.loli.net/2021/06/19/pYLJOeNfdwgvo2E.png" alt="image-20210619002413045" style="zoom: 80%;" />

### 附加挑战

这个 Lab 的附加挑战难度不高，都是很容易实现的小功能。