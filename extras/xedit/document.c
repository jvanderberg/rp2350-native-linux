// SPDX-License-Identifier: MIT
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _FILE_OFFSET_BITS 64

#include "document.h"

#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct node { struct root left, right; };
struct page {
    off_t start;
    size_t len;
    unsigned char bytes[DOC_PAGE];
};

#define NODE_CACHE 256
#define NODE_BATCH 128

static char *path;
static struct stat expected;
static int original = -1, additions = -1, index_fd = -1;
static int exists;
static off_t added, indexed;
static struct page pages[2];
static struct {
    off_t id;
    int valid;
    struct node node;
} cache[NODE_CACHE];
static struct node pending[NODE_BATCH];
static size_t pending_count;
static int edit_error;

static int transfer(int fd, off_t pos, void *buf, size_t len, int writing)
{
    unsigned char *p = buf;

    while (len) {
        ssize_t n = writing ? pwrite(fd, p, len, pos) : pread(fd, p, len, pos);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (n == 0)
                errno = EIO;
            return -1;
        }
        pos += n;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int temp_file(const char *target, char **name)
{
    const char *slash = strrchr(target, '/');
    const char suffix[] = ".xedit.XXXXXX";
    size_t dir = slash ? (size_t)(slash - target) + 1 : 0;
    int fd;

    *name = malloc(dir + sizeof suffix);
    if (!*name)
        return -1;
    memcpy(*name, target, dir);
    memcpy(*name + dir, suffix, sizeof suffix);
    fd = mkstemp(*name);
    if (fd < 0) {
        int error = errno;

        free(*name);
        *name = NULL;
        errno = error;
    }
    return fd;
}

/* Scratch files go beside the file, so a big edit lands on its disk. An
 * untitled document, or one in a folder we can't write to, uses $TMPDIR or
 * /tmp. */
static int scratch_file(void)
{
    char *name, base[PATH_MAX];
    const char *tmp = getenv("TMPDIR");
    int fd = -1;

    if (path)
        fd = temp_file(path, &name);
    if (fd < 0 && (!path || errno == EACCES || errno == EROFS || errno == EPERM)) {
        snprintf(base, sizeof base, "%s/x", tmp && *tmp ? tmp : "/tmp");
        fd = temp_file(base, &name);
    }
    if (fd < 0)
        return -1;
    if (unlink(name) < 0) {
        int error = errno;

        close(fd);
        free(name);
        errno = error;
        return -1;
    }
    free(name);
    return fd;
}

/* Resolve existing links, but reject dangling links and special files. */
static char *resolve(const char *name, struct stat *st, int *present)
{
    char *target;

    if (!*name) {
        errno = ENOENT;
        return NULL;
    }
    target = realpath(name, NULL);
    if (!target) {
        if (errno != ENOENT)
            return NULL;
        if (lstat(name, st) == 0) {
            errno = ENOENT;
            return NULL;
        }
        if (errno != ENOENT)
            return NULL;
        *present = 0;
        return strdup(name);
    }
    if (stat(target, st) == 0) {
        if (S_ISREG(st->st_mode)) {
            *present = 1;
            return target;
        }
        errno = EINVAL;
    }
    int error = errno;
    free(target);
    errno = error;
    return NULL;
}

int doc_open(const char *name, struct root *root)
{
    struct stat st = {0};
    int present, fd = -1;
    char *target = resolve(name, &st, &present);

    if (!target)
        return -1;
    if (present) {
        fd = open(target, O_RDONLY | O_NONBLOCK);
        if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
            int error = fd >= 0 && !S_ISREG(st.st_mode) ? EINVAL : errno;

            if (fd >= 0)
                close(fd);
            free(target);
            errno = error;
            return -1;
        }
    }
    doc_close();
    path = target;
    expected = st;
    original = fd;
    exists = present;
    *root = (struct root){.size = st.st_size, .height = st.st_size != 0};
    return 0;
}

int doc_new(struct root *root)
{
    doc_close();
    memset(&expected, 0, sizeof expected);
    *root = (struct root){0};
    return 0;
}

void doc_close(void)
{
    if (original >= 0)
        close(original);
    if (additions >= 0)
        close(additions);
    if (index_fd >= 0)
        close(index_fd);
    free(path);
    path = NULL;
    original = additions = index_fd = -1;
    exists = 0;
    added = indexed = 0;
    memset(pages, 0, sizeof pages);
    memset(cache, 0, sizeof cache);
}

const char *doc_path(void)
{
    return path;
}

int doc_exists(void)
{
    return exists;
}

static int get_node(struct root root, struct node *node)
{
    off_t id = root.id;
    size_t slot = (size_t)(id / sizeof *node) % NODE_CACHE;

    if (id >= indexed) {
        *node = pending[(id - indexed) / sizeof *node];
        return 0;
    }
    if (!cache[slot].valid || cache[slot].id != id) {
        if (transfer(index_fd, id, node, sizeof *node, 0) < 0)
            return -1;
        cache[slot].node = *node;
        cache[slot].id = id;
        cache[slot].valid = 1;
    } else {
        *node = cache[slot].node;
    }
    return 0;
}

