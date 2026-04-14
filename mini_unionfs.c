#define _GNU_SOURCE
#define FUSE_USE_VERSION 31

#include <fuse.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#define WHITEOUT_PREFIX ".wh."
#define OPAQUE_MARKER ".wh..wh..opq"
#define COPY_BUFFER_SIZE 65536

struct mini_unionfs_state {
    char *lower_dir;
    char *upper_dir;
};

#define UNIONFS_DATA ((struct mini_unionfs_state *)fuse_get_context()->private_data)

enum resolved_source {
    RESOLVED_NONE = 0,
    RESOLVED_UPPER = 1,
    RESOLVED_LOWER = 2,
};

struct path_lookup {
    char upper[PATH_MAX];
    char lower[PATH_MAX];
    char whiteout[PATH_MAX];
    int hidden;
    int upper_exists;
    int lower_exists;
    int whiteout_exists;
    struct stat upper_st;
    struct stat lower_st;
};

struct name_list {
    char **items;
    size_t count;
    size_t capacity;
};

static int checked_snprintf(char *buf, size_t size, const char *fmt, ...);
static int lstat_optional(const char *path, struct stat *stbuf);
static int build_host_path(const char *root, const char *path, char *out, size_t out_size);
static int next_path_component(const char **cursor, char *component, size_t component_size);
static int append_union_component(const char *base, const char *name, char *out, size_t out_size);
static int split_union_path(
    const char *path,
    char *parent,
    size_t parent_size,
    char *name,
    size_t name_size);
static int build_whiteout_path(const char *path, char *out, size_t out_size);
static int build_opaque_path(const char *path, char *out, size_t out_size);
static int is_whiteouted(const char *path);
static int is_opaque_directory(const char *path);
static int path_has_opaque_prefix(const char *path, bool include_self);
static int lookup_path(const char *path, struct path_lookup *lookup);
static int resolve_path(
    const char *path,
    char *resolved_path,
    size_t resolved_path_size,
    enum resolved_source *source);
static void name_list_destroy(struct name_list *list);
static bool name_list_contains(const struct name_list *list, const char *name);
static int name_list_add_unique(struct name_list *list, const char *name);
static bool open_requires_copy_up(int flags);
static int ensure_upper_parents(const char *path);
static int remove_whiteout(const char *path);
static int create_whiteout(const char *path);
static int create_opaque_marker(const char *path);
static int copy_file_contents(int from_fd, int to_fd);
static int copy_up_file(const char *path);
static int collect_merged_dir_entries(const char *path, struct name_list *entries);
static int merged_dir_is_empty(const char *path);
static int purge_upper_metadata(const char *upper_dir_path);
static void *unionfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg);
static int unionfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi);
static int unionfs_statfs(const char *path, struct statvfs *stbuf);
static int unionfs_readdir(
    const char *path,
    void *buf,
    fuse_fill_dir_t filler,
    off_t offset,
    struct fuse_file_info *fi,
    enum fuse_readdir_flags flags);
static int unionfs_open(const char *path, struct fuse_file_info *fi);
static int unionfs_create(const char *path, mode_t mode, struct fuse_file_info *fi);
static int unionfs_read(
    const char *path,
    char *buf,
    size_t size,
    off_t offset,
    struct fuse_file_info *fi);
static int unionfs_write(
    const char *path,
    const char *buf,
    size_t size,
    off_t offset,
    struct fuse_file_info *fi);
static int unionfs_truncate(const char *path, off_t size, struct fuse_file_info *fi);
static int unionfs_release(const char *path, struct fuse_file_info *fi);
static int unionfs_unlink(const char *path);
static int unionfs_mkdir(const char *path, mode_t mode);
static int unionfs_rmdir(const char *path);
static void print_usage(const char *program_name);
static int validate_directory(const char *path, const char *label);

static int checked_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    int written;

    va_start(args, fmt);
    written = vsnprintf(buf, size, fmt, args);
    va_end(args);

    if (written < 0 || (size_t)written >= size) {
        return -ENAMETOOLONG;
    }

    return 0;
}

static int lstat_optional(const char *path, struct stat *stbuf)
{
    struct stat local_st;

    if (lstat(path, stbuf != NULL ? stbuf : &local_st) == 0) {
        return 1;
    }

    if (errno == ENOENT || errno == ENOTDIR) {
        return 0;
    }

    return -errno;
}

