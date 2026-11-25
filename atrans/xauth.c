// SPDX-License-Identifier: MIT
/*
 * xauth - the part of X11's xauth that SSH X11 forwarding needs.
 *
 * Dropbear (like OpenSSH's sshd) runs "xauth -q" for a login with X11
 * forwarding and writes "add unix:10.0 MIT-MAGIC-COOKIE-1 <hex>" to it. The
 * cookie goes into the user's authority file, where X clients find it and
 * send it: ssh only passes on connections that carry it.
 *
 *     xauth [-f FILE] [-q] [COMMAND ARGS...]
 *
 * With no command, commands are read from standard input, one per line.
 *
 *     add DISPLAY PROTOCOL HEXKEY   add or replace the entry for DISPLAY
 *     remove DISPLAY                remove the entries for DISPLAY
 *     list [DISPLAY]                show the entries (all, or DISPLAY's)
 *
 * DISPLAY is HOST:N[.S]. unix:N, :N, localhost:N and this machine's own name
 * are stored as Xlib looks them up: family Local, with the host name as the
 * address. A dotted IPv4 address is family Internet. The file is $XAUTHORITY,
 * else ~/.Xauthority. Changes are made under a lock (flock on FILE-l, which
 * stays), so logins at the same moment don't lose each other's cookies, and
 * the file is replaced in one rename. A file that can't be read, or ends in
 * the middle of an entry, is left alone: commands that would change it fail.
 * -q is accepted, as dropbear passes it; this xauth prints nothing anyway.
 *
 * Built into the image by build-extras.sh; on the board: cc -o xauth xauth.c
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define FAMILY_INTERNET 0
#define FAMILY_LOCAL 256
#define FAMILY_WILD 65535

struct field {
    unsigned short len;
    unsigned char *s;
};

struct entry {
    unsigned short family;
    struct field addr, number, name, data;
};

static struct entry *entries;
static int nentries, changed, unreadable;
static const char *file;

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);

    if (p == NULL) {
        fprintf(stderr, "xauth: out of memory\n");
        exit(1);
    }
    return p;
}

static void set_field(struct field *f, const void *s, size_t len)
{
    f->len = (unsigned short)len;
    f->s = xmalloc(len);
    memcpy(f->s, s, len);
}

static int read_field(FILE *fp, struct field *f)
{
    int hi = getc(fp), lo = getc(fp);

    if (hi == EOF || lo == EOF)
        return -1;
    f->len = (unsigned short)(hi << 8 | lo);
    f->s = xmalloc(f->len);
    return fread(f->s, 1, f->len, fp) == f->len ? 0 : -1;
}

static void write_field(FILE *fp, const struct field *f)
{
    putc(f->len >> 8, fp);
    putc(f->len & 255, fp);
    fwrite(f->s, 1, f->len, fp);
}

static void free_entry(struct entry *e)
{
    free(e->addr.s);
    free(e->number.s);
    free(e->name.s);
    free(e->data.s);
}

static void append_entry(const struct entry *e)
{
    struct entry *p = realloc(entries, (size_t)(nentries + 1) * sizeof *entries);

    if (p == NULL) {
        fprintf(stderr, "xauth: out of memory\n");
        exit(1);
    }
    entries = p;
    entries[nentries++] = *e;
}

/* Read FILE into entries. A missing file is empty; one that can't be read
 * or is cut short sets unreadable, so it isn't written over. */
static void load(void)
{
    FILE *fp = fopen(file, "rb");
    struct entry e;
    int hi, lo;

    if (fp == NULL) {
        if (errno != ENOENT) {
            fprintf(stderr, "xauth: %s: %s\n", file, strerror(errno));
            unreadable = 1;
        }
        return;
    }
    while ((hi = getc(fp)) != EOF && (lo = getc(fp)) != EOF) {
        memset(&e, 0, sizeof e);
        e.family = (unsigned short)(hi << 8 | lo);
        if (read_field(fp, &e.addr) < 0 || read_field(fp, &e.number) < 0 ||
            read_field(fp, &e.name) < 0 || read_field(fp, &e.data) < 0) {
            free_entry(&e);
            fprintf(stderr, "xauth: %s: ends in the middle of an entry\n", file);
            unreadable = 1;
            break;
        }
        append_entry(&e);
    }
    if (ferror(fp)) {
        fprintf(stderr, "xauth: %s: read error\n", file);
        unreadable = 1;
    }
    fclose(fp);
}

/* FILE with suffix appended (malloc'd). */
static char *file_plus(const char *suffix)
{
    size_t n = strlen(file), m = strlen(suffix);
    char *s = xmalloc(n + m + 1);

    memcpy(s, file, n);
    memcpy(s + n, suffix, m + 1);
    return s;
}

