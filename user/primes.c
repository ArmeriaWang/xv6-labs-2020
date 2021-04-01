#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

/**
 * 很怪，
 * 一开始我是全用了重定向，但导致程序无法正常结束（可以正常输出结果）
 * 把重定向全部删除后（共 2 * 3 = 6 行），可以正常结束并通过测试
 * 只留下 sieve 中最上面的重定向，也可以通过测试
 * 但其余组合均不能通过，不明白是为什么
**/
void sieve(int p[]) {
    int num, x;
    close(0);
    dup(p[0]);
    close(p[1]);

    read(p[0], &x, sizeof(x));
    fprintf(2, "prime %d\n", x);
    int len = read(p[0], &num, sizeof(num));
    // fprintf(2, "A :: len = %d, x = %d, num = %d\n", len, x, num);
    if (len != sizeof(num)) {
        close(p[0]);
        exit(0);
    }

    // fprintf(2, "B :: x = %d\n", x);
    int newp[2];
    pipe(newp);
    if (fork() == 0) {
        sieve(newp);
    }
    else {
        // close(1);
        // dup(newp[1]);
        close(newp[0]);

        do {
            if (num % x != 0) {
                write(newp[1], &num, sizeof(num));
                // fprintf(2, "C :: x=%d  num=%d\n", x, num);
            }
        } while (read(p[0], &num, sizeof(num)) == sizeof(num));
        // fprintf(2, "D :: %d closed!\n", x);

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
        sieve(p);
    }
    else {
        // close(1);
        // dup(p[1]);
        close(p[0]);

        for (int x = 2; x <= 35; x++) {
            write(p[1], &x, sizeof(x));
        }

        close(p[1]);
        // fprintf(2, "M :: root closed!\n");
        wait(0);
        exit(0);
    }
    return -1;
}