static int build_host_path(const char *root, const char *path, char *out, size_t out_size)
{
    if (strcmp(path, "/") == 0) {
        return checked_snprintf(out, out_size, "%s", root);
    }

    return checked_snprintf(out, out_size, "%s%s", root, path);
}

static int next_path_component(const char **cursor, char *component, size_t component_size)
{
    const char *start = *cursor;
    const char *end;
    size_t length;

    while (*start == '/') {
        start++;
    }

    if (*start == '\0') {
        *cursor = start;
        return 0;
    }

    end = start;
    while (*end != '\0' && *end != '/') {
        end++;
    }

    length = (size_t)(end - start);
    if (length + 1 > component_size) {
        return -ENAMETOOLONG;
    }

    memcpy(component, start, length);
    component[length] = '\0';

    while (*end == '/') {
        end++;
    }

    *cursor = end;
    return 1;
}

static int append_union_component(const char *base, const char *name, char *out, size_t out_size)
{
    if (base[0] == '\0' || strcmp(base, "/") == 0) {
        return checked_snprintf(out, out_size, "/%s", name);
    }

    return checked_snprintf(out, out_size, "%s/%s", base, name);
}

static int split_union_path(
    const char *path,
    char *parent,
    size_t parent_size,
    char *name,
    size_t name_size)
{
    const char *slash;
    size_t parent_length;

    if (strcmp(path, "/") == 0) {
        return -EINVAL;
    }

    slash = strrchr(path, '/');
    if (slash == NULL || slash[1] == '\0') {
        return -EINVAL;
    }

    if (slash == path) {
        if (checked_snprintf(parent, parent_size, "/") < 0) {
            return -ENAMETOOLONG;
        }
    } else {
        parent_length = (size_t)(slash - path);
        if (parent_length + 1 > parent_size) {
            return -ENAMETOOLONG;
        }
        memcpy(parent, path, parent_length);
        parent[parent_length] = '\0';
    }

    if (strlen(slash + 1) + 1 > name_size) {
        return -ENAMETOOLONG;
    }
    strcpy(name, slash + 1);

    return 0;
}

static int build_whiteout_path(const char *path, char *out, size_t out_size)
{
    char parent[PATH_MAX];
    char name[NAME_MAX + 1];
    char parent_host[PATH_MAX];
    int ret;

    if (strcmp(path, "/") == 0) {
        return -EINVAL;
    }

    ret = split_union_path(path, parent, sizeof(parent), name, sizeof(name));
    if (ret < 0) {
        return ret;
    }

    ret = build_host_path(UNIONFS_DATA->upper_dir, parent, parent_host, sizeof(parent_host));
    if (ret < 0) {
        return ret;
    }

    return checked_snprintf(out, out_size, "%s/%s%s", parent_host, WHITEOUT_PREFIX, name);
}

static int build_opaque_path(const char *path, char *out, size_t out_size)
{
    char upper_path[PATH_MAX];
    int ret;

    ret = build_host_path(UNIONFS_DATA->upper_dir, path, upper_path, sizeof(upper_path));
    if (ret < 0) {
        return ret;
    }

    return checked_snprintf(out, out_size, "%s/%s", upper_path, OPAQUE_MARKER);
}

static int is_whiteouted(const char *path)
{
    char component[NAME_MAX + 1];
    char current[PATH_MAX] = "";
    char next[PATH_MAX];
    char whiteout[PATH_MAX];
    const char *cursor = path;
    int step;
    int ret;

    if (strcmp(path, "/") == 0) {
        return 0;
    }

    for (;;) {
        step = next_path_component(&cursor, component, sizeof(component));
        if (step < 0) {
            return step;
        }
        if (step == 0) {
            break;
        }

        ret = append_union_component(current, component, next, sizeof(next));
        if (ret < 0) {
            return ret;
        }

        ret = build_whiteout_path(next, whiteout, sizeof(whiteout));
        if (ret == 0) {
            step = lstat_optional(whiteout, NULL);
            if (step < 0) {
                return step;
            }
            if (step == 1) {
                return 1;
            }
        }

        ret = checked_snprintf(current, sizeof(current), "%s", next);
        if (ret < 0) {
            return ret;
        }
    }

    return 0;
}

static int is_opaque_directory(const char *path)
{
    char opaque_path[PATH_MAX];
    int ret;

    ret = build_opaque_path(path, opaque_path, sizeof(opaque_path));
    if (ret < 0) {
        return ret;
    }

    return lstat_optional(opaque_path, NULL);
}

