/* orouteragent - controller /api/info client implementation
 *
 * GET https://<controller>:8043/api/info -> {"errorCode":0,...,
 * "result":{"omadacId":"<32-hex>"}}  (unauthenticated endpoint)
 */
#include "controller_info.h"
#include "tls.h"
#include "../protocol/message.h"
#include "../util.h"

#include <stdio.h>
#include <string.h>

/* portable strcasestr (musl does not provide one) */
static char *find_ci(char *hay, const char *needle)
{
    size_t nl = strlen(needle);

    if (!nl)
        return hay;
    for (; *hay; hay++) {
        if (strncasecmp(hay, needle, nl) == 0)
            return hay;
    }
    return NULL;
}

bool ora_controller_info_get(const char *host, int port, bool verify_tls,
                             char *omadac_id, size_t outsz)
{
    struct ora_tls t;
    char req[256];
    char *resp = NULL;
    size_t cap = 8192, len = 0;
    bool ok = false;
    json_object *root = NULL;
    json_tokener *tok = NULL;

    omadac_id[0] = '\0';

    if (!ora_tls_init(&t))
        return false;

    snprintf(req, sizeof(req),
             "GET /api/info HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "User-Agent: orouteragent\r\n"
             "Accept: application/json\r\n"
             "Connection: close\r\n"
             "\r\n", host, port);

    if (!ora_tls_connect(&t, host, port, 10000, verify_tls))
        goto out;

    if (ora_tls_write(&t, req, strlen(req)) < 0)
        goto out;

    resp = malloc(cap);
    if (!resp)
        goto out;

    for (;;) {
        int rv;
        if (len + 1024 > cap) {
            char *nb = realloc(resp, cap * 2);
            if (!nb)
                goto out;
            resp = nb;
            cap *= 2;
        }
        rv = ora_tls_read(&t, resp + len, cap - len - 1, 5000);
        if (rv < 0)
            break; /* closed */
        if (rv == 0)
            continue;
        len += (size_t)rv;
        /* complete response? headers + body; body length unknown -> read
         * until close (Connection: close) */
    }
    if (len == 0)
        goto out;
    resp[len] = '\0';

    /* find body */
    {
        char *body = strstr(resp, "\r\n\r\n");
        long clen = -1;
        char *cp;

        if (!body)
            goto out;
        body += 4;

        /* honor Content-Length if present */
        cp = find_ci(resp, "content-length:");
        if (cp && cp < body) {
            clen = strtol(cp + 15, NULL, 10);
            if (clen >= 0 && (size_t)clen <= len - (size_t)(body - resp))
                len = (size_t)(body - resp) + (size_t)clen;
        }

        tok = json_tokener_new();
        root = json_tokener_parse_ex(tok, body, (int)(len - (size_t)(body - resp)));
        if (!root || json_tokener_get_error(tok) != json_tokener_success)
            goto out;

        json_object *result = ora_json_get_obj(root, "result");
        const char *id = ora_json_get_str(result, "omadacId", NULL);
        if (!id || !*id) {
            goto out;
        }
        snprintf(omadac_id, outsz, "%s", id);
        ok = true;
    }

out:
    if (root)
        json_object_put(root);
    if (tok)
        json_tokener_free(tok);
    free(resp);
    ora_tls_close(&t);
    ora_tls_free(&t);
    return ok;
}