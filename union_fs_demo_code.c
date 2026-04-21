/*
 * Mini UnionFS - A simple union filesystem using FUSE
 * 
 * This implements a copy-on-write union filesystem that overlays two directories:
 * - lower_dir: read-only base layer (original files)
 * - upper_dir: writable overlay layer (all changes go here)
 * 
 * Key concepts:
 * - Whiteout files (.wh.filename): mark files as deleted from lower layer
 * - Opaque marker (.wh..wh..opq): mark directory as opaque (hide lower layer contents)
 * - Copy-up: when modifying a lower file, it gets copied to upper layer first
 */

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

// Magic strings for unionfs metadata files
#define WHITEOUT_PREFIX ".wh."      // Prefix for whiteout files (deletion markers)
#define OPAQUE_MARKER ".wh..wh..opq" // File that hides lower directory contents
#define COPY_BUFFER_SIZE 65536       // 64KB buffer for file copy operations

// Runtime state for our filesystem - holds paths to the two layers
struct mini_unionfs_state {
    char *lower_dir;  // Read-only base directory
    char *upper_dir;  // Writable overlay directory
};

// Macro to get our state from FUSE context
#define UNIONFS_DATA ((struct mini_unionfs_state *)fuse_get_context()->private_data)

// Tracks where a file was found (upper layer, lower layer, or not found)
enum resolved_source {
    RESOLVED_NONE = 0,
    RESOLVED_UPPER = 1,
    RESOLVED_LOWER = 2,
};

// Complete information about a path lookup across both layers
struct path_lookup {
    char upper[PATH_MAX];      // Full path in upper directory
    char lower[PATH_MAX];      // Full path in lower directory
    char whiteout[PATH_MAX];   // Path to potential whiteout file
    int hidden;                // Whether path is whiteouted
    int upper_exists;          // Does file exist in upper layer?
    int lower_exists;          // Does file exist in lower layer?
    int whiteout_exists;       // Does whiteout file exist?
    struct stat upper_st;      // Stat info for upper file
    struct stat lower_st;      // Stat info for lower file
};

// Dynamic list of names - used for directory listing merging
struct name_list {
    char **items;     // Array of name strings
    size_t count;     // Number of items currently stored
    size_t capacity;  // Total allocated capacity
};

// Forward declarations of all functions
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

/*
 * Safe snprintf that checks for truncation errors.
 * Returns 0 on success, -ENAMETOOLONG if output was truncated.
 */
static int checked_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    int written;

    va_start(args, fmt);
    written = vsnprintf(buf, size, fmt, args);
    va_end(args);

    // Check if output was truncated (written >= size) or error occurred
    if (written < 0 || (size_t)written >= size) {
        return -ENAMETOOLONG;
    }

    return 0;
}

/*
 * Does lstat() but handles ENOENT gracefully.
 * Returns: 1 if file exists, 0 if not found, negative errno on error.
 */
static int lstat_optional(const char *path, struct stat *stbuf)
{
    struct stat local_st;

    // Try to stat the file, using local buffer if caller didn't provide one
    if (lstat(path, stbuf != NULL ? stbuf : &local_st) == 0) {
        return 1;  // File exists
    }

    // File not found - this is expected, not an error
    if (errno == ENOENT || errno == ENOTDIR) {
        return 0;
    }

    // Some other error occurred
    return -errno;
}

/*
 * Builds the actual filesystem path by combining root directory and union path.
 * Example: build_host_path("/upper", "/foo/bar") -> "/upper/foo/bar"
 */
static int build_host_path(const char *root, const char *path, char *out, size_t out_size)
{
    // Special case: root path "/" maps to the root directory itself
    if (strcmp(path, "/") == 0) {
        return checked_snprintf(out, out_size, "%s", root);
    }

    // Normal case: concatenate root and path
    return checked_snprintf(out, out_size, "%s%s", root, path);
}

/*
 * Extracts the next component from a path string.
 * Example: cursor="/foo/bar" -> component="foo", cursor advanced to "bar"
 * Returns: 1 if component extracted, 0 if at end, negative on error.
 */
static int next_path_component(const char **cursor, char *component, size_t component_size)
{
    const char *start = *cursor;
    const char *end;
    size_t length;

    // Skip any leading slashes
    while (*start == '/') {
        start++;
    }

    // Check if we've reached the end
    if (*start == '\0') {
        *cursor = start;
        return 0;
    }

    // Find the end of this component (next slash or end of string)
    end = start;
    while (*end != '\0' && *end != '/') {
        end++;
    }

    // Check if component fits in buffer
    length = (size_t)(end - start);
    if (length + 1 > component_size) {
        return -ENAMETOOLONG;
    }

    // Copy the component and null-terminate
    memcpy(component, start, length);
    component[length] = '\0';

    // Skip any slashes after the component
    while (*end == '/') {
        end++;
    }

    *cursor = end;  // Update cursor for next call
    return 1;
}