static int path_has_opaque_prefix(const char *path, bool include_self)
{
    char component[NAME_MAX + 1];
    char current[PATH_MAX] = "";
    char next[PATH_MAX];
    const char *cursor = path;
    int step;
    int ret;

    if (strcmp(path, "/") == 0) {
        return 0;
    }

    for (;;) {
        step = next_path_component(&cursor, component, sizeof(component));
        if (step < 0) {
            return step;
        }
        if (step == 0) {
            break;
        }

        ret = append_union_component(current, component, next, sizeof(next));
        if (ret < 0) {
            return ret;
        }

        if (*cursor != '\0' || include_self) {
            ret = is_opaque_directory(next);
            if (ret < 0) {
                return ret;
            }
            if (ret == 1) {
                return 1;
            }
        }

        ret = checked_snprintf(current, sizeof(current), "%s", next);
        if (ret < 0) {
            return ret;
        }
    }

    return 0;
}

static int lookup_path(const char *path, struct path_lookup *lookup)
{
    int ret;

    memset(lookup, 0, sizeof(*lookup));

    ret = build_host_path(UNIONFS_DATA->upper_dir, path, lookup->upper, sizeof(lookup->upper));
    if (ret < 0) {
        return ret;
    }

    ret = build_host_path(UNIONFS_DATA->lower_dir, path, lookup->lower, sizeof(lookup->lower));
    if (ret < 0) {
        return ret;
    }

    if (strcmp(path, "/") != 0) {
        ret = build_whiteout_path(path, lookup->whiteout, sizeof(lookup->whiteout));
        if (ret < 0) {
            return ret;
        }

        ret = lstat_optional(lookup->whiteout, NULL);
        if (ret < 0) {
            return ret;
        }
        lookup->whiteout_exists = ret;
    }

    ret = is_whiteouted(path);
    if (ret < 0) {
        return ret;
    }
    lookup->hidden = ret;

    ret = lstat_optional(lookup->upper, &lookup->upper_st);
    if (ret < 0) {
        return ret;
    }
    lookup->upper_exists = ret;

    ret = lstat_optional(lookup->lower, &lookup->lower_st);
    if (ret < 0) {
        return ret;
    }
    lookup->lower_exists = ret;

    return 0;
}

static int resolve_path(
    const char *path,
    char *resolved_path,
    size_t resolved_path_size,
    enum resolved_source *source)
{
    struct path_lookup lookup;
    int ret;

    ret = lookup_path(path, &lookup);
    if (ret < 0) {
        return ret;
    }

    if (lookup.hidden) {
        return -ENOENT;
    }

    if (lookup.upper_exists) {
        if (resolved_path != NULL) {
            ret = checked_snprintf(resolved_path, resolved_path_size, "%s", lookup.upper);
            if (ret < 0) {
                return ret;
            }
        }
        if (source != NULL) {
            *source = RESOLVED_UPPER;
        }
        return 0;
    }

    if (lookup.lower_exists) {
        ret = path_has_opaque_prefix(path, false);
        if (ret < 0) {
            return ret;
        }
        if (ret == 0) {
            if (resolved_path != NULL) {
                ret = checked_snprintf(resolved_path, resolved_path_size, "%s", lookup.lower);
                if (ret < 0) {
                    return ret;
                }
            }
            if (source != NULL) {
                *source = RESOLVED_LOWER;
            }
            return 0;
        }
    }

    return -ENOENT;
}

static void name_list_destroy(struct name_list *list)
{
    size_t i;

    for (i = 0; i < list->count; ++i) {
        free(list->items[i]);
    }

    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static bool name_list_contains(const struct name_list *list, const char *name)
{
    size_t i;

    for (i = 0; i < list->count; ++i) {
        if (strcmp(list->items[i], name) == 0) {
            return true;
        }
    }

    return false;
}

static int name_list_add_unique(struct name_list *list, const char *name)
{
    char **new_items;

    if (name_list_contains(list, name)) {
        return 0;
    }

    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        new_items = realloc(list->items, new_capacity * sizeof(char *));
        if (new_items == NULL) {
            return -ENOMEM;
        }

        list->items = new_items;
        list->capacity = new_capacity;
    }

    list->items[list->count] = strdup(name);
    if (list->items[list->count] == NULL) {
        return -ENOMEM;
    }

    list->count++;
    return 1;
}

static bool open_requires_copy_up(int flags)
{
    return ((flags & O_ACCMODE) != O_RDONLY) || (flags & O_TRUNC);
}

