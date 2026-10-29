#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    const char *disk = argc > 1 ? argv[1] : NULL;
    if (!disk) {
        fprintf(stderr, "usage: inst <disk>\n");
        return 2;
    }
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "printf 'o\\nn\\np\\n1\\n\\n+512M\\na\\nn\\np\\n2\\n\\n\\nw\\n' | fdisk %s", disk);
    if (system(cmd) != 0) return 1;
    snprintf(cmd, sizeof(cmd), "mkfs.vfat -F 32 %s1", disk);
    if (system(cmd) != 0) return 1;
    snprintf(cmd, sizeof(cmd), "mkfs.ext4 -F %s2", disk);
    if (system(cmd) != 0) return 1;
    printf("inst: formatted %s\n", disk);
    return 0;
}