/*
 * Appends a name to a base path to form a new union path.
 * Example: append_union_component("/foo", "bar") -> "/foo/bar"
 */
static int append_union_component(const char *base, const char *name, char *out, size_t out_size)
{
    // Handle empty or root base specially to avoid double slashes
    if (base[0] == '\0' || strcmp(base, "/") == 0) {
        return checked_snprintf(out, out_size, "/%s", name);
    }

    // Normal case: add slash and name
    return checked_snprintf(out, out_size, "%s/%s", base, name);
}

/*
 * Splits a union path into parent directory and base name.
 * Example: "/foo/bar/baz" -> parent="/foo/bar", name="baz"
 */
static int split_union_path(
    const char *path,
    char *parent,
    size_t parent_size,
    char *name,
    size_t name_size)
{
    const char *slash;
    size_t parent_length;

    // Can't split the root path
    if (strcmp(path, "/") == 0) {
        return -EINVAL;
    }

    // Find the last slash
    slash = strrchr(path, '/');
    if (slash == NULL || slash[1] == '\0') {
        return -EINVAL;  // No slash or trailing slash
    }

    // Extract parent path
    if (slash == path) {
        // Parent is root
        if (checked_snprintf(parent, parent_size, "/") < 0) {
            return -ENAMETOOLONG;
        }
    } else {
        // Parent is everything before last slash
        parent_length = (size_t)(slash - path);
        if (parent_length + 1 > parent_size) {
            return -ENAMETOOLONG;
        }
        memcpy(parent, path, parent_length);
        parent[parent_length] = '\0';
    }

    // Extract base name (everything after last slash)
    if (strlen(slash + 1) + 1 > name_size) {
        return -ENAMETOOLONG;
    }
    strcpy(name, slash + 1);

    return 0;
}

/*
 * Builds the path to a whiteout file for a given union path.
 * Whiteout files are named ".wh.<filename>" and live in the parent directory.
 */
static int build_whiteout_path(const char *path, char *out, size_t out_size)
{
    char parent[PATH_MAX];
    char name[NAME_MAX + 1];
    char parent_host[PATH_MAX];
    int ret;

    // Root can't have a whiteout
    if (strcmp(path, "/") == 0) {
        return -EINVAL;
    }

    // Split path into parent and name
    ret = split_union_path(path, parent, sizeof(parent), name, sizeof(name));
    if (ret < 0) {
        return ret;
    }

    // Get the actual host path for the parent directory
    ret = build_host_path(UNIONFS_DATA->upper_dir, parent, parent_host, sizeof(parent_host));
    if (ret < 0) {
        return ret;
    }

    // Whiteout file is in upper/parent/.wh.name
    return checked_snprintf(out, out_size, "%s/%s%s", parent_host, WHITEOUT_PREFIX, name);
}

/*
 * Builds the path to an opaque marker for a directory.
 * Opaque markers are named ".wh..wh..opq" and live inside the directory.
 */
static int build_opaque_path(const char *path, char *out, size_t out_size)
{
    char upper_path[PATH_MAX];
    int ret;

    // Get the actual host path for this directory
    ret = build_host_path(UNIONFS_DATA->upper_dir, path, upper_path, sizeof(upper_path));
    if (ret < 0) {
        return ret;
    }

    // Opaque marker lives inside the directory
    return checked_snprintf(out, out_size, "%s/%s", upper_path, OPAQUE_MARKER);
}

/*
 * Checks if a path is whiteouted (deleted) by walking up the tree.
 * A file is whiteouted if any parent directory contains a whiteout for it.
 * Returns: 1 if whiteouted, 0 if not, negative on error.
 */
static int is_whiteouted(const char *path)
{
    char component[NAME_MAX + 1];
    char current[PATH_MAX] = "";
    char next[PATH_MAX];
    char whiteout[PATH_MAX];
    const char *cursor = path;
    int step;
    int ret;

    // Root is never whiteouted
    if (strcmp(path, "/") == 0) {
        return 0;
    }

    // Walk down the path component by component
    for (;;) {
        step = next_path_component(&cursor, component, sizeof(component));
        if (step < 0) {
            return step;
        }
        if (step == 0) {
            break;  // Reached end of path
        }

        // Build the path so far
        ret = append_union_component(current, component, next, sizeof(next));
        if (ret < 0) {
            return ret;
        }

        // Check if there's a whiteout for this component
        ret = build_whiteout_path(next, whiteout, sizeof(whiteout));
        if (ret == 0) {
            step = lstat_optional(whiteout, NULL);
            if (step < 0) {
                return step;
            }
            if (step == 1) {
                return 1;  // Found a whiteout - path is deleted
            }
        }

        // Update current path and continue
        ret = checked_snprintf(current, sizeof(current), "%s", next);
        if (ret < 0) {
            return ret;
        }
    }

    return 0;  // No whiteouts found
}

