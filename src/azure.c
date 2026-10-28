#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#define MAXPATH 4096
#define MAXLINE 8192
#define MAXPKG 4096
#define MAXNAME 256

typedef struct {
    char name[MAXNAME];
    char path[MAXPATH];
    char provides[MAXLINE];
    char depends[MAXLINE];
    char makedepends[MAXLINE];
    char relurl[MAXPATH];
    char checksum[256];
    char source[MAXPATH];
    char sha256sums[MAXLINE];
    int is_bconf;
} Package;

static Package packages[MAXPKG];
static int package_count = 0;

static char db_path[MAXPATH] = "/var/lib/azure";
static char cache_path[MAXPATH] = "/var/cache/azure";
static char base_file[MAXPATH] = "/etc/azure/base-packages";

static char repos[64][MAXPATH];
static int repo_count = 0;

static char install_order[MAXPKG][MAXNAME];
static int install_count = 0;

static int visiting[MAXPKG];
static int visited[MAXPKG];

static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\n' || s[len-1] == '\r')) s[--len] = '\0';
}

static int ends_with(const char *s, const char *suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    return ls >= lf && strcmp(s + ls - lf, suffix) == 0;
}

static void add_repo(const char *path) {
    if (repo_count >= 64) return;
    strncpy(repos[repo_count], path, MAXPATH-1);
    repos[repo_count][MAXPATH-1] = '\0';
    repo_count++;
}

static void load_repos(void) {
    const char *env = getenv("AZURE_REPO");
    if (env && *env) { add_repo(env); return; }
    FILE *f = fopen("/etc/azure/repos.conf", "r");
    if (f) {
        char line[MAXPATH];
        while (fgets(line, sizeof(line), f)) {
            trim(line);
            if (line[0] == '\0' || line[0] == '#') continue;
            add_repo(line);
        }
        fclose(f);
        if (repo_count > 0) return;
    }
    add_repo("/usr/share/azure/repo");
    add_repo("/home/ripie/buildroot/pkgrepo");
    add_repo("/home/ripie/bmcrepo");
}

static int is_base(const char *name) {
    FILE *f = fopen(base_file, "r");
    if (!f) return 0;
    char line[MAXNAME];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (strcmp(line, name) == 0) { fclose(f); return 1; }
    }
    fclose(f);
    return 0;
}

static int find_package(const char *name) {
    for (int i = 0; i < package_count; i++) {
        if (strcmp(packages[i].name, name) == 0) return i;
    }
    return -1;
}

static void parse_bbr(const char *path, Package *p) {
    memset(p, 0, sizeof(*p));
    strncpy(p->path, path, MAXPATH-1);
    p->is_bconf = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[MAXLINE];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0] != '-') continue;
        char field[MAXNAME], value[MAXLINE];
        if (sscanf(line, "-%255s", field) != 1) continue;
        char *colon = strchr(line, ':');
        char *val = colon ? colon + 1 : line + 1 + strlen(field);
        while (*val == ' ' || *val == '\t') val++;
        strncpy(value, val, MAXLINE-1);
        value[MAXLINE-1] = '\0';
        trim(value);
        if (strcmp(field, "name") == 0) strcpy(p->name, value);
        else if (strcmp(field, "provides") == 0) strcpy(p->provides, value);
        else if (strcmp(field, "depends") == 0) strcpy(p->depends, value);
        else if (strcmp(field, "makedepends") == 0) strcpy(p->makedepends, value);
        else if (strcmp(field, "relurl") == 0) strcpy(p->relurl, value);
        else if (strcmp(field, "checksum") == 0) strcpy(p->checksum, value);
        else if (strcmp(field, "upstream") == 0) { /* ignored */ }
    }
    fclose(f);
    if (p->name[0] == '\0') {
        char *base = strrchr(path, '/');
        base = base ? base + 1 : (char*)path;
        strncpy(p->name, base, MAXNAME-1);
        char *dot = strrchr(p->name, '.');
        if (dot) *dot = '\0';
    }
}

