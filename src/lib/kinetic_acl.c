/*
* kinetic-c-client
* Copyright (C) 2015 Seagate Technology.
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*
*/
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

#include "kinetic_logger.h"
#include "kinetic_acl.h"
#include "json.h"

typedef struct {
    KineticProto_Command_Security_ACL_Permission permission;
    const char *string;
} permission_pair;

static permission_pair permission_table[] = {
    { KINETIC_PROTO_COMMAND_SECURITY_ACL_PERMISSION_INVALID_PERMISSION, "INVALID" },
    { KINETIC_PROTO_COMMAND_SECURITY_ACL_PERMISSION_READ, "READ" },
    { KINETIC_PROTO_COMMAND_SECURITY_ACL_PERMISSION_WRITE, "WRITE" },
    { KINETIC_PROTO_COMMAND_SECURITY_ACL_PERMISSION_DELETE, "DELETE" },
    { KINETIC_PROTO_COMMAND_SECURITY_ACL_PERMISSION_RANGE, "RANGE" },
    { KINETIC_PROTO_COMMAND_SECURITY_ACL_PERMISSION_SETUP, "SETUP" },
    { KINETIC_PROTO_COMMAND_SECURITY_ACL_PERMISSION_P2POP, "P2POP" },
    { KINETIC_PROTO_COMMAND_SECURITY_ACL_PERMISSION_GETLOG, "GETLOG" },
    { KINETIC_PROTO_COMMAND_SECURITY_ACL_PERMISSION_SECURITY, "SECURITY" },
};

#define PERM_TABLE_ROWS sizeof(permission_table)/sizeof(permission_table)[0]

static KineticACLLoadResult read_next_ACL(const char *buf, size_t buf_size,
    size_t offset, size_t *new_offset, struct json_tokener *tokener,
    KineticProto_Command_Security_ACL **instance);
static KineticACLLoadResult unpack_scopes(KineticProto_Command_Security_ACL *acl,
    int scope_count, json_object *scopes);

static const char *str_of_permission(KineticProto_Command_Security_ACL_Permission perm) {
    for (size_t i = 0; i < PERM_TABLE_ROWS; i++) {
        permission_pair *pp = &permission_table[i];
        if (pp->permission == perm) { return pp->string; }
    }
    return "INVALID";
}

static KineticProto_Command_Security_ACL_Permission permission_of_str(const char *str) {
    for (size_t i = 0; i < PERM_TABLE_ROWS; i++) {
        permission_pair *pp = &permission_table[i];
        if (0 == strcmp(str, pp->string)) { return pp->permission; }
    }
    return KINETIC_PROTO_COMMAND_SECURITY_ACL_PERMISSION_INVALID_PERMISSION;
}

KineticACLLoadResult
KineticACL_LoadFromFile(const char *path, struct ACL **instance) {
    if (path == NULL || instance == NULL) {
        return ACL_ERROR_NULL;
    }
    
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
#ifndef TEST
        LOGF0("Failed ot open file '%s': %s", path, strerror(errno));
#endif
        errno = 0;
        return ACL_ERROR_BAD_JSON;
    }
    KineticACLLoadResult res = ACL_ERROR_NULL;

    const int BUF_START_SIZE = 256;
    char *buf = calloc(1, BUF_START_SIZE);
    if (buf == NULL) {
        res = ACL_ERROR_MEMORY;
        goto cleanup;
    }
    size_t buf_sz = BUF_START_SIZE;
    size_t buf_used = 0;

    ssize_t read_sz = 0;
    for (;;) {
        read_sz = read(fd, &buf[buf_used], buf_sz - buf_used);
        if (read_sz == -1) {
            res = ACL_ERROR_JSON_FILE;
            goto cleanup;
        } else if (read_sz == 0) {
            break;
        } else {
            buf_used += read_sz;
            if (buf_sz == buf_used) {
                size_t nsz = 2 * buf_sz;
                char *nbuf = realloc(buf, nsz);
                if (nbuf) {
                    buf_sz = nsz;
                    buf = nbuf;
                }
            }
        }
    }

