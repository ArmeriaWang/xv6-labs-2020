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

int read_line(int *arg_cnt) {
    char ch;
    int len = 0, ok = read(0, &ch, 1);
    if (ok < 1) return 0;
    // fprintf(2, "D :: ch = %d\n", ch);
    while (ch != '\n' && *arg_cnt < MAXARG - 1) {
        while (ch != ' ' && ch != '\n' && len < MAXARGLEN - 1) {
            argx[*arg_cnt][len++] = ch;
            if (read(0, &ch, 1) < 1) break;
        }
        argx[*arg_cnt][len] = '\0';
        // fprintf(2, "arg[%d] = %s  len = %d\n", *arg_cnt, argx[*arg_cnt], len);
        if (len > 0) (*arg_cnt)++;
        len = 0;
        if (ch == '\n') break;
        else if (read(0, &ch, 1) < 1) break;
        // fprintf(2, "arg_cnt = %d\n", *arg_cnt);
    }
    return 1;
}

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
    // fprintf(2, "A\n");
    while (read_line(&new_argc)) {
        // get_cmd(new_argc);
        // fprintf(2, "B :: new_argc = %d  ", new_argc, cmd);
        // for (int i = 0; i < new_argc; i++) fprintf(2, "%s ", argx[i]);
        // fprintf(2, "\n");
        if (fork() == 0) {
            // prepare_exec(argx);
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