static int ensure_upper_parents(const char *path)
{
    char parent[PATH_MAX];
    char name[NAME_MAX + 1];
    char current[PATH_MAX] = "";
    char next[PATH_MAX];
    char component[NAME_MAX + 1];
    char resolved[PATH_MAX];
    char upper_path[PATH_MAX];
    struct stat st;
    enum resolved_source source;
    const char *cursor;
    int ret;
    int step;

    ret = split_union_path(path, parent, sizeof(parent), name, sizeof(name));
    if (ret < 0) {
        return ret;
    }

    if (strcmp(parent, "/") == 0) {
        return 0;
    }

    cursor = parent;
    for (;;) {
        step = next_path_component(&cursor, component, sizeof(component));
        if (step < 0) {
            return step;
        }
        if (step == 0) {
            break;
        }

        ret = append_union_component(current, component, next, sizeof(next));
        if (ret < 0) {
            return ret;
        }

        ret = resolve_path(next, resolved, sizeof(resolved), &source);
        if (ret < 0) {
            return ret;
        }

        if (lstat(resolved, &st) == -1) {
            return -errno;
        }
        if (!S_ISDIR(st.st_mode)) {
            return -ENOTDIR;
        }

        if (source == RESOLVED_LOWER) {
            ret = build_host_path(UNIONFS_DATA->upper_dir, next, upper_path, sizeof(upper_path));
            if (ret < 0) {
                return ret;
            }

            if (mkdir(upper_path, st.st_mode & 0777) == -1 && errno != EEXIST) {
                return -errno;
            }
        }

        ret = checked_snprintf(current, sizeof(current), "%s", next);
        if (ret < 0) {
            return ret;
        }
    }

    return 0;
}

static int remove_whiteout(const char *path)
{
    char whiteout[PATH_MAX];
    int ret;

    if (strcmp(path, "/") == 0) {
        return 0;
    }

    ret = build_whiteout_path(path, whiteout, sizeof(whiteout));
    if (ret < 0) {
        return ret;
    }

    if (unlink(whiteout) == -1 && errno != ENOENT) {
        return -errno;
    }

    return 0;
}

static int create_whiteout(const char *path)
{
    char whiteout[PATH_MAX];
    int fd;
    int ret;

    if (strcmp(path, "/") == 0) {
        return -EPERM;
    }

    ret = ensure_upper_parents(path);
    if (ret < 0) {
        return ret;
    }

    ret = build_whiteout_path(path, whiteout, sizeof(whiteout));
    if (ret < 0) {
        return ret;
    }

    fd = open(whiteout, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd == -1) {
        if (errno == EEXIST) {
            return 0;
        }
        return -errno;
    }

    if (close(fd) == -1) {
        return -errno;
    }

    return 0;
}

static int create_opaque_marker(const char *path)
{
    char opaque_path[PATH_MAX];
    int fd;
    int ret;

    ret = build_opaque_path(path, opaque_path, sizeof(opaque_path));
    if (ret < 0) {
        return ret;
    }

    fd = open(opaque_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd == -1) {
        return -errno;
    }

    if (close(fd) == -1) {
        return -errno;
    }

    return 0;
}

static int copy_file_contents(int from_fd, int to_fd)
{
    char buffer[COPY_BUFFER_SIZE];

    for (;;) {
        ssize_t bytes_read = read(from_fd, buffer, sizeof(buffer));
        ssize_t written_total = 0;

        if (bytes_read == 0) {
            return 0;
        }
        if (bytes_read < 0) {
            return -errno;
        }

        while (written_total < bytes_read) {
            ssize_t bytes_written = write(to_fd, buffer + written_total, (size_t)(bytes_read - written_total));
            if (bytes_written < 0) {
                return -errno;
            }
            written_total += bytes_written;
        }
    }
}