static void parse_bconf(const char *path, Package *p) {
    memset(p, 0, sizeof(*p));
    strncpy(p->path, path, MAXPATH-1);
    p->is_bconf = 1;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[MAXLINE];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);
        if (val[0] == '"' && val[strlen(val)-1] == '"') {
            val[strlen(val)-1] = '\0';
            val++;
        }
        if (strcmp(key, "pkgname") == 0) strcpy(p->name, val);
        else if (strcmp(key, "pkgver") == 0) { /* store in provides? */ }
        else if (strcmp(key, "source") == 0) strcpy(p->source, val);
        else if (strcmp(key, "sha256sums") == 0) strcpy(p->sha256sums, val);
        else if (strcmp(key, "depends") == 0) strcpy(p->depends, val);
        else if (strcmp(key, "makedepends") == 0) strcpy(p->makedepends, val);
        else if (strcmp(key, "relurl") == 0) strcpy(p->relurl, val);
    }
    fclose(f);
    if (p->relurl[0] == '\0' && p->source[0] != '\0') {
        /* use first source as relurl for simple cases */
        char tmp[MAXPATH];
        strncpy(tmp, p->source, MAXPATH-1);
        char *sp = strchr(tmp, ' ');
        if (sp) *sp = '\0';
        strcpy(p->relurl, tmp);
    }
    if (p->name[0] == '\0') {
        char *base = strrchr(path, '/');
        base = base ? base + 1 : (char*)path;
        strncpy(p->name, base, MAXNAME-1);
        char *dot = strrchr(p->name, '.');
        if (dot) *dot = '\0';
    }
}

static void walk_repo(const char *root) {
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[MAXPATH];
        snprintf(path, sizeof(path), "%s/%s", root, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            walk_repo(path);
        } else if (ends_with(path, ".bbr") || ends_with(path, ".bconf")) {
            if (package_count >= MAXPKG) break;
            if (ends_with(path, ".bconf")) parse_bconf(path, &packages[package_count]);
            else parse_bbr(path, &packages[package_count]);
            if (packages[package_count].name[0] != '\0') package_count++;
        }
    }
    closedir(d);
}

static void load_packages(void) {
    load_repos();
    for (int i = 0; i < repo_count; i++) walk_repo(repos[i]);
}

static char **split_list(const char *s, int *count) {
    static char buf[MAXLINE];
    static char *items[128];
    *count = 0;
    if (!s || !*s) return items;
    strncpy(buf, s, MAXLINE-1);
    buf[MAXLINE-1] = '\0';
    /* replace commas with spaces */
    for (char *p = buf; *p; p++) if (*p == ',') *p = ' ';
    char *tok = strtok(buf, " \t");
    while (tok && *count < 128) {
        items[(*count)++] = tok;
        tok = strtok(NULL, " \t");
    }
    return items;
}

static void resolve_one(int idx);

static void resolve_deps(const char *depstr) {
    int count;
    char **deps = split_list(depstr, &count);
    for (int i = 0; i < count; i++) {
        if (is_base(deps[i])) continue;
        int idx = find_package(deps[i]);
        if (idx < 0) {
            fprintf(stderr, "azure: missing recipe: %s\n", deps[i]);
            exit(1);
        }
        resolve_one(idx);
    }
}

static void resolve_one(int idx) {
    if (visited[idx]) return;
    if (visiting[idx]) {
        fprintf(stderr, "azure: dependency cycle detected at %s\n", packages[idx].name);
        exit(1);
    }
    visiting[idx] = 1;
    resolve_deps(packages[idx].depends);
    resolve_deps(packages[idx].makedepends);
    visiting[idx] = 0;
    visited[idx] = 1;
    if (install_count < MAXPKG) {
        strcpy(install_order[install_count++], packages[idx].name);
    }
}

static void resolve(const char *name) {
    if (is_base(name)) return;
    int idx = find_package(name);
    if (idx < 0) { fprintf(stderr, "azure: missing recipe: %s\n", name); exit(1); }
    resolve_one(idx);
}