/*
 * Checks if a directory has an opaque marker.
 * Returns: 1 if opaque, 0 if not, negative on error.
 */
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

/*
 * Checks if any parent directory (or self) is opaque.
 * An opaque directory hides all contents from lower layers.
 * If include_self is true, checks the directory itself too.
 */
static int path_has_opaque_prefix(const char *path, bool include_self)
{
    char component[NAME_MAX + 1];
    char current[PATH_MAX] = "";
    char next[PATH_MAX];
    const char *cursor = path;
    int step;
    int ret;

    // Root can't be opaque
    if (strcmp(path, "/") == 0) {
        return 0;
    }

    // Walk down the path component by component
    for (;;) {
        step = next_path_component(&cursor, component, sizeof(component));
        if (step < 0) {
            return step;
        }
        if (step == 0) {
            break;
        }

        // Build the path so far
        ret = append_union_component(current, component, next, sizeof(next));
        if (ret < 0) {
            return ret;
        }

        // Check for opaque marker - either on parents or (if requested) on self
        if (*cursor != '\0' || include_self) {
            ret = is_opaque_directory(next);
            if (ret < 0) {
                return ret;
            }
            if (ret == 1) {
                return 1;  // Found an opaque marker
            }
        }

        // Update current path and continue
        ret = checked_snprintf(current, sizeof(current), "%s", next);
        if (ret < 0) {
            return ret;
        }
    }

    return 0;  // No opaque markers found
}

/*
 * Performs a complete lookup of a path across both layers.
 * Fills a path_lookup struct with all information about the file.
 */
