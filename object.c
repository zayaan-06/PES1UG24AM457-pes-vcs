#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <errno.h>

// ─── ALL PROVIDED & IMPLEMENTED FUNCTIONS ───────────────────────────────────
//initializing hashes 
void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        sscanf(hex + i * 2, "%2x", &byte);
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    const char *type_str = (type == OBJ_BLOB) ? "blob" : (type == OBJ_TREE) ? "tree" : "commit";
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    size_t full_len = header_len + 1 + len;
    uint8_t *full_obj = malloc(full_len);
    memcpy(full_obj, header, header_len + 1);
    memcpy(full_obj + header_len + 1, data, len);
    compute_hash(full_obj, full_len, id_out);
    if (object_exists(id_out)) { free(full_obj); return 0; }
    mkdir(PES_DIR, 0755); mkdir(OBJECTS_DIR, 0755);
    char hex[HASH_HEX_SIZE + 1]; hash_to_hex(id_out, hex);
    char shard_dir[512], final_path[512], tmp_path[512];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(shard_dir, 0755);
    object_path(id_out, final_path, sizeof(final_path));
    snprintf(tmp_path, sizeof(tmp_path), "%s/%.2s/tmp_XXXXXX", OBJECTS_DIR, hex);
    int fd = mkstemp(tmp_path);
    write(fd, full_obj, full_len);
    fsync(fd); close(fd); rename(tmp_path, final_path);
    free(full_obj); return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512]; object_path(id, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long size = ftell(f); rewind(f);
    uint8_t *buf = malloc(size); fread(buf, 1, size, f); fclose(f);
    uint8_t *null_pos = memchr(buf, '\0', size);
    if (strncmp((char*)buf, "blob ", 5) == 0) *type_out = OBJ_BLOB;
    else if (strncmp((char*)buf, "tree ", 5) == 0) *type_out = OBJ_TREE;
    else *type_out = OBJ_COMMIT;
    *len_out = size - (null_pos + 1 - buf);
    *data_out = malloc(*len_out);
    memcpy(*data_out, null_pos + 1, *len_out);
    free(buf); return 0;
}