static char *sha256_file(const char *path) {
    static char out[256];
    out[0] = '\0';
    char cmd[MAXPATH + 64];
    snprintf(cmd, sizeof(cmd), "sha256sum '%s' 2>/dev/null", path);
    FILE *p = popen(cmd, "r");
    if (!p) return out;
    if (fscanf(p, "%255s", out) != 1) out[0] = '\0';
    pclose(p);
    return out;
}

static void run(const char *cmd) {
    if (system(cmd) != 0) {
        fprintf(stderr, "azure: command failed: %s\n", cmd);
        exit(1);
    }
}

static void build_package(const char *name) {
    int idx = find_package(name);
    if (idx < 0) { if (!is_base(name)) { fprintf(stderr, "azure: missing recipe: %s\n", name); exit(1);} return; }
    if (is_base(name)) { printf("azure: %s is provided by the base system; skipping build\n", name); return; }

    char hashpath[MAXPATH];
    snprintf(hashpath, sizeof(hashpath), "%s/hashes/%s", db_path, name);
    struct stat st;
    char installed_file[MAXPATH];
    snprintf(installed_file, sizeof(installed_file), "%s/installed", db_path);
    /* check installed */
    FILE *inf = fopen(installed_file, "r");
    int already = 0;
    if (inf) {
        char line[MAXNAME];
        while (fgets(line, sizeof(line), inf)) {
            trim(line);
            if (strcmp(line, name) == 0) { already = 1; break; }
        }
        fclose(inf);
    }
    char curhash[256];
    /* hash recipe */
    char hashcmd[MAXPATH+64];
    snprintf(hashcmd, sizeof(hashcmd), "sha256sum '%s'", packages[idx].path);
    FILE *hp = popen(hashcmd, "r");
    curhash[0]='\0';
    if (hp) { fscanf(hp, "%255s", curhash); pclose(hp); }

    if (already) {
        char oldhash[256] = "";
        FILE *hf = fopen(hashpath, "r");
        if (hf) { fscanf(hf, "%255s", oldhash); fclose(hf); }
        if (strcmp(oldhash, curhash) == 0) {
            printf("azure: %s is already installed\n", name);
            return;
        }
        printf("azure: recipe for %s changed; reinstalling\n", name);
    }

    char srcdir[MAXPATH], pkgdir[MAXPATH], startdir[MAXPATH];
    snprintf(srcdir, sizeof(srcdir), "%s/src/%s", cache_path, name);
    snprintf(pkgdir, sizeof(pkgdir), "%s/stage/%s", cache_path, name);
    snprintf(startdir, sizeof(startdir), "%s/build/%s", cache_path, name);

    char cmd[MAXLINE];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s' '%s' && mkdir -p '%s' '%s' '%s' '%s/downloads' '%s/manifests' '%s/hashes'",
             srcdir, pkgdir, startdir, srcdir, pkgdir, startdir, cache_path, db_path, db_path);
    run(cmd);

    if (packages[idx].is_bconf) {
        /* parse source and sums already in struct */
        /* download first source only for simplicity */
        if (packages[idx].source[0]) {
            /* split by space */
            char srcs[MAXLINE];
            strcpy(srcs, packages[idx].source);
            char *tok = strtok(srcs, " ");
            while (tok) {
                char archive[MAXPATH];
                snprintf(archive, sizeof(archive), "%s/downloads/%s", cache_path, strrchr(tok, '/') ? strrchr(tok, '/') + 1 : tok);
                struct stat as;
                if (stat(archive, &as) != 0) {
                    snprintf(cmd, sizeof(cmd), "wget -O '%s' '%s'", archive, tok);
                    run(cmd);
                }
                snprintf(cmd, sizeof(cmd), "tar -xf '%s' -C '%s'", archive, srcdir);
                run(cmd);
                tok = strtok(NULL, " ");
            }
        }
        /* build */
        setenv("srcdir", srcdir, 1);
        setenv("pkgdir", pkgdir, 1);
        setenv("startdir", startdir, 1);
        snprintf(cmd, sizeof(cmd), "sh -c '. \"%s\"; cd \"$srcdir\"; if command -v build >/dev/null 2>&1; then build; fi; cd \"$srcdir\"; if command -v package >/dev/null 2>&1; then package; fi' _ '%s'",
                 packages[idx].path, packages[idx].path);
        run(cmd);
    } else {
        if (packages[idx].relurl[0]) {
            char archive[MAXPATH];
            snprintf(archive, sizeof(archive), "%s/downloads/%s", cache_path, strrchr(packages[idx].relurl, '/') ? strrchr(packages[idx].relurl, '/') + 1 : packages[idx].relurl);
            struct stat as;
            if (stat(archive, &as) != 0) {
                snprintf(cmd, sizeof(cmd), "wget -O '%s' '%s'", archive, packages[idx].relurl);
                run(cmd);
            }
            if (packages[idx].checksum[0]) {
                char *h = sha256_file(archive);
                char expected[256];
                strcpy(expected, packages[idx].checksum);
                char *colon = strchr(expected, ':');
                if (colon) memmove(expected, colon+1, strlen(colon+1)+1);
                trim(expected);
                if (strcmp(h, expected) != 0) {
                    fprintf(stderr, "azure: checksum mismatch for %s\n", archive);
                    exit(1);
                }
            }
            snprintf(cmd, sizeof(cmd), "tar -xf '%s' -C '%s' --strip-components=1", archive, srcdir);
            run(cmd);
        }
        /* detect build system */
        char meson[MAXPATH], configure[MAXPATH], makefile[MAXPATH];
        snprintf(meson, sizeof(meson), "%s/meson.build", srcdir);
        snprintf(configure, sizeof(configure), "%s/configure", srcdir);
        snprintf(makefile, sizeof(makefile), "%s/Makefile", srcdir);
        struct stat ms, cs, mks;
        if (stat(meson, &ms) == 0) {
            snprintf(cmd, sizeof(cmd), "cd '%s' && meson setup build --prefix=/usr && ninja -C build && DESTDIR='%s' ninja -C build install", srcdir, pkgdir);
            run(cmd);
        } else if (stat(configure, &cs) == 0) {
            snprintf(cmd, sizeof(cmd), "cd '%s' && ./configure --prefix=/usr && make && make install DESTDIR='%s'", srcdir, pkgdir);
            run(cmd);
        } else if (stat(makefile, &mks) == 0) {
            snprintf(cmd, sizeof(cmd), "cd '%s' && make", srcdir);
            run(cmd);
            /* try install */
            snprintf(cmd, sizeof(cmd), "cd '%s' && make -n install >/dev/null 2>&1 && make install DESTDIR='%s' PREFIX=/usr", srcdir, pkgdir);
            system(cmd);
        } else {
            fprintf(stderr, "azure: unsupported build system for %s\n", name);
            exit(1);
        }
    }

    /* manifest */
    char manifest[MAXPATH];
    snprintf(manifest, sizeof(manifest), "%s/manifests/%s.list", db_path, name);
    snprintf(cmd, sizeof(cmd), "(cd '%s' && find . \\( -type f -o -type l \\) -print) > '%s'", pkgdir, manifest);
    run(cmd);

    /* install */
    snprintf(cmd, sizeof(cmd), "(cd '%s' && cp -a ./* / 2>/dev/null || true)", pkgdir);
    run(cmd);

    /* db */
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", db_path);
    run(cmd);
    snprintf(installed_file, sizeof(installed_file), "%s/installed", db_path);
    FILE *af = fopen(installed_file, "a");
    if (af) { fprintf(af, "%s\n", name); fclose(af); }
    FILE *hf = fopen(hashpath, "w");
    if (hf) { fprintf(hf, "%s\n", curhash); fclose(hf); }

    printf("azure: installed %s\n", name);
}