static int lookup_path(const char *path, struct path_lookup *lookup)
{
    int ret;

    // Initialize all fields to zero/false
    memset(lookup, 0, sizeof(*lookup));

    // Build host paths for both layers
    ret = build_host_path(UNIONFS_DATA->upper_dir, path, lookup->upper, sizeof(lookup->upper));
    if (ret < 0) {
        return ret;
    }

    ret = build_host_path(UNIONFS_DATA->lower_dir, path, lookup->lower, sizeof(lookup->lower));
    if (ret < 0) {
        return ret;
    }

    // Check for whiteout (except for root)
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

    // Check if path is whiteouted by any parent
    ret = is_whiteouted(path);
    if (ret < 0) {
        return ret;
    }
    lookup->hidden = ret;

    // Check existence in both layers
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

/*
 * Resolves a union path to an actual host filesystem path.
 * This is the core path resolution function that decides which layer to use.
 * Upper layer takes precedence over lower layer.
 * Respects whiteouts and opaque markers.
 */
static int resolve_path(
    const char *path,
    char *resolved_path,
    size_t resolved_path_size,
    enum resolved_source *source)
{
    struct path_lookup lookup;
    int ret;

    // Look up the path in both layers
    ret = lookup_path(path, &lookup);
    if (ret < 0) {
        return ret;
    }

    // If path is whiteouted, it doesn't exist
    if (lookup.hidden) {
        return -ENOENT;
    }

    // Upper layer has precedence
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

    // Lower layer only if not hidden by opaque marker
    if (lookup.lower_exists) {
        ret = path_has_opaque_prefix(path, false);
        if (ret < 0) {
            return ret;
        }
        if (ret == 0) {  // Not blocked by opaque marker
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

    // File not found in either layer
    return -ENOENT;
}

/*
 * Frees all memory used by a name_list.
 */
static void name_list_destroy(struct name_list *list)
{
    size_t i;

    // Free each individual string
    for (i = 0; i < list->count; ++i) {
        free(list->items[i]);
    }

    // Free the array itself
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

/*
 * Checks if a name is already in the list.
 */
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

/*
 * Adds a name to the list if it's not already present.
 * Returns: 1 if added, 0 if already present, negative on error.
 */
static int name_list_add_unique(struct name_list *list, const char *name)
{
    char **new_items;

    // Don't add duplicates
    if (name_list_contains(list, name)) {
        return 0;
    }

    // Grow the array if needed (double capacity when full)
    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        new_items = realloc(list->items, new_capacity * sizeof(char *));
        if (new_items == NULL) {
            return -ENOMEM;
        }

        list->items = new_items;
        list->capacity = new_capacity;
    }

    // Duplicate the string and store it
    list->items[list->count] = strdup(name);
    if (list->items[list->count] == NULL) {
        return -ENOMEM;
    }

    list->count++;
    return 1;
}

/*
 * Determines if opening a file with these flags requires copy-up.
 * Write access or truncation requires the file to be in upper layer.
 */
static bool open_requires_copy_up(int flags)
{
    // O_RDONLY doesn't need copy-up, but O_WRONLY, O_RDWR, or O_TRUNC do
    return ((flags & O_ACCMODE) != O_RDONLY) || (flags & O_TRUNC);
}

/*
 * Ensures all parent directories of a path exist in the upper layer.
 * Copies up directory structure from lower layer if needed.
 * This is called before creating files in the upper layer.
 */
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

    // Get the parent directory path
    ret = split_union_path(path, parent, sizeof(parent), name, sizeof(name));
    if (ret < 0) {
        return ret;
    }

    // If parent is root, nothing to ensure
    if (strcmp(parent, "/") == 0) {
        return 0;
    }

    // Walk down the parent path component by component
    cursor = parent;
    for (;;) {
        step = next_path_component(&cursor, component, sizeof(component));
        if (step < 0) {
            return step;
        }
        if (step == 0) {
            break;
        }

        // Build path to this component
        ret = append_union_component(current, component, next, sizeof(next));
        if (ret < 0) {
            return ret;
        }

        // Resolve where this directory currently exists
        ret = resolve_path(next, resolved, sizeof(resolved), &source);
        if (ret < 0) {
            return ret;
        }

        // Verify it's actually a directory
        if (lstat(resolved, &st) == -1) {
            return -errno;
        }
        if (!S_ISDIR(st.st_mode)) {
            return -ENOTDIR;
        }

        // If it's in the lower layer, copy it up to upper layer
        if (source == RESOLVED_LOWER) {
            ret = build_host_path(UNIONFS_DATA->upper_dir, next, upper_path, sizeof(upper_path));
            if (ret < 0) {
                return ret;
            }

            // Create the directory in upper layer with same permissions
            if (mkdir(upper_path, st.st_mode & 0777) == -1 && errno != EEXIST) {
                return -errno;
            }
        }

        // Update current path and continue
        ret = checked_snprintf(current, sizeof(current), "%s", next);
        if (ret < 0) {
            return ret;
        }
    }

    return 0;
}

/*
 * Removes a whiteout file for a given path.
 * Called when a previously deleted file is being recreated.
 */
static int remove_whiteout(const char *path)
{
    char whiteout[PATH_MAX];
    int ret;

    // Root has no whiteout
    if (strcmp(path, "/") == 0) {
        return 0;
    }

    ret = build_whiteout_path(path, whiteout, sizeof(whiteout));
    if (ret < 0) {
        return ret;
    }

    // Try to unlink - ENOENT is fine (no whiteout existed)
    if (unlink(whiteout) == -1 && errno != ENOENT) {
        return -errno;
    }

    return 0;
}

/*
 * Creates a whiteout file to mark a path as deleted.
 * This hides any file in the lower layer with the same name.
 */
static int create_whiteout(const char *path)
{
    char whiteout[PATH_MAX];
    int fd;
    int ret;

    // Can't whiteout root
    if (strcmp(path, "/") == 0) {
        return -EPERM;
    }

    // Ensure parent directories exist in upper layer
    ret = ensure_upper_parents(path);
    if (ret < 0) {
        return ret;
    }

    ret = build_whiteout_path(path, whiteout, sizeof(whiteout));
    if (ret < 0) {
        return ret;
    }

    // Create the whiteout file exclusively
    fd = open(whiteout, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd == -1) {
        if (errno == EEXIST) {
            return 0;  // Whiteout already exists - that's fine
        }
        return -errno;
    }

    // Close immediately - whiteout files are empty
    if (close(fd) == -1) {
        return -errno;
    }

    return 0;
}

/*
 * Creates an opaque marker in a directory.
 * This hides all entries from lower layer within this directory.
 */
static int create_opaque_marker(const char *path)
{
    char opaque_path[PATH_MAX];
    int fd;
    int ret;

    ret = build_opaque_path(path, opaque_path, sizeof(opaque_path));
    if (ret < 0) {
        return ret;
    }

    // Create (or truncate) the opaque marker file
    fd = open(opaque_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd == -1) {
        return -errno;
    }

    if (close(fd) == -1) {
        return -errno;
    }

    return 0;
}

/*
 * Copies file contents from one fd to another.
 * Used during copy-up to copy file data from lower to upper layer.
 */
static int copy_file_contents(int from_fd, int to_fd)
{
    char buffer[COPY_BUFFER_SIZE];

    // Read and write in chunks until EOF
    for (;;) {
        ssize_t bytes_read = read(from_fd, buffer, sizeof(buffer));
        ssize_t written_total = 0;

        if (bytes_read == 0) {
            return 0;  // EOF - success
        }
        if (bytes_read < 0) {
            return -errno;
        }

        // Write all bytes read (handle partial writes)
        while (written_total < bytes_read) {
            ssize_t bytes_written = write(to_fd, buffer + written_total, (size_t)(bytes_read - written_total));
            if (bytes_written < 0) {
                return -errno;
            }
            written_total += bytes_written;
        }
    }
}

/*
 * Copies a file from the lower layer to the upper layer.
 * Preserves file permissions, ownership, and timestamps.
 * This is called before modifying a file that exists only in lower layer.
 */
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

    // Find where the file currently is
    ret = resolve_path(path, resolved, sizeof(resolved), &source);
    if (ret < 0) {
        return ret;
    }

    // Already in upper layer - nothing to do
    if (source == RESOLVED_UPPER) {
        return 0;
    }

    // Get file metadata from lower layer
    if (lstat(resolved, &lower_st) == -1) {
        return -errno;
    }

    // Only regular files can be copied up this way
    if (!S_ISREG(lower_st.st_mode)) {
        if (S_ISDIR(lower_st.st_mode)) {
            return -EISDIR;
        }
        return -EINVAL;
    }

    // Ensure parent directories exist in upper layer
    ret = ensure_upper_parents(path);
    if (ret < 0) {
        return ret;
    }

    // Build path in upper layer
    ret = build_host_path(UNIONFS_DATA->upper_dir, path, upper_path, sizeof(upper_path));
    if (ret < 0) {
        return ret;
    }

    // Open source file (lower layer) for reading
    from_fd = open(resolved, O_RDONLY);
    if (from_fd == -1) {
        return -errno;
    }

    // Create destination file (upper layer) with same permissions
    to_fd = open(upper_path, O_WRONLY | O_CREAT | O_EXCL, lower_st.st_mode & 0777);
    if (to_fd == -1) {
        ret = (errno == EEXIST) ? 0 : -errno;  // Already exists is OK
        close(from_fd);
        return ret;
    }

    // Copy the actual file contents
    ret = copy_file_contents(from_fd, to_fd);
    if (ret < 0) {
        close(from_fd);
        close(to_fd);
        unlink(upper_path);  // Clean up on error
        return ret;
    }

    // Set file permissions (including setuid/setgid/sticky bits)
    if (fchmod(to_fd, lower_st.st_mode & 07777) == -1) {
        ret = -errno;
        close(from_fd);
        close(to_fd);
        unlink(upper_path);
        return ret;
    }

    // Preserve access and modification times
    times[0] = lower_st.st_atim;
    times[1] = lower_st.st_mtim;
    if (futimens(to_fd, times) == -1) {
        ret = -errno;
        close(from_fd);
        close(to_fd);
        unlink(upper_path);
        return ret;
    }

    // Close both files
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

/*
 * Collects all directory entries from both layers, merging them.
 * Handles whiteouts (hides deleted files) and opaque markers (hides entire lower dir).
 * Returns a name_list with all visible entries.
 */
static int collect_merged_dir_entries(const char *path, struct name_list *entries)
{
    struct path_lookup lookup;
    struct name_list whiteouts = {0};  // Track whiteouted names in this directory
    char resolved[PATH_MAX];
    enum resolved_source source;
    struct stat visible_st;
    DIR *dir = NULL;
    struct dirent *entry;
    int use_upper;
    int use_lower;
    int lower_blocked;
    int ret;

    // Initialize empty entries list
    memset(entries, 0, sizeof(*entries));

    // Resolve the directory to read its metadata
    ret = resolve_path(path, resolved, sizeof(resolved), &source);
    if (ret < 0) {
        return ret;
    }

    // Verify it's actually a directory
    if (lstat(resolved, &visible_st) == -1) {
        return -errno;
    }
    if (!S_ISDIR(visible_st.st_mode)) {
        return -ENOTDIR;
    }

    // Look up both layers
    ret = lookup_path(path, &lookup);
    if (ret < 0) {
        return ret;
    }

    // Check if lower layer is blocked by an opaque marker
    lower_blocked = path_has_opaque_prefix(path, true);
    if (lower_blocked < 0) {
        return lower_blocked;
    }

    // Determine which layers to read from
    use_upper = lookup.upper_exists && S_ISDIR(lookup.upper_st.st_mode);
    use_lower = lookup.lower_exists && S_ISDIR(lookup.lower_st.st_mode) && !lower_blocked;

    // Read upper layer first (it has precedence)
    if (use_upper) {
        dir = opendir(lookup.upper);
        if (dir == NULL) {
            ret = -errno;
            goto cleanup;
        }

        while ((entry = readdir(dir)) != NULL) {
            // Skip . and ..
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            // Skip opaque marker (it's metadata, not a real file)
            if (strcmp(entry->d_name, OPAQUE_MARKER) == 0) {
                continue;
            }

            // Track whiteout files (they hide lower layer entries)
            if (strncmp(entry->d_name, WHITEOUT_PREFIX, strlen(WHITEOUT_PREFIX)) == 0) {
                // Extract the actual filename (after the prefix)
                ret = name_list_add_unique(&whiteouts, entry->d_name + strlen(WHITEOUT_PREFIX));
                if (ret < 0) {
                    closedir(dir);
                    goto cleanup;
                }
                continue;
            }

            // Normal file - add to entries
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

    // Read lower layer for files not in upper and not whiteouted
    if (use_lower) {
        dir = opendir(lookup.lower);
        if (dir == NULL) {
            ret = -errno;
            goto cleanup;
        }

        while ((entry = readdir(dir)) != NULL) {
            // Skip . and ..
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            // Skip if already in entries or whiteouted
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

/*
 * Checks if a directory appears empty in the merged view.
 * Returns: 1 if empty, 0 if not, negative on error.
 */
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

/*
 * Removes unionfs metadata files (whiteouts and opaque markers) from a directory.
 * Used when deleting an upper-layer directory to clean up before rmdir.
 * Fails if the directory contains any real files (ENOTEMPTY).
 */
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

        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        ret = checked_snprintf(child, sizeof(child), "%s/%s", upper_dir_path, entry->d_name);
        if (ret < 0) {
            closedir(dir);
            return ret;
        }

        // Remove whiteout files and opaque markers
        if (strcmp(entry->d_name, OPAQUE_MARKER) == 0 ||
            strncmp(entry->d_name, WHITEOUT_PREFIX, strlen(WHITEOUT_PREFIX)) == 0) {
            if (unlink(child) == -1) {
                ret = -errno;
                closedir(dir);
                return ret;
            }
            continue;
        }

        // Found a real file - directory is not empty
        closedir(dir);
        return -ENOTEMPTY;
    }

    if (closedir(dir) == -1) {
        return -errno;
    }

    return 0;
}

/*
 * FUSE init callback. Called when filesystem is mounted.
 * Disables kernel caching to ensure consistent view of both layers.
 */
static void *unionfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;

    // Disable all caching - we need fresh view each time
    cfg->kernel_cache = 0;
    cfg->entry_timeout = 0;
    cfg->attr_timeout = 0;
    cfg->negative_timeout = 0;
    cfg->use_ino = 1;  // Use our own inode numbers

    return UNIONFS_DATA;
}

/*
 * FUSE getattr callback. Returns file attributes (stat info).
 */
static int unionfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
    char resolved[PATH_MAX];
    int ret;

    (void)fi;
    memset(stbuf, 0, sizeof(*stbuf));

    // Find the actual file
    ret = resolve_path(path, resolved, sizeof(resolved), NULL);
    if (ret < 0) {
        return ret;
    }

    // Get its attributes
    if (lstat(resolved, stbuf) == -1) {
        return -errno;
    }

    return 0;
}

/*
 * FUSE statfs callback. Returns filesystem statistics.
 * We report stats from the upper layer (where writes go).
 */
static int unionfs_statfs(const char *path, struct statvfs *stbuf)
{
    (void)path;

    if (statvfs(UNIONFS_DATA->upper_dir, stbuf) == -1) {
        return -errno;
    }

    return 0;
}

/*
 * FUSE readdir callback. Lists contents of a directory.
 * Merges entries from both layers, handling whiteouts and opaque markers.
 */
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

    // Get merged list of directory entries
    ret = collect_merged_dir_entries(path, &entries);
    if (ret < 0) {
        return ret;
    }

    // Always include . and ..
    if (filler(buf, ".", NULL, 0, 0) != 0 || filler(buf, "..", NULL, 0, 0) != 0) {
        name_list_destroy(&entries);
        return 0;
    }

    // Add all merged entries
    for (i = 0; i < entries.count; ++i) {
        if (filler(buf, entries.items[i], NULL, 0, 0) != 0) {
            break;  // Buffer full
        }
    }

    name_list_destroy(&entries);
    return 0;
}

/*
 * FUSE open callback. Opens a file.
 * Copies up from lower layer if opening for write.
 */
static int unionfs_open(const char *path, struct fuse_file_info *fi)
{
    char resolved[PATH_MAX];
    struct stat st;
    int open_flags;
    int fd;
    int ret;

    // Copy up if opening for write or truncate
    if (open_requires_copy_up(fi->flags)) {
        ret = copy_up_file(path);
        if (ret < 0) {
            return ret;
        }
    }

    // Resolve to actual file (now in upper layer if copied up)
    ret = resolve_path(path, resolved, sizeof(resolved), NULL);
    if (ret < 0) {
        return ret;
    }

    // Can't open directories with open()
    if (lstat(resolved, &st) == -1) {
        return -errno;
    }
    if (S_ISDIR(st.st_mode)) {
        return -EISDIR;
    }

    // Remove O_CREAT and O_EXCL flags (we already handled creation if needed)
    open_flags = fi->flags & ~(O_CREAT | O_EXCL);
    fd = open(resolved, open_flags);
    if (fd == -1) {
        return -errno;
    }

    // Store fd for later read/write operations
    fi->fh = (uint64_t)fd;
    return 0;
}

/*
 * FUSE create callback. Creates a new file.
 * Always creates in the upper layer.
 */
static int unionfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    struct path_lookup lookup;
    int fd;
    int ret;

    // Check if file already exists (shouldn't for create)
    ret = resolve_path(path, NULL, 0, NULL);
    if (ret == 0) {
        return -EEXIST;
    }
    if (ret != -ENOENT) {
        return ret;
    }

    // Get upper layer path
    ret = lookup_path(path, &lookup);
    if (ret < 0) {
        return ret;
    }

    // Ensure parent directories exist in upper layer
    ret = ensure_upper_parents(path);
    if (ret < 0) {
        return ret;
    }

    // Create the file in upper layer
    fd = open(lookup.upper, fi->flags | O_CREAT, mode);
    if (fd == -1) {
        return -errno;
    }

    // Remove any whiteout that might have existed
    ret = remove_whiteout(path);
    if (ret < 0) {
        close(fd);
        unlink(lookup.upper);  // Clean up
        return ret;
    }

    fi->fh = (uint64_t)fd;
    return 0;
}

/*
 * FUSE read callback. Reads data from a file.
 */
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

    // Use existing fd if available
    if (fi != NULL) {
        fd = (int)fi->fh;
    } else {
        // Fallback: open the file directly
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

    // Read at the given offset
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

/*
 * FUSE write callback. Writes data to a file.
 * Ensures file is in upper layer first.
 */
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

    // Use existing fd if available
    if (fi != NULL) {
        fd = (int)fi->fh;
    } else {
        // Fallback: copy up and open directly
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

    // Write at the given offset
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

/*
 * FUSE truncate callback. Changes file size.
 * Ensures file is in upper layer first.
 */
static int unionfs_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    char resolved[PATH_MAX];
    int ret;

    // Copy up if file is in lower layer
    ret = copy_up_file(path);
    if (ret < 0 && ret != -EISDIR) {
        return ret;
    }

    ret = resolve_path(path, resolved, sizeof(resolved), NULL);
    if (ret < 0) {
        return ret;
    }

    // Truncate using fd if available
    if (fi != NULL) {
        if (ftruncate((int)fi->fh, size) == -1) {
            return -errno;
        }
        return 0;
    }

    // Otherwise truncate by path
    if (truncate(resolved, size) == -1) {
        return -errno;
    }

    return 0;
}

/*
 * FUSE release callback. Closes an open file.
 */
static int unionfs_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;

    if (close((int)fi->fh) == -1) {
        return -errno;
    }

    return 0;
}

/*
 * FUSE unlink callback. Deletes a file.
 * If file is in upper layer: delete it, create whiteout if lower layer has it.
 * If file is in lower layer: create whiteout to hide it.
 */
static int unionfs_unlink(const char *path)
{
    struct path_lookup lookup;
    char resolved[PATH_MAX];
    enum resolved_source source;
    struct stat st;
    int lower_blocked;
    int ret;

    // Find the file
    ret = resolve_path(path, resolved, sizeof(resolved), &source);
    if (ret < 0) {
        return ret;
    }

    // Can't unlink directories with this function
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
        // Delete the actual file from upper layer
        if (unlink(lookup.upper) == -1) {
            return -errno;
        }

        // If file exists in lower layer, create whiteout to hide it
        if (lookup.lower_exists && !lower_blocked) {
            ret = create_whiteout(path);
            if (ret < 0) {
                return ret;
            }
        }

        return 0;
    }

    if (source == RESOLVED_LOWER) {
        // File is in lower layer - create whiteout to hide it
        return create_whiteout(path);
    }

    return -ENOENT;
}

