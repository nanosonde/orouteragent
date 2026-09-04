/* orouteragent - persistent state implementation */
#include "state.h"
#include "protocol/constants.h"
#include "protocol/message.h"
#include "util.h"

#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static bool is_md5_hex(const char *value)
{
    size_t i;

    if (!value || strlen(value) != 32)
        return false;
    for (i = 0; i < 32; i++) {
        if (!isxdigit((unsigned char)value[i]))
            return false;
    }
    return true;
}

void ora_state_init(struct ora_state *st, const char *path, size_t max_config_kb)
{
    memset(st, 0, sizeof(*st));
    snprintf(st->path, sizeof(st->path), "%s", path ? path : "");
    st->max_config_kb = max_config_kb ? max_config_kb : 512;
    st->boot_epoch = (int64_t)ora_now_s() - (int64_t)ora_uptime_s();
    st->set_blobs = json_object_new_object();
    st->cfg_results = json_object_new_array();
}

bool ora_state_load(struct ora_state *st)
{
    FILE *f;
    json_object *root = NULL;
    json_tokener *tok = NULL;
    char *data = NULL;
    long len;
    bool ok = false;

    f = fopen(st->path, "re");
    if (!f)
        return true; /* fresh start is not an error */
    if (fseek(f, 0, SEEK_END) != 0)
        goto out;
    len = ftell(f);
    if (len < 0 || len > 4 * 1024 * 1024)
        goto out;
    rewind(f);
    data = malloc(len + 1);
    if (!data)
        goto out;
    if (fread(data, 1, len, f) != (size_t)len)
        goto out;
    data[len] = '\0';

    tok = json_tokener_new();
    root = json_tokener_parse_ex(tok, data, (int)len);
    if (!root || json_tokener_get_error(tok) != json_tokener_success)
        goto out;
    if (!json_object_is_type(root, json_type_object))
        goto out;

    {
        const char *s;
        json_object *o;

        if ((s = ora_json_get_str(root, "omadacId", NULL)) && *s) {
            snprintf(st->omadac_id, sizeof(st->omadac_id), "%s", s);
            if (!strcmp(st->omadac_id, ORA_FACTORY_SENTINEL_ID))
                st->omadac_id[0] = '\0';
        }
        st->adopted = ora_json_get_bool(root, "adopted", false);
        if ((s = ora_json_get_str(root, "deviceUsername", NULL)))
            snprintf(st->device_username, sizeof(st->device_username), "%s", s);
        if ((s = ora_json_get_str(root, "devicePassword", NULL)))
            snprintf(st->device_password, sizeof(st->device_password), "%s", s);
        /* Preserve the provisioned representation so resumed authentication
         * does not hash an already encoded password a second time. */
        st->device_password_is_md5 = ora_json_get_bool(
            root, "devicePasswordIsMd5", is_md5_hex(st->device_password));
        st->config_version = ora_json_get_int(root, "configVersion", 0);
        if ((s = ora_json_get_str(root, "captureFile", NULL)))
            snprintf(st->capture_file, sizeof(st->capture_file), "%s", s);
        st->boot_epoch = ora_json_get_int(root, "bootEpoch", st->boot_epoch);

        o = json_object_object_get(root, "setBlobs");
        if (o && json_object_is_type(o, json_type_object)) {
            json_object_put(st->set_blobs);
            st->set_blobs = json_object_get(o); /* steal */
            /* recompute blob byte count */
            st->blob_bytes = 0;
            json_object_object_foreach(st->set_blobs, k, v) {
                st->blob_bytes += strlen(k) + (size_t)json_object_get_string_len(v) + 8;
            }
        }
    }
    ok = true;
out:
    if (root)
        json_object_put(root);
    if (tok)
        json_tokener_free(tok);
    free(data);
    fclose(f);
    if (!ok)
        ora_log(ORA_LOG_WARN, "state file %s unreadable; starting fresh", st->path);
    return true; /* always start, even with a corrupt file */
}

