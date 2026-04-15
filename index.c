//staging
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#ifndef INDEX_PATH
#define INDEX_PATH ".pes/index"
#endif

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);



IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) return &index->entries[i];
    }
    return NULL;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    for (int i = 0; i < index->count; i++) printf("  staged:     %s\n", index->entries[i].path);
    if (index->count == 0) printf("  (nothing to show)\n");
    return 0;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            memmove(&index->entries[i], &index->entries[i+1], (index->count-i-1)*sizeof(IndexEntry));
            index->count--; return index_save(index);
        }
    }
    return -1;
}


//opening file objact
int index_load(Index *index) {
    index->count = 0;
    FILE *f = fopen(INDEX_PATH, "r");
    if (!f) return (errno == ENOENT) ? 0 : -1;
    char hex[65];
    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *e = &index->entries[index->count];
        if (fscanf(f, "%o %64s %lu %u %511s\n", &e->mode, hex, &e->mtime_sec, &e->size, e->path) == EOF) break;
        hex_to_hash(hex, &e->hash); index->count++;
    }
    fclose(f); return 0;
}

int index_save(const Index *index) {
    char tmp[512]; snprintf(tmp, 512, "%s.tmp", INDEX_PATH);
    FILE *f = fopen(tmp, "w");
    for (int i = 0; i < index->count; i++) {
        char hex[65]; hash_to_hex(&index->entries[i].hash, hex);
        fprintf(f, "%o %s %lu %u %s\n", index->entries[i].mode, hex, index->entries[i].mtime_sec, index->entries[i].size, index->entries[i].path);
    }
    fflush(f); fsync(fileno(f)); fclose(f); rename(tmp, INDEX_PATH);
    return 0;
}

int index_add(Index *index, const char *path) {
    struct stat st; if (stat(path, &st) != 0) return -1;
    FILE *f = fopen(path, "rb");
    void *data = malloc(st.st_size); fread(data, 1, st.st_size, f); fclose(f);
    ObjectID hash; object_write(OBJ_BLOB, data, st.st_size, &hash); free(data);
    IndexEntry *e = index_find(index, path);
    if (!e) e = &index->entries[index->count++];
    e->mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    e->hash = hash; e->mtime_sec = st.st_mtime; e->size = st.st_size;
    strncpy(e->path, path, 511); return index_save(index);
}
