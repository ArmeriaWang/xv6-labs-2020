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