static int copy_up_file(const char *path)
{
    char resolved[PATH_MAX];
    char upper_path[PATH_MAX];
    struct stat lower_st;
    struct timespec times[2];
    enum resolved_source source;
    int from_fd = -1;
    int to_fd = -1;
    int ret;

    ret = resolve_path(path, resolved, sizeof(resolved), &source);
    if (ret < 0) {
        return ret;
    }

    if (source == RESOLVED_UPPER) {
        return 0;
    }

    if (lstat(resolved, &lower_st) == -1) {
        return -errno;
    }

    if (!S_ISREG(lower_st.st_mode)) {
        if (S_ISDIR(lower_st.st_mode)) {
            return -EISDIR;
        }
        return -EINVAL;
    }

    ret = ensure_upper_parents(path);
    if (ret < 0) {
        return ret;
    }

    ret = build_host_path(UNIONFS_DATA->upper_dir, path, upper_path, sizeof(upper_path));
    if (ret < 0) {
        return ret;
    }

    from_fd = open(resolved, O_RDONLY);
    if (from_fd == -1) {
        return -errno;
    }

    to_fd = open(upper_path, O_WRONLY | O_CREAT | O_EXCL, lower_st.st_mode & 0777);
    if (to_fd == -1) {
        ret = (errno == EEXIST) ? 0 : -errno;
        close(from_fd);
        return ret;
    }

    ret = copy_file_contents(from_fd, to_fd);
    if (ret < 0) {
        close(from_fd);
        close(to_fd);
        unlink(upper_path);
        return ret;
    }

    if (fchmod(to_fd, lower_st.st_mode & 07777) == -1) {
        ret = -errno;
        close(from_fd);
        close(to_fd);
        unlink(upper_path);
        return ret;
    }

    times[0] = lower_st.st_atim;
    times[1] = lower_st.st_mtim;
    if (futimens(to_fd, times) == -1) {
        ret = -errno;
        close(from_fd);
        close(to_fd);
        unlink(upper_path);
        return ret;
    }

    if (close(from_fd) == -1) {
        close(to_fd);
        unlink(upper_path);
        return -errno;
    }

    if (close(to_fd) == -1) {
        unlink(upper_path);
        return -errno;
    }

    return 0;
}

static int collect_merged_dir_entries(const char *path, struct name_list *entries)
{
    struct path_lookup lookup;
    struct name_list whiteouts = {0};
    char resolved[PATH_MAX];
    enum resolved_source source;
    struct stat visible_st;
    DIR *dir = NULL;
    struct dirent *entry;
    int use_upper;
    int use_lower;
    int lower_blocked;
    int ret;

    memset(entries, 0, sizeof(*entries));

    ret = resolve_path(path, resolved, sizeof(resolved), &source);
    if (ret < 0) {
        return ret;
    }

    if (lstat(resolved, &visible_st) == -1) {
        return -errno;
    }
    if (!S_ISDIR(visible_st.st_mode)) {
        return -ENOTDIR;
    }

    ret = lookup_path(path, &lookup);
    if (ret < 0) {
        return ret;
    }

    lower_blocked = path_has_opaque_prefix(path, true);
    if (lower_blocked < 0) {
        return lower_blocked;
    }

    use_upper = lookup.upper_exists && S_ISDIR(lookup.upper_st.st_mode);
    use_lower = lookup.lower_exists && S_ISDIR(lookup.lower_st.st_mode) && !lower_blocked;

    if (use_upper) {
        dir = opendir(lookup.upper);
        if (dir == NULL) {
            ret = -errno;
            goto cleanup;
        }

        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            if (strcmp(entry->d_name, OPAQUE_MARKER) == 0) {
                continue;
            }

            if (strncmp(entry->d_name, WHITEOUT_PREFIX, strlen(WHITEOUT_PREFIX)) == 0) {
                ret = name_list_add_unique(&whiteouts, entry->d_name + strlen(WHITEOUT_PREFIX));
                if (ret < 0) {
                    closedir(dir);
                    goto cleanup;
                }
                continue;
            }

            ret = name_list_add_unique(entries, entry->d_name);
            if (ret < 0) {
                closedir(dir);
                goto cleanup;
            }
        }

        if (closedir(dir) == -1) {
            ret = -errno;
            goto cleanup;
        }
        dir = NULL;
    }

    if (use_lower) {
        dir = opendir(lookup.lower);
        if (dir == NULL) {
            ret = -errno;
            goto cleanup;
        }

        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            if (name_list_contains(&whiteouts, entry->d_name) || name_list_contains(entries, entry->d_name)) {
                continue;
            }

            ret = name_list_add_unique(entries, entry->d_name);
            if (ret < 0) {
                closedir(dir);
                goto cleanup;
            }
        }

        if (closedir(dir) == -1) {
            ret = -errno;
            goto cleanup;
        }
        dir = NULL;
    }

    ret = 0;

cleanup:
    if (dir != NULL) {
        closedir(dir);
    }
    name_list_destroy(&whiteouts);
    if (ret < 0) {
        name_list_destroy(entries);
    }
    return ret;
}

