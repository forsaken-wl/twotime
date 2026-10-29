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

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int main(void) {
    const char *env_disk = getenv("INST_DISK");
    const char *env_boot = getenv("INST_BOOTLOADER");
    const char *env_confirm = getenv("INST_CONFIRM");
    char diskname[64] = "";
    if (env_disk) strncpy(diskname, env_disk, sizeof(diskname)-1);
    char bootloader[32] = "";
    if (env_boot) strncpy(bootloader, env_boot, sizeof(bootloader)-1);
    char confirm[16] = "";
    if (env_confirm) strncpy(confirm, env_confirm, sizeof(confirm)-1);
    char disk[128];
    char bootpart[160], rootpart[160];
    char mnt[] = "/mnt/rinix-target";
    char cmd[4096];

    printf("Rinix installer\n");
    printf("Available disks:\n");
    system("for d in /sys/block/*; do n=${d##*/}; case \"$n\" in loop*|ram*|sr*) continue;; esac; printf '  %s\\n' \"$n\"; done");

    if (diskname[0] == '\0') {
        printf("Select target disk (e.g. sda, vda): ");
        if (!fgets(diskname, sizeof(diskname), stdin)) return 1;
        diskname[strcspn(diskname, "\n")] = '\0';
    }
    if (diskname[0] == '\0') { fprintf(stderr, "inst: empty disk selection\n"); return 1; }
    snprintf(disk, sizeof(disk), "/dev/%s", diskname);
    struct stat st;
    if (stat(disk, &st) != 0 || !S_ISBLK(st.st_mode)) {
        fprintf(stderr, "inst: not a block device: %s\n", disk);
        return 1;
    }

    printf("WARNING: all data on %s will be destroyed.\n", disk);
    if (confirm[0] == '\0') {
        printf("Type YES to continue: ");
        if (!fgets(confirm, sizeof(confirm), stdin)) return 1;
        confirm[strcspn(confirm, "\n")] = '\0';
    }
    if (strcmp(confirm, "YES") != 0) {
        fprintf(stderr, "inst: aborted\n");
        return 1;
    }

    if (bootloader[0] == '\0') {
        printf("Bootloader (grub or syslinux): ");
        if (!fgets(bootloader, sizeof(bootloader), stdin)) return 1;
        bootloader[strcspn(bootloader, "\n")] = '\0';
    }
    if (strcmp(bootloader, "grub") != 0 && strcmp(bootloader, "syslinux") != 0) {
        fprintf(stderr, "inst: unsupported bootloader: %s\n", bootloader);
        return 1;
    }

    snprintf(cmd, sizeof(cmd), "printf 'o\\nn\\np\\n1\\n\\n+512M\\na\\nn\\np\\n2\\n\\n\\nw\\n' | fdisk %s", disk);
    if (run(cmd) != 0) { fprintf(stderr, "inst: partitioning failed\n"); return 1; }

    /* Allow kernel to reread partition table. */
    run("sync");
    snprintf(cmd, sizeof(cmd), "blockdev --rereadpt %s 2>/dev/null || partprobe %s 2>/dev/null || true", disk, disk);
    run(cmd);

    snprintf(bootpart, sizeof(bootpart), "%s1", disk);
    snprintf(rootpart, sizeof(rootpart), "%s2", disk);
    if (!exists(bootpart)) { snprintf(bootpart, sizeof(bootpart), "%sp1", disk); }
    if (!exists(rootpart)) { snprintf(rootpart, sizeof(rootpart), "%sp2", disk); }
    if (!exists(bootpart) || !exists(rootpart)) {
        fprintf(stderr, "inst: partitions were not created\n");
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
    snprintf(cmd, sizeof(cmd), "mkdir -p %s/boot", mnt);
    run(cmd);
    snprintf(cmd, sizeof(cmd), "mount %s %s/boot", bootpart, mnt);
    if (run(cmd) != 0) { fprintf(stderr, "inst: failed to mount boot partition\n"); return 1; }

    const char *rootfs = getenv("BMC_ROOTFS");
    if (!rootfs) rootfs = "/media/rinix/rootfs.tar.gz";
    if (access(rootfs, F_OK) == 0) {
        snprintf(cmd, sizeof(cmd), "tar -xzf %s -C %s", rootfs, mnt);
        if (run(cmd) != 0) { fprintf(stderr, "inst: failed to extract root filesystem\n"); return 1; }
    } else {
        fprintf(stderr, "inst: root filesystem archive not found at %s; root skeleton left empty\n", rootfs);
    }

    snprintf(cmd, sizeof(cmd), "printf '%s\\n' > %s/etc/rinix-bootloader", bootloader, mnt);
    run(cmd);

    run("sync");
    snprintf(cmd, sizeof(cmd), "umount %s/boot", mnt);
    run(cmd);
    snprintf(cmd, sizeof(cmd), "umount %s", mnt);
    run(cmd);

    printf("inst: installation completed for %s\n", disk);
    return 0;
}