static void flush_nodes(void)
{
    size_t bytes = pending_count * sizeof *pending;

    if (!edit_error && bytes) {
        if (indexed > INT64_MAX - (off_t)bytes)
            edit_error = EFBIG;
        else if (transfer(index_fd, indexed, pending, bytes, 1) < 0)
            edit_error = errno;
        else
            indexed += (off_t)bytes;
    }
    pending_count = 0;
}

static struct root parent(struct root left, struct root right)
{
    unsigned height = left.height > right.height ? left.height : right.height;
    off_t id;

    if (pending_count == NODE_BATCH)
        flush_nodes();
    if (edit_error)
        return (struct root){0};
    if (indexed > INT64_MAX - (off_t)((pending_count + 1) * sizeof *pending)) {
        edit_error = EFBIG;
        return (struct root){0};
    }
    id = indexed + (off_t)(pending_count * sizeof *pending);
    pending[pending_count++] = (struct node){left, right};
    return (struct root){left.size + right.size, id, height + 1};
}

static struct node children(struct root root)
{
    struct node node = {0};

    if (!edit_error && get_node(root, &node) < 0)
        edit_error = errno;
    return node;
}

static struct root balance(struct root left, struct root right)
{
    if (edit_error)
        return (struct root){0};
    if (left.height > right.height + 1) {
        struct node a = children(left);

        if (a.left.height >= a.right.height)
            return parent(a.left, parent(a.right, right));
        struct node b = children(a.right);
        return parent(parent(a.left, b.left), parent(b.right, right));
    }
    if (right.height > left.height + 1) {
        struct node a = children(right);

        if (a.right.height >= a.left.height)
            return parent(parent(left, a.left), a.right);
        struct node b = children(a.left);
        return parent(parent(left, b.left), parent(b.right, a.right));
    }
    return parent(left, right);
}

static struct root leaf_slice(struct root leaf, off_t first, off_t len)
{
    if (!len)
        return (struct root){0};
    return (struct root){len, leaf.id < 0 ? leaf.id - first : leaf.id + first, 1};
}

/* Join two AVL ropes, copying only their boundary paths. Coalesce adjacent
 * spans when the entire join fits in a single leaf. */
static struct root join(struct root left, struct root right)
{
    if (edit_error || !right.height)
        return left;
    if (!left.height)
        return right;
    if (left.height == 1 && right.height == 1 &&
        ((left.id >= 0 && right.id >= 0 && left.id + left.size == right.id) ||
         (left.id < 0 && right.id < 0 && left.id - left.size == right.id))) {
        left.size += right.size;
        return left;
    }
    if (left.height > right.height + 1) {
        struct node a = children(left);

        return balance(a.left, join(a.right, right));
    }
    if (right.height > left.height + 1) {
        struct node a = children(right);

        return balance(join(left, a.left), a.right);
    }
    return parent(left, right);
}

static void split(struct root root, off_t pos, struct root *left, struct root *right)
{
    if (edit_error || pos == 0) {
        *left = (struct root){0};
        *right = root;
    } else if (pos == root.size) {
        *left = root;
        *right = (struct root){0};
    } else if (root.height == 1) {
        *left = leaf_slice(root, 0, pos);
        *right = leaf_slice(root, pos, root.size - pos);
    } else {
        struct node a = children(root);

        if (pos < a.left.size) {
            split(a.left, pos, left, right);
            *right = join(*right, a.right);
        } else {
            split(a.right, pos - a.left.size, left, right);
            *left = join(a.left, *left);
        }
    }
}

static int read_span(struct root leaf, off_t pos, unsigned char *out, size_t len)
{
    int source = leaf.id < 0;
    struct page *page = &pages[source];
    off_t offset = (source ? -(leaf.id + 1) : leaf.id) + pos;
    int fd = source ? additions : original;

    while (len) {
        off_t start = offset - offset % DOC_PAGE;
        size_t in_page = (size_t)(offset - start);
        size_t n;

        if (page->start != start || page->len <= in_page) {
            ssize_t got;

            do {
                got = pread(fd, page->bytes, sizeof page->bytes, start);
            } while (got < 0 && errno == EINTR);
            if (got <= (ssize_t)in_page) {
                if (got >= 0)
                    errno = EIO;
                page->len = 0;
                return -1;
            }
            page->start = start;
            page->len = (size_t)got;
        }
        n = page->len - in_page;
        if (n > len)
            n = len;
        memcpy(out, page->bytes + in_page, n);
        out += n;
        offset += n;
        len -= n;
    }
    return 0;
}

