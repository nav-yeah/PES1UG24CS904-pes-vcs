// object.c — Content-addressable object store

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>




void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
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

// ─────────────────────────────────────────────────────────────────────────────
// Helper: create directory if it doesn't exist
static void ensure_dir(const char *path) {
    mkdir(path, 0755); // ignore errors (EEXIST is fine)
}

// ─────────────────────────────────────────────────────────────────────────────
// OBJECT WRITE

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    if (!data || !id_out) return -1;

    // 1. Build header
    const char *type_str =
        (type == OBJ_BLOB) ? "blob" :
        (type == OBJ_TREE) ? "tree" :
        "commit";

    char header[128];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);

    size_t total_len = header_len + 1 + len; // +1 for '\0'
    char *buffer = malloc(total_len);
    if (!buffer) return -1;

    memcpy(buffer, header, header_len);
    buffer[header_len] = '\0';
    memcpy(buffer + header_len + 1, data, len);

    // 2. Compute hash of FULL object
    compute_hash(buffer, total_len, id_out);

    // 3. Deduplication check
    if (object_exists(id_out)) {
        free(buffer);
        return 0;
    }

    // 4. Create shard directory
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex);

    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s/%.2s", OBJECTS_DIR, hex);
    ensure_dir(OBJECTS_DIR);
    ensure_dir(dir_path);

    // 5. Final + temp path
    char final_path[512];
    snprintf(final_path, sizeof(final_path), "%s/%s", dir_path, hex + 2);

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.tmp_%s", dir_path, hex + 2);

    // 6. Write temp file
    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(buffer);
        return -1;
    }

    if (write(fd, buffer, total_len) != (ssize_t)total_len) {
        close(fd);
        free(buffer);
        return -1;
    }

    fsync(fd);
    close(fd);

    // 7. Atomic rename
    if (rename(tmp_path, final_path) != 0) {
        free(buffer);
        return -1;
    }

    // 8. fsync directory
    int dfd = open(dir_path, O_RDONLY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }

    free(buffer);
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// OBJECT READ

int object_read(const ObjectID *id, ObjectType *type_out,
                void **data_out, size_t *len_out) {
    if (!id || !data_out || !len_out || !type_out) return -1;

    char path[512];
    object_path(id, path, sizeof(path));

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    fseek(fp, 0, SEEK_END);
    size_t file_size = ftell(fp);
    rewind(fp);

    char *file_data = malloc(file_size);
    if (!file_data) {
        fclose(fp);
        return -1;
    }

    if (fread(file_data, 1, file_size, fp) != file_size) {
        fclose(fp);
        free(file_data);
        return -1;
    }
    fclose(fp);

    // 1. Find header/data split
    char *sep = memchr(file_data, '\0', file_size);
    if (!sep) {
        free(file_data);
        return -1;
    }

    size_t header_len = sep - file_data;
    char *data = sep + 1;
    size_t data_len = file_size - header_len - 1;

    // 2. Parse type
    if (strncmp(file_data, "blob", 4) == 0) *type_out = OBJ_BLOB;
    else if (strncmp(file_data, "tree", 4) == 0) *type_out = OBJ_TREE;
    else if (strncmp(file_data, "commit", 6) == 0) *type_out = OBJ_COMMIT;
    else {
        free(file_data);
        return -1;
    }

    // 3. Verify hash
    ObjectID computed;
    compute_hash(file_data, file_size, &computed);

    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(file_data);
        return -1;
    }

    // 4. Return data
    *data_out = malloc(data_len);
    if (!*data_out) {
        free(file_data);
        return -1;
    }

    memcpy(*data_out, data, data_len);
    *len_out = data_len;

    free(file_data);
    return 0;
}