static void cmd_add(const char *name) {
    /* resolve */
    install_count = 0;
    memset(visited, 0, sizeof(visited));
    memset(visiting, 0, sizeof(visiting));
    resolve(name);
    for (int i = 0; i < install_count; i++) {
        printf("[%d/%d] installing %s\n", i+1, install_count, install_order[i]);
        build_package(install_order[i]);
    }
}

static void cmd_del(const char *name) {
    /* reverse deps */
    char installed_file[MAXPATH];
    snprintf(installed_file, sizeof(installed_file), "%s/installed", db_path);
    /* check base */
    if (is_base(name)) { fprintf(stderr, "azure: %s is provided by base system\n", name); exit(1); }
    /* remove files */
    char manifest[MAXPATH];
    snprintf(manifest, sizeof(manifest), "%s/manifests/%s.list", db_path, name);
    FILE *mf = fopen(manifest, "r");
    if (mf) {
        char line[MAXPATH];
        int total = 0, count = 0;
        /* count */
        while (fgets(line, sizeof(line), mf)) total++;
        rewind(mf);
        while (fgets(line, sizeof(line), mf)) {
            trim(line);
            if (line[0] == '\0') continue;
            count++;
            char path[MAXPATH];
            snprintf(path, sizeof(path), "/%s", line[0]=='.'?line+2:line);
            /* remove leading ./ */
            if (strncmp(line, "./", 2) == 0) snprintf(path, sizeof(path), "/%s", line+2);
            else snprintf(path, sizeof(path), "/%s", line);
            printf("[%d/%d] removing %s\n", count, total, path);
            remove(path);
        }
        fclose(mf);
        remove(manifest);
    }
    /* update installed */
    char tmp[MAXPATH];
    snprintf(tmp, sizeof(tmp), "%s/installed.tmp", db_path);
    FILE *in = fopen(installed_file, "r");
    FILE *out = fopen(tmp, "w");
    if (in && out) {
        char line[MAXNAME];
        while (fgets(line, sizeof(line), in)) {
            trim(line);
            if (strcmp(line, name) != 0) fprintf(out, "%s\n", line);
        }
        fclose(in); fclose(out);
        rename(tmp, installed_file);
    }
    printf("azure: removed %s\n", name);
}

