#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>


//write declaration 
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── Constants ───────────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED FUNCTIONS ──────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

//parsing the tree
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1;
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';

        ptr = null_byte + 1;
        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    // FIX: Increased to 301 to accommodate max name (256) + mode + null + hash (32)
    size_t max_size = tree->count * 301; 
    if (max_size == 0) max_size = 1; // Prevent malloc(0)
    
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 for the null terminator
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── IMPLEMENTED RECURSION ───────────────────────────────────────────────────

static int write_tree_recursive(IndexEntry **entries, int count, int depth, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    for (int i = 0; i < count; ) {
        const char *path = entries[i]->path;
        const char *p = path;
        
        // Advance pointer to current directory level
        for (int d = 0; d < depth; d++) {
            p = strchr(p, '/');
            if (!p) return -1;
            p++; 
        }

        const char *slash = strchr(p, '/');

        if (!slash) {
            // It's a file at this level
            if (tree.count >= MAX_TREE_ENTRIES) return -1;
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i]->mode;
            te->hash = entries[i]->hash;
            strncpy(te->name, p, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            i++;
        } else {
            // It's a subdirectory
            size_t dir_name_len = slash - p;
            char dir_name[256];
            if (dir_name_len >= sizeof(dir_name)) return -1;
            memcpy(dir_name, p, dir_name_len);
            dir_name[dir_name_len] = '\0';

            // Group entries for recursion
            int j = i;
            while (j < count) {
                const char *pp = entries[j]->path;
                for (int d = 0; d < depth; d++) {
                    pp = strchr(pp, '/');
                    if (!pp) break;
                    pp++;
                }
                if (!pp) break;
                const char *sl = strchr(pp, '/');
                if (!sl) break;
                
                size_t len = sl - pp;
                if (len != dir_name_len || strncmp(pp, dir_name, len) != 0)
                    break;
                j++;
            }

            ObjectID subtree_id;
            if (write_tree_recursive(&entries[i], j - i, depth + 1, &subtree_id) != 0)
                return -1;

            if (tree.count >= MAX_TREE_ENTRIES) return -1;
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = MODE_DIR;
            te->hash = subtree_id;
            strncpy(te->name, dir_name, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';

            i = j;
        }
    }

    void *tree_data;
    size_t tree_len;
    if (tree_serialize(&tree, &tree_data, &tree_len) != 0) return -1;

    int ret = object_write(OBJ_TREE, tree_data, tree_len, id_out);
    free(tree_data);
    return ret;
}

// Helper for qsort 
static int compare_index_pointers(const void *a, const void *b) {
    return strcmp((*(IndexEntry **)a)->path, (*(IndexEntry **)b)->path);
}

int tree_from_index(ObjectID *id_out) {
    Index index;
    if (index_load(&index) != 0) return -1;

    if (index.count == 0) {
        Tree empty = { .count = 0 };
        void *data; size_t len;
        if (tree_serialize(&empty, &data, &len) != 0) return -1;
        int ret = object_write(OBJ_TREE, data, len, id_out);
        free(data);
        return ret;
    }

    IndexEntry **entries = malloc(index.count * sizeof(IndexEntry *));
    if (!entries) return -1;
    for (int i = 0; i < index.count; i++)
        entries[i] = &index.entries[i];

    // FIX: Using qsort instead of insertion sort for reliability/speed
    qsort(entries, index.count, sizeof(IndexEntry *), compare_index_pointers);

    int ret = write_tree_recursive(entries, index.count, 0, id_out);
    free(entries);
    return ret;
}