bool ora_state_save(const struct ora_state *st)
{
    json_object *root;
    const char *s;
    char tmp[280];
    FILE *f = NULL;
    int fd;
    bool ok = false;

    root = json_object_new_object();
    if (!root)
        return false;

    json_object_object_add(root, "omadacId",
        json_object_new_string(st->omadac_id[0] ? st->omadac_id : ORA_FACTORY_SENTINEL_ID));
    json_object_object_add(root, "adopted", json_object_new_boolean(st->adopted));
    json_object_object_add(root, "deviceUsername", json_object_new_string(st->device_username));
    json_object_object_add(root, "devicePassword", json_object_new_string(st->device_password));
    json_object_object_add(root, "devicePasswordIsMd5",
                           json_object_new_boolean(st->device_password_is_md5));
    json_object_object_add(root, "configVersion", json_object_new_int64(st->config_version));
    json_object_object_add(root, "captureFile", json_object_new_string(st->capture_file));
    json_object_object_add(root, "bootEpoch", json_object_new_int64(st->boot_epoch));
    if (st->set_blobs)
        json_object_object_add(root, "setBlobs", json_object_get(st->set_blobs));

    s = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);

    snprintf(tmp, sizeof(tmp), "%s.tmp", st->path);
    /* The 0600 state file holds the device account. Create it fresh and
     * never through a symlink, including in a shared parent directory. */
    if (unlink(tmp) != 0 && errno != ENOENT)
        goto out_nofile;
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        goto out_nofile;
    f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        goto out;
    }
    if (fputs(s, f) == EOF)
        goto out_close;
    if (fclose(f) != 0) {
        f = NULL;
        goto out;
    }
    f = NULL;
    if (rename(tmp, st->path) != 0) {
        unlink(tmp);
        goto out;
    }
    ok = true;
    goto out_noclose;
out_close:
    fclose(f);
out:
    unlink(tmp);
out_nofile:
out_noclose:
    json_object_put(root);
    if (!ok)
        ora_log(ORA_LOG_ERR, "failed to persist state to %s: %s", st->path,
                strerror(errno));
    return ok;
}

void ora_state_set_omadac(struct ora_state *st, const char *omadac_id)
{
    if (!omadac_id || !*omadac_id || !strcmp(omadac_id, ORA_FACTORY_SENTINEL_ID)) {
        st->omadac_id[0] = '\0';
    } else {
        snprintf(st->omadac_id, sizeof(st->omadac_id), "%s", omadac_id);
    }
}

void ora_state_set_adopted(struct ora_state *st, bool adopted)
{
    st->adopted = adopted;
}

bool ora_state_set_blob(struct ora_state *st, const char *key, const char *json_str)
{
    json_object *v;
    size_t len;

    if (!st->set_blobs) {
        st->set_blobs = json_object_new_object();
        if (!st->set_blobs)
            return false;
    }
    len = strlen(key) + strlen(json_str) + 8;
    if (st->blob_bytes + len > st->max_config_kb * 1024) {
        ora_log(ORA_LOG_WARN, "config blob store full (%zu bytes); dropping SET %s",
                st->blob_bytes, key);
        return false;
    }
    v = json_object_new_string(json_str);
    if (!v)
        return false;
    /* replaces any previous value for this key, so recompute rather
     * than accumulate: the controller re-pushes the same keys forever */
    json_object_object_add(st->set_blobs, key, v);
    st->blob_bytes = 0;
    json_object_object_foreach(st->set_blobs, k, val) {
        st->blob_bytes += strlen(k) + (size_t)json_object_get_string_len(val) + 8;
    }
    return true;
}

void ora_state_clear_blobs(struct ora_state *st)
{
    if (st->set_blobs) {
        json_object_put(st->set_blobs);
        st->set_blobs = json_object_new_object();
    }
    st->blob_bytes = 0;
    if (st->cfg_results) {
        json_object_put(st->cfg_results);
        st->cfg_results = json_object_new_array();
    }
    if (st->last_cfg_result) {
        json_object_put(st->last_cfg_result);
        st->last_cfg_result = NULL;
    }
}

void ora_state_record_cfg_result(struct ora_state *st, json_object *resp)
{
    if (!resp)
        return;
    if (!st->cfg_results)
        st->cfg_results = json_object_new_array();

    while (json_object_array_length(st->cfg_results) >= ORA_CFG_RESULT_MAX)
        json_object_array_del_idx(st->cfg_results, 0, 1);
    json_object_array_add(st->cfg_results, json_object_get(resp));

    if (st->last_cfg_result)
        json_object_put(st->last_cfg_result);
    st->last_cfg_result = json_object_get(resp);
}

void ora_state_set_account(struct ora_state *st, const char *user, const char *pass,
                           bool password_is_md5)
{
    if (user && *user)
        snprintf(st->device_username, sizeof(st->device_username), "%s", user);
    if (pass && *pass) {
        snprintf(st->device_password, sizeof(st->device_password), "%s", pass);
        st->device_password_is_md5 = password_is_md5;
    }
}

void ora_state_free(struct ora_state *st)
{
    if (st->set_blobs) {
        json_object_put(st->set_blobs);
        st->set_blobs = NULL;
    }
    if (st->cfg_results) {
        json_object_put(st->cfg_results);
        st->cfg_results = NULL;
    }
    if (st->last_cfg_result) {
        json_object_put(st->last_cfg_result);
        st->last_cfg_result = NULL;
    }
}