static int read_tree(struct root root, off_t pos, unsigned char *out, size_t len)
{
    struct node a;

    if (root.height == 1)
        return read_span(root, pos, out, len);
    if (get_node(root, &a) < 0)
        return -1;
    if (pos < a.left.size) {
        off_t available = a.left.size - pos;
        size_t take = available < (off_t)len ? (size_t)available : len;

        if (read_tree(a.left, pos, out, take) < 0)
            return -1;
        len -= take;
        if (len == 0)
            return 0;
        out += take;
        pos = a.left.size;
    }
    return read_tree(a.right, pos - a.left.size, out, len);
}

int doc_read(struct root root, off_t pos, void *buf, size_t len)
{
    if (pos < 0 || pos > root.size || (uint64_t)len > (uint64_t)(root.size - pos)) {
        errno = EINVAL;
        return -1;
    }
    return len ? read_tree(root, pos, buf, len) : 0;
}

int doc_edit(struct root *root, off_t pos, off_t remove, const void *data, size_t len)
{
    struct root left, rest, discarded, right, insert = {0}, result;

    if (pos < 0 || pos > root->size || remove < 0 || remove > root->size - pos) {
        errno = EINVAL;
        return -1;
    }
    if ((uint64_t)len > (uint64_t)(INT64_MAX - (root->size - remove)) ||
        (uint64_t)len > (uint64_t)(INT64_MAX - added)) {
        errno = EFBIG;
        return -1;
    }
    if (!remove && !len)
        return 0;
    if (index_fd < 0) {
        index_fd = scratch_file();
        if (index_fd < 0)
            return -1;
    }
    if (len) {
        if (additions < 0)
            additions = scratch_file();
        if (pages[1].len && added - pages[1].start < (off_t)DOC_PAGE)
            pages[1].len = 0;
        if (additions < 0 || transfer(additions, added, (void *)data, len, 1) < 0)
            return -1;
        insert = (struct root){(off_t)len, -added - 1, 1};
        added += (off_t)len;
    }
    edit_error = 0;
    split(*root, pos, &left, &rest);
    split(rest, remove, &discarded, &right);
    result = join(join(left, insert), right);
    flush_nodes();
    if (edit_error) {
        errno = edit_error;
        return -1;
    }
    *root = result;
    return 0;
}

static int same_time(struct timespec a, struct timespec b)
{
    return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
}

/* Whether target is still the file we opened or last saved. Not by inode
 * number: vfat makes a new one whenever the kernel drops the inode from its
 * cache, nor by ctime, which vfat reloads as the mtime. */
static int unchanged(const char *target, const struct stat *want, int present)
{
    struct stat st;

    if (lstat(target, &st) < 0) {
        if (!present && errno == ENOENT)
            return 1;
        return 0;
    }
    if (present && S_ISREG(st.st_mode) && st.st_dev == want->st_dev &&
        st.st_size == want->st_size &&
#ifdef __APPLE__
        same_time(st.st_mtimespec, want->st_mtimespec))
#else
        same_time(st.st_mtim, want->st_mtim))
#endif
        return 1;
    errno = EBUSY;
    return 0;
}

static mode_t mask(void)
{
    mode_t m = umask(0);

    umask(m);
    return m;
}

static int save_to(struct root root, const char *target, const struct stat *want, int present)
{
    unsigned char buf[DOC_PAGE];
    char *name;
    int fd, error = 0;

    if (!unchanged(target, want, present))
        return -1;
    fd = temp_file(target, &name);
    if (fd < 0)
        return -1;
    for (off_t pos = 0; pos < root.size;) {
        size_t len = root.size - pos > (off_t)sizeof buf ? sizeof buf : (size_t)(root.size - pos);

        if (doc_read(root, pos, buf, len) < 0 || transfer(fd, pos, buf, len, 1) < 0) {
            error = errno;
            break;
        }
        pos += (off_t)len;
    }
    /* mkstemp made it 0600: keep the old file's mode, or give a new one
     * what open() would */
    if (!error && fchmod(fd, present ? want->st_mode & 0777 : 0666 & ~mask()) < 0)
        error = errno;
    if (!error && fsync(fd) < 0)
        error = errno;
    if (!error && (!unchanged(target, want, present) || rename(name, target) < 0))
        error = errno;
    if (!error) {
        exists = 1;
        if (fstat(fd, &expected) < 0)
            error = errno;
    }
    close(fd);
    if (error)
        unlink(name);
    free(name);
    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

int doc_save(struct root root)
{
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    return save_to(root, path, &expected, exists);
}

int doc_save_as(struct root root, const char *name, int overwrite)
{
    struct stat st = {0};
    int present, rc, error;
    char *target = resolve(name, &st, &present);

    if (!target)
        return -1;
    /* the open file too: replacing it means saving over whatever is there now */
    if (present && !overwrite) {
        free(target);
        errno = EEXIST;
        return -1;
    }
    rc = save_to(root, target, &st, present);
    error = errno;
    if (rc == 0) {
        free(path);
        path = target;
    } else {
        free(target);
        errno = error;
    }
    return rc;
}