#ifndef TEST
    LOGF2(" -- read %zd bytes, parsing...\n", buf_used);
#endif
    res = acl_of_string(buf, buf_used, instance);
cleanup:
    if (buf) { free(buf); }
    close(fd);
    return res;
}

KineticACLLoadResult
acl_of_string(const char *buf, size_t buf_size, struct ACL **instance) {
    KineticACLLoadResult res = ACL_ERROR_MEMORY;
    struct ACL *acl_group = NULL;
    KineticProto_Command_Security_ACL **acl_array = NULL;
    struct json_tokener* tokener = NULL;

    acl_group = calloc(1, sizeof(*acl_group));
    if (acl_group == NULL) { goto cleanup; }

    acl_group->ACLs = calloc(1, sizeof(KineticProto_Command_Security_ACL *));
    if (acl_group->ACLs == NULL) { goto cleanup; }
    acl_group->ACL_ceil = 1;
    acl_group->ACL_count = 0;

    tokener = json_tokener_new();
    if (tokener == NULL) { goto cleanup; }

    size_t offset = 0;

    while (buf_size - offset > 0) {
        size_t offset_out = 0;
        KineticProto_Command_Security_ACL *new_acl = NULL;
#ifndef TEST
        LOGF2(" -- reading next ACL at offset %zd, rem %zd\n", offset, buf_size - offset);
#endif
        res = read_next_ACL(buf, buf_size, offset,
            &offset_out, tokener, &new_acl);
        offset += offset_out;
#ifndef TEST
        LOGF2(" -- result %d, offset_out %zd\n", res, offset);
#endif
        if (res == ACL_OK) {
            if (acl_group->ACL_count == acl_group->ACL_ceil) {  /* grow */
                size_t nsz = 2 * acl_group->ACL_ceil * sizeof(acl_group->ACLs[0]);
                KineticProto_Command_Security_ACL **nACLs = realloc(acl_group->ACLs, nsz);
                if (nACLs == NULL) {
                    goto cleanup;
                } else {
                    acl_group->ACL_ceil *= 2;
                    acl_group->ACLs = nACLs;
                }
            }
            acl_group->ACLs[acl_group->ACL_count] = new_acl;
            acl_group->ACL_count++;
        } else {
            break;
        }
    }

cleanup:
    if (tokener) { json_tokener_free(tokener); }
    
    if (res == ACL_END_OF_STREAM || res == ACL_OK) {
        if (acl_group && acl_group->ACL_count == 0) {
            LOG2("Failed to read any JSON objects\n");
            return ACL_ERROR_BAD_JSON;
        } else {            /* read at least one ACL */
            *instance = acl_group;
            return ACL_OK;
        }
    } else {
        if (acl_group) { free(acl_group); }
        if (acl_array) { free(acl_array); }
        return res;
    }
}