/*
 * FUSE mkdir callback. Creates a new directory.
 * Always creates in upper layer.
 */
static int unionfs_mkdir(const char *path, mode_t mode)
{
    struct path_lookup lookup;
    int needs_opaque;
    int ret;

    // Check if directory already exists
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

    // Ensure parent directories exist in upper layer
    ret = ensure_upper_parents(path);
    if (ret < 0) {
        return ret;
    }

    // Create the directory
    if (mkdir(lookup.upper, mode) == -1) {
        return -errno;
    }

    // If there was a whiteout and lower layer has a directory here,
    // we need an opaque marker to hide lower layer contents
    needs_opaque = lookup.whiteout_exists && lookup.lower_exists && S_ISDIR(lookup.lower_st.st_mode);
    if (needs_opaque) {
        ret = create_opaque_marker(path);
        if (ret < 0) {
            rmdir(lookup.upper);  // Clean up
            return ret;
        }
    }

    // Remove any whiteout file
    ret = remove_whiteout(path);
    if (ret < 0) {
        purge_upper_metadata(lookup.upper);
        rmdir(lookup.upper);
        return ret;
    }

    return 0;
}

/*
 * FUSE rmdir callback. Removes a directory.
 * If directory is in upper layer: delete it, create whiteout if lower layer has it.
 * If directory is in lower layer: create whiteout to hide it.
 * Only works on empty directories.
 */
