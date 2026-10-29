#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int run(const char *cmd) {
    printf("inst: %s\n", cmd);
    fflush(stdout);
    return system(cmd);
}

static int is_block(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISBLK(st.st_mode);
}

int main(void) {
    char rootpart[128] = "";
    char bootpart[128] = "";
    char confirm[16] = "";
    char mnt[] = "/mnt/rinix-target";
    char cmd[4096];

    const char *env_root = getenv("INST_ROOT");
    const char *env_boot = getenv("INST_BOOT");
    const char *env_confirm = getenv("INST_CONFIRM");
    const char *env_rootfs = getenv("BMC_ROOTFS");

    if (env_root) strncpy(rootpart, env_root, sizeof(rootpart)-1);
    if (env_boot) strncpy(bootpart, env_boot, sizeof(bootpart)-1);
    if (env_confirm) strncpy(confirm, env_confirm, sizeof(confirm)-1);

    printf("Rinix installer\n");
    printf("Partition the disk yourself, then tell inst which partitions to use.\n");

    if (rootpart[0] == '\0') {
        printf("Root partition (e.g. /dev/sda2): ");
        if (!fgets(rootpart, sizeof(rootpart), stdin)) return 1;
        rootpart[strcspn(rootpart, "\n")] = '\0';
    }
    if (bootpart[0] == '\0') {
        printf("Boot/EFI partition (e.g. /dev/sda1): ");
        if (!fgets(bootpart, sizeof(bootpart), stdin)) return 1;
        bootpart[strcspn(bootpart, "\n")] = '\0';
    }

    if (!is_block(rootpart)) {
        fprintf(stderr, "inst: not a block device: %s\n", rootpart);
        return 1;
    }
    if (!is_block(bootpart)) {
        fprintf(stderr, "inst: not a block device: %s\n", bootpart);
        return 1;
    }

    if (confirm[0] == '\0') {
        printf("WARNING: %s and %s will be formatted.\n", rootpart, bootpart);
        printf("Type YES to continue: ");
        if (!fgets(confirm, sizeof(confirm), stdin)) return 1;
        confirm[strcspn(confirm, "\n")] = '\0';
    }
    if (strcmp(confirm, "YES") != 0) {
        fprintf(stderr, "inst: aborted\n");
        return 1;
    }

    snprintf(cmd, sizeof(cmd), "mkfs.vfat -F 32 %s", bootpart);
    if (run(cmd) != 0) { fprintf(stderr, "inst: failed to format boot partition\n"); return 1; }
    snprintf(cmd, sizeof(cmd), "mkfs.ext4 -F %s", rootpart);
    if (run(cmd) != 0) { fprintf(stderr, "inst: failed to format root partition\n"); return 1; }

    snprintf(cmd, sizeof(cmd), "mkdir -p %s", mnt);
    run(cmd);
    snprintf(cmd, sizeof(cmd), "mount %s %s", rootpart, mnt);
    if (run(cmd) != 0) { fprintf(stderr, "inst: failed to mount root partition\n"); return 1; }
    snprintf(cmd, sizeof(cmd), "mkdir -p %s/boot/efi", mnt);
    run(cmd);
    snprintf(cmd, sizeof(cmd), "mount %s %s/boot/efi", bootpart, mnt);
    if (run(cmd) != 0) { fprintf(stderr, "inst: failed to mount boot/EFI partition\n"); return 1; }

    const char *rootfs = env_rootfs ? env_rootfs : "/media/rinix/rootfs.tar.gz";
    if (access(rootfs, F_OK) == 0) {
        snprintf(cmd, sizeof(cmd), "tar -xzf %s -C %s", rootfs, mnt);
        if (run(cmd) != 0) { fprintf(stderr, "inst: failed to extract root filesystem\n"); return 1; }
    } else {
        fprintf(stderr, "inst: root filesystem archive not found at %s; root skeleton left empty\n", rootfs);
    }

    run("sync");
    snprintf(cmd, sizeof(cmd), "umount %s/boot/efi", mnt);
    run(cmd);
    snprintf(cmd, sizeof(cmd), "umount %s", mnt);
    run(cmd);

    printf("inst: installation completed for %s and %s\n", rootpart, bootpart);
    return 0;
}