static void cmd_sync(void) {
    for (int i = 0; i < repo_count; i++) {
        char gitdir[MAXPATH];
        snprintf(gitdir, sizeof(gitdir), "%s/.git", repos[i]);
        struct stat st;
        if (stat(gitdir, &st) == 0) {
            char cmd[MAXLINE];
            printf("azure: syncing %s\n", repos[i]);
            snprintf(cmd, sizeof(cmd), "git -C '%s' pull --ff-only", repos[i]);
            run(cmd);
        }
    }
    printf("azure: sync complete\n");
}

static void cmd_search(const char *term) {
    int found = 0;
    for (int i = 0; i < package_count; i++) {
        if (strstr(packages[i].name, term)) { printf("%s\n", packages[i].name); found = 1; continue; }
        FILE *f = fopen(packages[i].path, "r");
        if (f) {
            char line[MAXLINE];
            while (fgets(line, sizeof(line), f)) {
                if (strcasestr(line, term)) { printf("%s\n", packages[i].name); found = 1; break; }
            }
            fclose(f);
        }
    }
    if (!found) printf("azure: no package found for %s\n", term);
}

static void cmd_see(const char *arg) {
    char manifest[MAXPATH];
    snprintf(manifest, sizeof(manifest), "%s/manifests/%s.list", db_path, arg);
    FILE *f = fopen(manifest, "r");
    if (f) {
        char line[MAXPATH];
        while (fgets(line, sizeof(line), f)) {
            trim(line);
            if (line[0]) printf("%s\n", line);
        }
        fclose(f);
        return;
    }
    /* path ownership */
    int found = 0;
    char norm[MAXPATH];
    strncpy(norm, arg, MAXPATH-1);
    if (norm[0] == '/') memmove(norm, norm+1, strlen(norm));
    for (int i = 0; i < package_count; i++) {
        char mpath[MAXPATH];
        snprintf(mpath, sizeof(mpath), "%s/manifests/%s.list", db_path, packages[i].name);
        FILE *mf = fopen(mpath, "r");
        if (!mf) continue;
        char line[MAXPATH];
        int hit = 0;
        while (fgets(line, sizeof(line), mf)) {
            trim(line);
            char full[MAXPATH];
            if (line[0] == '.') snprintf(full, sizeof(full), "/%s", line+2);
            else snprintf(full, sizeof(full), "/%s", line);
            if (strcmp(full, arg) == 0 || strncmp(full, arg, strlen(arg)) == 0) { hit = 1; break; }
            /* directory prefix */
            size_t al = strlen(arg);
            if (strncmp(full, arg, al) == 0 && (full[al] == '/' || full[al] == '\0')) { hit = 1; break; }
        }
        fclose(mf);
        if (hit) { printf("%s owns %s\n", packages[i].name, arg); found = 1; }
    }
    if (!found) printf("azure: no package owns %s\n", arg);
}