static int unionfs_rmdir(const char *path)
{
    struct path_lookup lookup;
    char resolved[PATH_MAX];
    enum resolved_source source;
    struct stat st;
    int empty;
    int lower_blocked;
    int ret;

    // Can't remove root
    if (strcmp(path, "/") == 0) {
        return -EBUSY;
    }

    // Find the directory
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

    // Check if directory is empty in merged view
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
        // Clean up metadata files (whiteouts, opaque markers) first
        ret = purge_upper_metadata(lookup.upper);
        if (ret < 0) {
            return ret;
        }
        // Remove the directory itself
        if (rmdir(lookup.upper) == -1) {
            return -errno;
        }

        // If directory exists in lower layer, create whiteout to hide it
        if (lookup.lower_exists && !lower_blocked) {
            ret = create_whiteout(path);
            if (ret < 0) {
                return ret;
            }
        }

        return 0;
    }

    if (source == RESOLVED_LOWER) {
        // Directory is in lower layer - create whiteout to hide it
        return create_whiteout(path);
    }

    return -ENOENT;
}

/*
 * FUSE operations structure - maps our functions to FUSE callbacks.
 */
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

/*
 * Prints usage information.
 */
static void print_usage(const char *program_name)
{
    fprintf(
        stderr,
        "Usage: %s <lower_dir> <upper_dir> <mountpoint> [FUSE options...]\n",
        program_name);
}