static KineticACLLoadResult read_next_ACL(const char *buf, size_t buf_size,
        size_t offset, size_t *new_offset,
        struct json_tokener *tokener, KineticProto_Command_Security_ACL **instance) {
    struct json_object *obj = json_tokener_parse_ex(tokener,
        &buf[offset], buf_size - offset);
    if (obj == NULL) {
        if (json_tokener_get_error(tokener) == json_tokener_error_parse_eof) {
            return ACL_END_OF_STREAM;
        } else {
            LOGF2("JSON error %d\n", json_tokener_get_error(tokener));
            return ACL_ERROR_BAD_JSON;
        }
    }
    
    *new_offset = tokener->char_offset;
    
    KineticACLLoadResult res = ACL_ERROR_MEMORY;
    KineticProto_Command_Security_ACL *acl = NULL;
    uint8_t *data = NULL;

    int scope_count = 0;
    struct json_object *val = NULL;
    if (json_object_object_get_ex(obj, "scope", &val)) {
        scope_count = json_object_array_length(val);
    } else {
        res = ACL_ERROR_MISSING_FIELD;
        goto cleanup;
    }
    
    size_t alloc_sz = sizeof(*acl);
    acl = calloc(1, alloc_sz);
    if (acl == NULL) { goto cleanup; }

    KineticProto_command_security_acl__init(acl);    

    /* Copy fields */
    if (json_object_object_get_ex(obj, "identity", &val)) {
        acl->has_identity = true;
        acl->identity = json_object_get_int64(val);
    } else {
        acl->has_identity = false;
    }
    
    if (json_object_object_get_ex(obj, "key", &val)) {
        const char *key = json_object_get_string(val);
        size_t len = strlen(key);

        acl->has_hmacAlgorithm = true;
        acl->hmacAlgorithm = KINETIC_PROTO_COMMAND_SECURITY_ACL_HMACALGORITHM_HmacSHA1;

        acl->key.len = len;
        data = calloc(1, len + 1);
        if (data == NULL) { goto cleanup; }
        memcpy(data, key, len);
        data[len] = '\0';
        acl->key.data = data;
        data = NULL;
        acl->has_key = true;
    }
    
    if (json_object_object_get_ex(obj, "HMACAlgorithm", &val)) {
        const char *hmac = json_object_get_string(val);
        if (0 != strcmp(hmac, "HmacSHA1")) {
            res = ACL_ERROR_INVALID_FIELD;
            goto cleanup;
        } else {
            // acl->key->type is already set to HMAC_SHA1.
        }
    }
    
    struct json_object *scopes = NULL;
    if (json_object_object_get_ex(obj, "scope", &scopes)) {
        res = unpack_scopes(acl, scope_count, scopes);
        if (res != ACL_OK) { goto cleanup; }
    }
    
    /* Decrement JSON object refcount, freeing it. */
    json_object_put(obj);
    *instance = acl;
    return ACL_OK;
cleanup:
    if (obj) { json_object_put(obj); }
    if (acl) { free(acl); }
    if (data) { free(data); }
    return res;
}

static KineticACLLoadResult unpack_scopes(KineticProto_Command_Security_ACL *acl,
        int scope_count, json_object *scopes) {
    KineticACLLoadResult res = ACL_ERROR_MEMORY;
    KineticProto_Command_Security_ACL_Scope **scope_array = NULL;
    KineticProto_Command_Security_ACL_Permission *perm_array = NULL;
    KineticProto_Command_Security_ACL_Scope *scope = NULL;
    uint8_t *data = NULL;

    scope_array = calloc(scope_count, sizeof(*scope_array));
    if (scope_array == NULL) { goto cleanup; }

    acl->scope = scope_array;

    for (int si = 0; si < scope_count; si++) {
        struct json_object *cur_scope = json_object_array_get_idx(scopes, si);
        if (cur_scope) {
            scope = calloc(1, sizeof(*scope));
            if (scope == NULL) { goto cleanup; }
            KineticProto_command_security_acl_scope__init(scope);
    
            struct json_object *val = NULL;
            if (json_object_object_get_ex(cur_scope, "offset", &val)) {
                scope->offset = json_object_get_int64(val);
                scope->has_offset = true;
            } else {
                scope->has_offset = false;
            }

            if (json_object_object_get_ex(cur_scope, "value", &val)) {
                const char *str = json_object_get_string(val);
                if (str) {
                    size_t len = strlen(str);
                    data = malloc(len + 1);
                    if (data == NULL) {
                        return ACL_ERROR_MEMORY;
                    } else {
                        memcpy(data, str, len);
                        data[len] = '\0';
                        scope->value.data = data;
                        scope->value.len = len;
                        data = NULL;
                        scope->has_value = true;
                    }
                }
            } else {
                scope->has_value = false;
            }

            scope->n_permission = 0;
            if (json_object_object_get_ex(cur_scope, "permission", &val)) {
                perm_array = calloc(ACL_MAX_PERMISSIONS, sizeof(*perm_array));
                if (perm_array == NULL) { goto cleanup; }
                scope->permission = perm_array;
                perm_array = NULL;

                enum json_type perm_type = json_object_get_type(val);
                if (perm_type == json_type_string) {
                    scope->permission[0] = permission_of_str(json_object_get_string(val));
                    if (scope->permission[0] == KINETIC_PROTO_COMMAND_SECURITY_ACL_PERMISSION_INVALID_PERMISSION) {
                        return ACL_ERROR_INVALID_FIELD;
                    } else {
                        scope->n_permission++;
                    }
                } else if (perm_type == json_type_array) {
                    int count = json_object_array_length(val);
                    for (int i = 0; i < count; i++) {
                        struct json_object *jperm = json_object_array_get_idx(val, i);
                        KineticProto_Command_Security_ACL_Permission p;
                        p = permission_of_str(json_object_get_string(jperm));
                        if (p == KINETIC_PROTO_COMMAND_SECURITY_ACL_PERMISSION_INVALID_PERMISSION) {
                            return ACL_ERROR_INVALID_FIELD;
                        } else {
                            scope->permission[scope->n_permission] = p;
                            scope->n_permission++;
                        }
                    }
                } else {
                    return ACL_ERROR_INVALID_FIELD;
                }
            }
            if (json_object_object_get_ex(cur_scope, "TlsRequired", &val)) {
                scope->TlsRequired = json_object_get_boolean(val);
                scope->has_TlsRequired = true;
            }

            acl->scope[acl->n_scope] = scope;
            acl->n_scope++;
            scope = NULL;
        }
    }
    acl->n_scope = scope_count;
    return ACL_OK;

cleanup:
    if (scope_array) { free(scope_array); }
    if (scope) { free(scope); }
    if (perm_array) { free(perm_array); }
    if (data) { free(data); }
    return res;
}