static int merged_dir_is_empty(const char *path)
{
    struct name_list entries;
    int ret;

    ret = collect_merged_dir_entries(path, &entries);
    if (ret < 0) {
        return ret;
    }

    ret = (entries.count == 0) ? 1 : 0;
    name_list_destroy(&entries);
    return ret;
}

static int purge_upper_metadata(const char *upper_dir_path)
{
    DIR *dir;
    struct dirent *entry;

    dir = opendir(upper_dir_path);
    if (dir == NULL) {
        return -errno;
    }

    while ((entry = readdir(dir)) != NULL) {
        char child[PATH_MAX];
        int ret;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        ret = checked_snprintf(child, sizeof(child), "%s/%s", upper_dir_path, entry->d_name);
        if (ret < 0) {
            closedir(dir);
            return ret;
        }

        if (strcmp(entry->d_name, OPAQUE_MARKER) == 0 ||
            strncmp(entry->d_name, WHITEOUT_PREFIX, strlen(WHITEOUT_PREFIX)) == 0) {
            if (unlink(child) == -1) {
                ret = -errno;
                closedir(dir);
                return ret;
            }
            continue;
        }

        closedir(dir);
        return -ENOTEMPTY;
    }

    if (closedir(dir) == -1) {
        return -errno;
    }

    return 0;
}

static void *unionfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;

    cfg->kernel_cache = 0;
    cfg->entry_timeout = 0;
    cfg->attr_timeout = 0;
    cfg->negative_timeout = 0;
    cfg->use_ino = 1;

    return UNIONFS_DATA;
}

static int unionfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
    char resolved[PATH_MAX];
    int ret;

    (void)fi;
    memset(stbuf, 0, sizeof(*stbuf));

    ret = resolve_path(path, resolved, sizeof(resolved), NULL);
    if (ret < 0) {
        return ret;
    }

    if (lstat(resolved, stbuf) == -1) {
        return -errno;
    }

    return 0;
}

static int unionfs_statfs(const char *path, struct statvfs *stbuf)
{
    (void)path;

    if (statvfs(UNIONFS_DATA->upper_dir, stbuf) == -1) {
        return -errno;
    }

    return 0;
}

static int unionfs_readdir(
    const char *path,
    void *buf,
    fuse_fill_dir_t filler,
    off_t offset,
    struct fuse_file_info *fi,
    enum fuse_readdir_flags flags)
{
    struct name_list entries;
    size_t i;
    int ret;

    (void)offset;
    (void)fi;
    (void)flags;

    ret = collect_merged_dir_entries(path, &entries);
    if (ret < 0) {
        return ret;
    }

    if (filler(buf, ".", NULL, 0, 0) != 0 || filler(buf, "..", NULL, 0, 0) != 0) {
        name_list_destroy(&entries);
        return 0;
    }

    for (i = 0; i < entries.count; ++i) {
        if (filler(buf, entries.items[i], NULL, 0, 0) != 0) {
            break;
        }
    }

    name_list_destroy(&entries);
    return 0;
}

static int unionfs_open(const char *path, struct fuse_file_info *fi)
{
    char resolved[PATH_MAX];
    struct stat st;
    int open_flags;
    int fd;
    int ret;

    if (open_requires_copy_up(fi->flags)) {
        ret = copy_up_file(path);
        if (ret < 0) {
            return ret;
        }
    }

    ret = resolve_path(path, resolved, sizeof(resolved), NULL);
    if (ret < 0) {
        return ret;
    }

    if (lstat(resolved, &st) == -1) {
        return -errno;
    }
    if (S_ISDIR(st.st_mode)) {
        return -EISDIR;
    }

    open_flags = fi->flags & ~(O_CREAT | O_EXCL);
    fd = open(resolved, open_flags);
    if (fd == -1) {
        return -errno;
    }

    fi->fh = (uint64_t)fd;
    return 0;
}

static int unionfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    struct path_lookup lookup;
    int fd;
    int ret;

    ret = resolve_path(path, NULL, 0, NULL);
    if (ret == 0) {
        return -EEXIST;
    }
    if (ret != -ENOENT) {
        return ret;
    }

    ret = lookup_path(path, &lookup);
    if (ret < 0) {
        return ret;
    }

    ret = ensure_upper_parents(path);
    if (ret < 0) {
        return ret;
    }

    fd = open(lookup.upper, fi->flags | O_CREAT, mode);
    if (fd == -1) {
        return -errno;
    }

    ret = remove_whiteout(path);
    if (ret < 0) {
        close(fd);
        unlink(lookup.upper);
        return ret;
    }

    fi->fh = (uint64_t)fd;
    return 0;
}

