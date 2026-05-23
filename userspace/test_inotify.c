#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>

#define DIR_PATH  "/tmp/inotify_test"
#define FILE_PATH "/tmp/inotify_test/newfile"

int main(void)
{
    mkdir(DIR_PATH, 0755);
    unlink(FILE_PATH);

    int fd = inotify_init1(0);
    if (fd < 0) {
        puts("FAIL: inotify_init1");
        return 1;
    }

    int wd = inotify_add_watch(fd, DIR_PATH, IN_CREATE | IN_DELETE);
    if (wd < 0) {
        puts("FAIL: inotify_add_watch");
        return 1;
    }

    /* Trigger IN_CREATE */
    int f = open(FILE_PATH, O_CREAT | O_WRONLY, 0644);
    if (f < 0) {
        puts("FAIL: create-file");
        return 1;
    }
    close(f);

    /* Read the event */
    char buf[sizeof(struct inotify_event) + 256];
    int n = read(fd, buf, sizeof(buf));
    if (n < (int)sizeof(struct inotify_event)) {
        puts("FAIL: read-event");
        return 1;
    }
    struct inotify_event *ev = (struct inotify_event *)buf;
    if (!(ev->mask & IN_CREATE)) {
        printf("FAIL: unexpected-mask=0x%x\n", ev->mask);
        return 1;
    }

    /* Trigger IN_DELETE */
    unlink(FILE_PATH);

    n = read(fd, buf, sizeof(buf));
    if (n < (int)sizeof(struct inotify_event)) {
        puts("FAIL: read-delete-event");
        return 1;
    }
    ev = (struct inotify_event *)buf;
    if (!(ev->mask & IN_DELETE)) {
        printf("FAIL: delete-mask=0x%x\n", ev->mask);
        return 1;
    }

    close(fd);
    rmdir(DIR_PATH);
    puts("OK");
    return 0;
}