/*
 * Validates that a path exists and is a directory.
 */
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

/*
 * Main entry point.
 * Parses arguments, initializes state, and starts FUSE.
 */
int main(int argc, char *argv[])
{
    struct mini_unionfs_state *state;
    char **fuse_argv;
    int fuse_argc;
    int i;
    int ret;

    // Need at least: program name + lower + upper + mountpoint
    if (argc < 4) {
        print_usage(argv[0]);
        return 1;
    }

    // Allocate our filesystem state
    state = calloc(1, sizeof(*state));
    if (state == NULL) {
        fprintf(stderr, "Failed to allocate filesystem state.\n");
        return 1;
    }

    // Validate input directories
    if (validate_directory(argv[1], "Lower directory") != 0 ||
        validate_directory(argv[2], "Upper directory") != 0) {
        free(state);
        return 1;
    }

    // Get canonical paths for both directories
    state->lower_dir = realpath(argv[1], NULL);
    state->upper_dir = realpath(argv[2], NULL);
    if (state->lower_dir == NULL || state->upper_dir == NULL) {
        fprintf(stderr, "Failed to canonicalize branch directories: %s\n", strerror(errno));
        free(state->lower_dir);
        free(state->upper_dir);
        free(state);
        return 1;
    }

    // Build argv for FUSE (skip lower_dir and upper_dir)
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

    // Start FUSE with our operations
    ret = fuse_main(fuse_argc, fuse_argv, &unionfs_oper, state);

    // Clean up
    free(fuse_argv);
    free(state->lower_dir);
    free(state->upper_dir);
    free(state);

    return ret;
}