static int unionfs_read(
    const char *path,
    char *buf,
    size_t size,
    off_t offset,
    struct fuse_file_info *fi)
{
    int fd = -1;
    int close_fd = 0;
    ssize_t result;

    if (fi != NULL) {
        fd = (int)fi->fh;
    } else {
        char resolved[PATH_MAX];
        int ret = resolve_path(path, resolved, sizeof(resolved), NULL);
        if (ret < 0) {
            return ret;
        }

        fd = open(resolved, O_RDONLY);
        if (fd == -1) {
            return -errno;
        }
        close_fd = 1;
    }

    result = pread(fd, buf, size, offset);
    if (result < 0) {
        if (close_fd) {
            close(fd);
        }
        return -errno;
    }

    if (close_fd && close(fd) == -1) {
        return -errno;
    }

    return (int)result;
}

static int unionfs_write(
    const char *path,
    const char *buf,
    size_t size,
    off_t offset,
    struct fuse_file_info *fi)
{
    int fd = -1;
    int close_fd = 0;
    ssize_t result;

    if (fi != NULL) {
        fd = (int)fi->fh;
    } else {
        char resolved[PATH_MAX];
        int ret = copy_up_file(path);
        if (ret < 0) {
            return ret;
        }

        ret = resolve_path(path, resolved, sizeof(resolved), NULL);
        if (ret < 0) {
            return ret;
        }

        fd = open(resolved, O_WRONLY);
        if (fd == -1) {
            return -errno;
        }
        close_fd = 1;
    }

    result = pwrite(fd, buf, size, offset);
    if (result < 0) {
        if (close_fd) {
            close(fd);
        }
        return -errno;
    }

    if (close_fd && close(fd) == -1) {
        return -errno;
    }

    return (int)result;
}

static int unionfs_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    char resolved[PATH_MAX];
    int ret;

    ret = copy_up_file(path);
    if (ret < 0 && ret != -EISDIR) {
        return ret;
    }

    ret = resolve_path(path, resolved, sizeof(resolved), NULL);
    if (ret < 0) {
        return ret;
    }

    if (fi != NULL) {
        if (ftruncate((int)fi->fh, size) == -1) {
            return -errno;
        }
        return 0;
    }

    if (truncate(resolved, size) == -1) {
        return -errno;
    }

    return 0;
}

static int unionfs_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;

    if (close((int)fi->fh) == -1) {
        return -errno;
    }

    return 0;
}

static int unionfs_unlink(const char *path)
{
    struct path_lookup lookup;
    char resolved[PATH_MAX];
    enum resolved_source source;
    struct stat st;
    int lower_blocked;
    int ret;

    ret = resolve_path(path, resolved, sizeof(resolved), &source);
    if (ret < 0) {
        return ret;
    }

    if (lstat(resolved, &st) == -1) {
        return -errno;
    }
    if (S_ISDIR(st.st_mode)) {
        return -EISDIR;
    }

    ret = lookup_path(path, &lookup);
    if (ret < 0) {
        return ret;
    }

    lower_blocked = path_has_opaque_prefix(path, false);
    if (lower_blocked < 0) {
        return lower_blocked;
    }

    if (source == RESOLVED_UPPER) {
        if (unlink(lookup.upper) == -1) {
            return -errno;
        }

        if (lookup.lower_exists && !lower_blocked) {
            ret = create_whiteout(path);
            if (ret < 0) {
                return ret;
            }
        }

        return 0;
    }

    if (source == RESOLVED_LOWER) {
        return create_whiteout(path);
    }

    return -ENOENT;
}

static int unionfs_mkdir(const char *path, mode_t mode)
{
    struct path_lookup lookup;
    int needs_opaque;
    int ret;

    ret = resolve_path(path, NULL, 0, NULL);
    if (ret == 0) {
        return -EEXIST;
    }
    if (ret != -ENOENT) {
        return ret;
    }

    ret = lookup_path(path, &lookup);
    if (ret < 0) {
        return ret;
    }

    ret = ensure_upper_parents(path);
    if (ret < 0) {
        return ret;
    }

    if (mkdir(lookup.upper, mode) == -1) {
        return -errno;
    }

    needs_opaque = lookup.whiteout_exists && lookup.lower_exists && S_ISDIR(lookup.lower_st.st_mode);
    if (needs_opaque) {
        ret = create_opaque_marker(path);
        if (ret < 0) {
            rmdir(lookup.upper);
            return ret;
        }
    }

    ret = remove_whiteout(path);
    if (ret < 0) {
        purge_upper_metadata(lookup.upper);
        rmdir(lookup.upper);
        return ret;
    }

    return 0;
}