int main(int argc, char **argv) {
    /* background mode */
    int background = 0;
    char filtered[4096] = "";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) background = 1;
        else {
            if (filtered[0]) strcat(filtered, " ");
            strcat(filtered, argv[i]);
        }
    }
    if (background && argc > 1) {
        /* rebuild argv */
        /* simple: exec via system */
        char cmd[MAXLINE];
        snprintf(cmd, sizeof(cmd), "%s %s >%s/azure-bg.log 2>&1 &", argv[0], filtered, db_path);
        system(cmd);
        printf("azure: started in background; log: %s/azure-bg.log\n", db_path);
        return 0;
    }

    load_packages();
    if (argc < 2) {
        fprintf(stderr, "Usage: azure add <package>\n       azure del <package>\n       azure sync\n       azure upgrade\n       azure upd\n       azure search <term>\n       azure see <path|package>\n");
        return 2;
    }
    if (strcmp(argv[1], "add") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: azure add <package>\n"); return 2; }
        cmd_add(argv[2]);
    } else if (strcmp(argv[1], "del") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: azure del <package>\n"); return 2; }
        cmd_del(argv[2]);
    } else if (strcmp(argv[1], "sync") == 0) {
        cmd_sync();
    } else if (strcmp(argv[1], "upgrade") == 0) {
        /* upgrade all installed */
        char installed_file[MAXPATH];
        snprintf(installed_file, sizeof(installed_file), "%s/installed", db_path);
        FILE *f = fopen(installed_file, "r");
        if (!f) { printf("azure: no installed packages\n"); return 0; }
        char pkgs[4096] = "";
        char line[MAXNAME];
        while (fgets(line, sizeof(line), f)) {
            trim(line);
            if (line[0]) { if (pkgs[0]) strcat(pkgs, " "); strcat(pkgs, line); }
        }
        fclose(f);
        /* reset installed */
        fopen(installed_file, "w");
        char *tok = strtok(pkgs, " ");
        int total = 0, count = 0;
        /* count */
        char tmp[4096]; strcpy(tmp, pkgs);
        char *t = strtok(tmp, " ");
        while (t) { total++; t = strtok(NULL, " "); }
        tok = strtok(pkgs, " ");
        while (tok) {
            count++;
            printf("[%d/%d] upgrading %s\n", count, total, tok);
            build_package(tok);
            tok = strtok(NULL, " ");
        }
        printf("azure: upgrade complete\n");
    } else if (strcmp(argv[1], "upd") == 0) {
        cmd_sync();
        /* upgrade inline */
        main(2, (char*[]){"azure","upgrade"});
    } else if (strcmp(argv[1], "search") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: azure search <term>\n"); return 2; }
        cmd_search(argv[2]);
    } else if (strcmp(argv[1], "see") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: azure see <path|package>\n"); return 2; }
        cmd_see(argv[2]);
    } else {
        fprintf(stderr, "azure: unknown command %s\n", argv[1]);
        return 2;
    }
    return 0;
}