/* Write FILE.new (mode 600, a new file: never through a link left there)
 * and rename it over FILE. Called with the lock held. */
static int save(void)
{
    char *tmp = file_plus(".new");
    FILE *fp;
    int fd;

    unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0 || (fp = fdopen(fd, "wb")) == NULL) {
        fprintf(stderr, "xauth: %s: %s\n", tmp, strerror(errno));
        if (fd >= 0)
            close(fd);
        free(tmp);
        return -1;
    }
    for (int i = 0; i < nentries; i++) {
        struct entry *e = &entries[i];

        putc(e->family >> 8, fp);
        putc(e->family & 255, fp);
        write_field(fp, &e->addr);
        write_field(fp, &e->number);
        write_field(fp, &e->name);
        write_field(fp, &e->data);
    }
    if (fflush(fp) != 0 || fsync(fileno(fp)) != 0 || fclose(fp) != 0 || rename(tmp, file) < 0) {
        fprintf(stderr, "xauth: %s: %s\n", file, strerror(errno));
        unlink(tmp);
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

/* Split DISPLAY into the family/address/number an entry for it has.
 * Returns -1 if it isn't HOST:N[.S]. */
static int parse_display(const char *d, unsigned short *family, struct field *addr, struct field *number)
{
    const char *colon = strrchr(d, ':'), *num, *end;
    char host[256];
    size_t hl;
    struct in_addr in;

    if (colon == NULL || (hl = (size_t)(colon - d)) >= sizeof host)
        return -1;
    memcpy(host, d, hl);
    host[hl] = '\0';
    num = colon + 1;
    for (end = num; *end >= '0' && *end <= '9'; end++)
        ;
    if (end == num || (*end != '\0' && *end != '.'))
        return -1;
    set_field(number, num, (size_t)(end - num));

    char self[256] = "";

    gethostname(self, sizeof self - 1);
    if (hl == 0 || strcmp(host, "unix") == 0 || strcmp(host, "localhost") == 0 ||
        strcmp(host, self) == 0 || d[0] == '/') {
        *family = FAMILY_LOCAL;
        set_field(addr, self, strlen(self));
    } else if (inet_pton(AF_INET, host, &in) == 1) {
        *family = FAMILY_INTERNET;
        set_field(addr, &in, 4);
    } else {
        fprintf(stderr, "xauth: %s: only unix, localhost, this host's name or an IPv4 address\n", d);
        free(number->s);
        return -1;
    }
    return 0;
}

static int same_display(const struct entry *e, unsigned short family, const struct field *addr,
                        const struct field *number)
{
    return e->family == family && e->addr.len == addr->len &&
           memcmp(e->addr.s, addr->s, addr->len) == 0 && e->number.len == number->len &&
           memcmp(e->number.s, number->s, number->len) == 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int cmd_add(char **argv, int argc)
{
    struct entry e;
    size_t n;

    if (argc != 4) {
        fprintf(stderr, "xauth: usage: add DISPLAY PROTOCOL HEXKEY\n");
        return -1;
    }
    memset(&e, 0, sizeof e);
    if (parse_display(argv[1], &e.family, &e.addr, &e.number) < 0) {
        fprintf(stderr, "xauth: bad display name \"%s\"\n", argv[1]);
        return -1;
    }
    n = strlen(argv[3]);
    if (n == 0 || n % 2 != 0 || n / 2 > 65535) {
        fprintf(stderr, "xauth: the key must be an even number of hex digits\n");
        free_entry(&e);
        return -1;
    }
    e.data.len = (unsigned short)(n / 2);
    e.data.s = xmalloc(n / 2);
    for (size_t i = 0; i < n / 2; i++) {
        int h = hexval(argv[3][2 * i]), l = hexval(argv[3][2 * i + 1]);

        if (h < 0 || l < 0) {
            fprintf(stderr, "xauth: the key must be an even number of hex digits\n");
            free_entry(&e);
            return -1;
        }
        e.data.s[i] = (unsigned char)(h << 4 | l);
    }
    set_field(&e.name, argv[2], strlen(argv[2]));
    for (int i = 0; i < nentries; i++)
        if (same_display(&entries[i], e.family, &e.addr, &e.number) &&
            entries[i].name.len == e.name.len && memcmp(entries[i].name.s, e.name.s, e.name.len) == 0) {
            free_entry(&entries[i]);
            entries[i] = e;
            changed = 1;
            return 0;
        }
    append_entry(&e);
    changed = 1;
    return 0;
}

/* remove DISPLAY, or list [DISPLAY] */
static int cmd_select(char **argv, int argc, int remove)
{
    unsigned short family = 0;
    struct field addr = { 0, NULL }, number = { 0, NULL };
    int all = argc == 1 && !remove, kept = 0, removed = 0;

    if (argc != 2 && !all) {
        fprintf(stderr, "xauth: usage: %s\n", remove ? "remove DISPLAY" : "list [DISPLAY]");
        return -1;
    }
    if (!all && parse_display(argv[1], &family, &addr, &number) < 0) {
        fprintf(stderr, "xauth: bad display name \"%s\"\n", argv[1]);
        return -1;
    }
    for (int i = 0; i < nentries; i++) {
        struct entry *e = &entries[i];

        if (!all && !same_display(e, family, &addr, &number)) {
            entries[kept++] = *e;
            continue;
        }
        if (remove) {
            free_entry(e);
            removed++;
            continue;
        }
        if (e->family == FAMILY_LOCAL)
            printf("%.*s/unix", e->addr.len, (char *)e->addr.s);
        else if (e->family == FAMILY_INTERNET && e->addr.len == 4)
            printf("%u.%u.%u.%u", e->addr.s[0], e->addr.s[1], e->addr.s[2], e->addr.s[3]);
        else if (e->family == FAMILY_WILD)
            printf("#ffff#");
        else
            printf("#%04x#", e->family);
        printf(":%.*s  %.*s  ", e->number.len, (char *)e->number.s, e->name.len, (char *)e->name.s);
        for (int j = 0; j < e->data.len; j++)
            printf("%02x", e->data.s[j]);
        putchar('\n');
        entries[kept++] = *e;
    }
    nentries = kept;
    if (removed)
        changed = 1;
    free(addr.s);
    free(number.s);
    return 0;
}

static int run(char **argv, int argc)
{
    if (argc == 0)
        return 0;
    if (strcmp(argv[0], "add") == 0)
        return cmd_add(argv, argc);
    if (strcmp(argv[0], "remove") == 0)
        return cmd_select(argv, argc, 1);
    if (strcmp(argv[0], "list") == 0)
        return cmd_select(argv, argc, 0);
    if (strcmp(argv[0], "quit") == 0 || strcmp(argv[0], "exit") == 0)
        return 0;
    fprintf(stderr, "xauth: unknown command \"%s\" (add, remove or list)\n", argv[0]);
    return -1;
}

int main(int argc, char **argv)
{
    int i = 1, status = 0, lock;
    char *path = NULL, *lockname;

    for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        if (strcmp(argv[i], "-q") == 0)
            ;
        else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
            file = argv[++i];
        else {
            fprintf(stderr, "usage: xauth [-f FILE] [-q] [add DISPLAY PROTOCOL HEXKEY | remove DISPLAY | list [DISPLAY]]\n");
            return 2;
        }
    }
    if (file == NULL)
        file = getenv("XAUTHORITY");
    if (file == NULL || *file == '\0') {
        const char *home = getenv("HOME");

        if (home == NULL || *home == '\0') {
            fprintf(stderr, "xauth: set HOME or XAUTHORITY\n");
            return 1;
        }
        path = xmalloc(strlen(home) + sizeof "/.Xauthority");
        strcpy(path, home);
        strcat(path, "/.Xauthority");
        file = path;
    }
    /* held until exit; if the lock file can't be made, neither can FILE.new */
    lockname = file_plus("-l");
    lock = open(lockname, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (lock >= 0)
        flock(lock, LOCK_EX);
    free(lockname);
    load();

    if (i < argc) {
        status = run(argv + i, argc - i) < 0;
    } else {
        char line[1024], *words[8];

        while (fgets(line, sizeof line, stdin) != NULL) {
            int n = 0, c;
            char *w;

            if (strchr(line, '\n') == NULL && !feof(stdin)) {
                fprintf(stderr, "xauth: line too long; skipped\n");
                while ((c = getchar()) != EOF && c != '\n')
                    ;
                status = 1;
                continue;
            }
            for (w = strtok(line, " \t\r\n"); w != NULL && n < 8; w = strtok(NULL, " \t\r\n"))
                words[n++] = w;
            if (w != NULL) {
                fprintf(stderr, "xauth: too many words in \"%s ...\"\n", words[0]);
                status = 1;
            } else if (n > 0 && words[0][0] != '#' && run(words, n) < 0) {
                status = 1;
            }
        }
    }
    if (changed && unreadable) {
        fprintf(stderr, "xauth: not changing %s, which couldn't be read in full\n", file);
        status = 1;
    } else if (changed && save() < 0) {
        status = 1;
    }
    free(path);
    return status;
}