void KineticACL_Print(FILE *f, struct ACL *ACLs) {
    if (ACLs == NULL) {
        fprintf(f, "NULL\n");
        return;
    }

    fprintf(f, "ACLs [%zd]:\n", ACLs->ACL_count);
    
    for (size_t ai = 0; ai < ACLs->ACL_count; ai++) {
        KineticProto_Command_Security_ACL *acl = ACLs->ACLs[ai];
        if (acl == NULL) { continue; }
        if (ai > 0) { fprintf(f, "\n"); }

        if (acl->has_identity) {
            fprintf(f, "  identity: %lld\n", (long long)acl->identity);
        }

        if (acl->has_key) {
            fprintf(f, "  key[%s,%zd]: \"%s\"\n",
                "HmacSHA1", acl->key.len, acl->key.data);
        }

        fprintf(f, "  scopes: (%zd)\n", acl->n_scope);
        
        for (size_t si = 0; si < acl->n_scope; si++) {
            KineticProto_Command_Security_ACL_Scope *scope = acl->scope[si];
            if (si > 0) { fprintf(f, "\n"); }
            fprintf(f, "    scope %zd:\n", si);
            if (scope->has_offset) {
                fprintf(f, "      offset: %lld\n", (long long)scope->offset);
            }
            if (scope->has_value) {
                fprintf(f, "      value[%zd]: \"%s\"\n",
                    scope->value.len, scope->value.data);
            }
            for (size_t pi = 0; pi < scope->n_permission; pi++) {
                fprintf(f, "      permission: %s\n",
                    str_of_permission(scope->permission[pi]));
            }

            if (scope->has_TlsRequired) {
                fprintf(f, "      TlsRequired: %d\n", scope->TlsRequired);
            }
        }
    }
}

void KineticACL_Free(struct ACL *ACLs) {
    if (ACLs) {
        for (size_t ai = 0; ai < ACLs->ACL_count; ai++) {
            KineticProto_Command_Security_ACL *acl = ACLs->ACLs[ai];
            if (acl) {
                for (size_t si = 0; si < acl->n_scope; si++) {
                    KineticProto_Command_Security_ACL_Scope *scope = acl->scope[si];
                    if (scope->has_value && scope->value.data) {
                        free(scope->value.data);
                    }

                    if (scope->n_permission > 0) {
                        free(scope->permission);
                    }                    
                    free(scope);
                }
                free(acl->scope);

                if (acl->has_key && acl->key.data) {
                    free(acl->key.data);
                }
                free(acl);
            }
        }
        free(ACLs->ACLs);
        free(ACLs);
    }
}