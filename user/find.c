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

    // fprintf(2, "pd_name = %s  st.type = %d\n", pre_de_name, st.type);
    if (st.type == T_FILE || st.type == T_DEVICE) {
        if (!flag) {
            fprintf(2, "find: %s is not a directory\n", path);
            close(fd);
            return;
        }
        if (strcmp(pre_de_name, fname) == 0) {
            // fprintf(2, "path = %s  de.name = %s\n", path, de.name);
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

    // fprintf(2, "pd_name = %s\n", pre_de_name);

    strcpy(buf, path);
    p = buf + strlen(buf);
    *(p++) = '/';
    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
        if (de.inum == 0) {
            continue;
        }
        // printf("de.name = %s\n", de.name);
        memmove(p, de.name, DIRSIZ);
        p[DIRSIZ] = 0;
        if (stat(buf, &st) < 0) {
            printf("find: cannot stat %s\n", buf);
            continue;
        }
        // printf("buf = %s  de.name = %s\n", buf, de.name);
        if (strcmp(de.name, ".") != 0 && strcmp(de.name, "..") != 0) {
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