static int unionfs_rmdir(const char *path)
{
    struct path_lookup lookup;
    char resolved[PATH_MAX];
    enum resolved_source source;
    struct stat st;
    int empty;
    int lower_blocked;
    int ret;

    if (strcmp(path, "/") == 0) {
        return -EBUSY;
    }

    ret = resolve_path(path, resolved, sizeof(resolved), &source);
    if (ret < 0) {
        return ret;
    }

    if (lstat(resolved, &st) == -1) {
        return -errno;
    }
    if (!S_ISDIR(st.st_mode)) {
        return -ENOTDIR;
    }

    empty = merged_dir_is_empty(path);
    if (empty < 0) {
        return empty;
    }
    if (!empty) {
        return -ENOTEMPTY;
    }

    ret = lookup_path(path, &lookup);
    if (ret < 0) {
        return ret;
    }

    lower_blocked = path_has_opaque_prefix(path, false);
    if (lower_blocked < 0) {
        return lower_blocked;
    }

    if (source == RESOLVED_UPPER) {
        ret = purge_upper_metadata(lookup.upper);
        if (ret < 0) {
            return ret;
        }
        if (rmdir(lookup.upper) == -1) {
            return -errno;
        }

        if (lookup.lower_exists && !lower_blocked) {
            ret = create_whiteout(path);
            if (ret < 0) {
                return ret;
            }
        }

        return 0;
    }

    if (source == RESOLVED_LOWER) {
        return create_whiteout(path);
    }

    return -ENOENT;
}

static struct fuse_operations unionfs_oper = {
    .init = unionfs_init,
    .getattr = unionfs_getattr,
    .statfs = unionfs_statfs,
    .readdir = unionfs_readdir,
    .open = unionfs_open,
    .create = unionfs_create,
    .read = unionfs_read,
    .write = unionfs_write,
    .truncate = unionfs_truncate,
    .release = unionfs_release,
    .unlink = unionfs_unlink,
    .mkdir = unionfs_mkdir,
    .rmdir = unionfs_rmdir,
};

static void print_usage(const char *program_name)
{
    fprintf(
        stderr,
        "Usage: %s <lower_dir> <upper_dir> <mountpoint> [FUSE options...]\n",
        program_name);
}

static int validate_directory(const char *path, const char *label)
{
    struct stat st;

    if (stat(path, &st) == -1) {
        fprintf(stderr, "Failed to access %s '%s': %s\n", label, path, strerror(errno));
        return -1;
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "%s '%s' is not a directory.\n", label, path);
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    struct mini_unionfs_state *state;
    char **fuse_argv;
    int fuse_argc;
    int i;
    int ret;

    if (argc < 4) {
        print_usage(argv[0]);
        return 1;
    }

    state = calloc(1, sizeof(*state));
    if (state == NULL) {
        fprintf(stderr, "Failed to allocate filesystem state.\n");
        return 1;
    }

    if (validate_directory(argv[1], "Lower directory") != 0 ||
        validate_directory(argv[2], "Upper directory") != 0) {
        free(state);
        return 1;
    }

    state->lower_dir = realpath(argv[1], NULL);
    state->upper_dir = realpath(argv[2], NULL);
    if (state->lower_dir == NULL || state->upper_dir == NULL) {
        fprintf(stderr, "Failed to canonicalize branch directories: %s\n", strerror(errno));
        free(state->lower_dir);
        free(state->upper_dir);
        free(state);
        return 1;
    }

    fuse_argc = argc - 2;
    fuse_argv = calloc((size_t)fuse_argc + 1, sizeof(char *));
    if (fuse_argv == NULL) {
        fprintf(stderr, "Failed to allocate FUSE argv.\n");
        free(state->lower_dir);
        free(state->upper_dir);
        free(state);
        return 1;
    }

    fuse_argv[0] = argv[0];
    for (i = 3; i < argc; ++i) {
        fuse_argv[i - 2] = argv[i];
    }

    ret = fuse_main(fuse_argc, fuse_argv, &unionfs_oper, state);

    free(fuse_argv);
    free(state->lower_dir);
    free(state->upper_dir);
    free(state);

    return ret;
}
