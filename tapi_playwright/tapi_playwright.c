/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Playwright TAPI
 *
 * The library runs te_playwright_runner.cjs on the agent as a job and
 * exchanges one JSON line per command with it over the job channels.
 */

#define TE_LGR_USER     "TAPI Playwright"

#include "te_config.h"

#include <stdlib.h>
#include <string.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_alloc.h"
#include "te_str.h"
#include "te_string.h"
#include "te_json.h"
#include "logger_api.h"
#include "rcf_api.h"
#include "tapi_job.h"
#include "tapi_file.h"
#include "tapi_test_log.h"

#include "tapi_playwright.h"

/** Runner script name, in the installation and on the agent. */
#define RUNNER_NAME     "te_playwright_runner.cjs"

/** Where the installation keeps the runner, relative to TE_INSTALL. */
#define RUNNER_INSTALL_DIR  "default/share/tsf-playwright"

/** Time to wait for the runner to exit after 'close'. */
#define RUNNER_EXIT_TIMEOUT_MS  10000

/** Time added to an action timeout while waiting for its response. */
#define RESPONSE_TIMEOUT_SLACK_MS   5000

/** Default action timeout. */
#define DEFAULT_TIMEOUT_MS  30000

const tapi_playwright_opts tapi_playwright_default_opts = {
    .node = NULL,
    .playwright_dir = NULL,
    .browsers_path = NULL,
    .runner = NULL,
    .workdir = NULL,
    .browser = TAPI_PLAYWRIGHT_CHROMIUM,
    .headed = false,
    .timeout_ms = 0,
};

struct tapi_playwright {
    tapi_playwright_opts opts;      /**< Options with the strings copied */
    const char *ta;                 /**< Agent of the factory */
    tapi_job_t *job;                /**< Runner job */
    tapi_job_channel_t *in;         /**< Runner stdin */
    tapi_job_channel_t *out[2];     /**< Runner stdout and stderr */
    tapi_job_channel_t *resp;       /**< Filter that yields responses */
    char *runner;                   /**< Runner path on the agent */
    bool runner_uploaded;           /**< The library put it there */
    unsigned int next_id;           /**< Id of the next request */
    bool running;                   /**< The runner is started */
};

static const char *browser_names[] = {
    [TAPI_PLAYWRIGHT_CHROMIUM] = "chromium",
    [TAPI_PLAYWRIGHT_FIREFOX] = "firefox",
    [TAPI_PLAYWRIGHT_WEBKIT] = "webkit",
};

static const char *state_names[] = {
    [TAPI_PLAYWRIGHT_ATTACHED] = "attached",
    [TAPI_PLAYWRIGHT_DETACHED] = "detached",
    [TAPI_PLAYWRIGHT_VISIBLE] = "visible",
    [TAPI_PLAYWRIGHT_HIDDEN] = "hidden",
};

static const char *load_names[] = {
    [TAPI_PLAYWRIGHT_LOAD] = "load",
    [TAPI_PLAYWRIGHT_DOMCONTENTLOADED] = "domcontentloaded",
    [TAPI_PLAYWRIGHT_NETWORKIDLE] = "networkidle",
};

/*
 * Decode one JSON string that starts at *in (after the opening quote)
 * into out. On success *in points after the closing quote.
 */
static te_errno
json_string_decode(const char **in, te_string *out)
{
    const char *p = *in;

    while (*p != '"')
    {
        if (*p == '\0')
            return TE_RC(TE_TAPI, TE_EPROTO);

        if (*p != '\\')
        {
            te_string_append(out, "%c", *p++);
            continue;
        }

        p++;
        switch (*p)
        {
            case '"':
            case '\\':
            case '/':
                te_string_append(out, "%c", *p);
                break;
            case 'b':
                te_string_append(out, "\b");
                break;
            case 'f':
                te_string_append(out, "\f");
                break;
            case 'n':
                te_string_append(out, "\n");
                break;
            case 'r':
                te_string_append(out, "\r");
                break;
            case 't':
                te_string_append(out, "\t");
                break;
            case 'u':
            {
                char hex[5] = {0};
                unsigned long code;
                char *end;

                memcpy(hex, p + 1, 4);
                code = strtoul(hex, &end, 16);
                if (end != hex + 4)
                    return TE_RC(TE_TAPI, TE_EPROTO);
                p += 4;

                /* A surrogate pair encodes one code point above U+FFFF */
                if (code >= 0xD800 && code <= 0xDBFF &&
                    p[1] == '\\' && p[2] == 'u')
                {
                    unsigned long low;

                    memcpy(hex, p + 3, 4);
                    low = strtoul(hex, &end, 16);
                    if (end == hex + 4 && low >= 0xDC00 && low <= 0xDFFF)
                    {
                        code = 0x10000 + ((code - 0xD800) << 10) +
                               (low - 0xDC00);
                        p += 6;
                    }
                }

                if (code < 0x80)
                    te_string_append(out, "%c", (int)code);
                else if (code < 0x800)
                    te_string_append(out, "%c%c", (int)(0xC0 | (code >> 6)),
                                     (int)(0x80 | (code & 0x3F)));
                else if (code < 0x10000)
                    te_string_append(out, "%c%c%c",
                                     (int)(0xE0 | (code >> 12)),
                                     (int)(0x80 | ((code >> 6) & 0x3F)),
                                     (int)(0x80 | (code & 0x3F)));
                else
                    te_string_append(out, "%c%c%c%c",
                                     (int)(0xF0 | (code >> 18)),
                                     (int)(0x80 | ((code >> 12) & 0x3F)),
                                     (int)(0x80 | ((code >> 6) & 0x3F)),
                                     (int)(0x80 | (code & 0x3F)));
                break;
            }
            default:
                return TE_RC(TE_TAPI, TE_EPROTO);
        }
        p++;
    }

    *in = p + 1;
    return 0;
}

/*
 * Take apart a runner response:
 *   {"id":<id>,"ok":true,"value":"<string>"}
 *   {"id":<id>,"ok":false,"error":"<string>"}
 * The runner writes the keys in this order, so the parser expects it.
 */
static te_errno
response_parse(const char *line, unsigned int *id, bool *ok,
               te_string *payload)
{
    const char *p = line;
    char *end;
    unsigned long n;

    if (strncmp(p, "{\"id\":", 6) != 0)
        return TE_RC(TE_TAPI, TE_EPROTO);
    p += 6;

    n = strtoul(p, &end, 10);
    if (end == p)
        return TE_RC(TE_TAPI, TE_EPROTO);
    *id = n;
    p = end;

    if (strncmp(p, ",\"ok\":true,\"value\":\"", 20) == 0)
    {
        *ok = true;
        p += 20;
    }
    else if (strncmp(p, ",\"ok\":false,\"error\":\"", 21) == 0)
    {
        *ok = false;
        p += 21;
    }
    else
    {
        return TE_RC(TE_TAPI, TE_EPROTO);
    }

    return json_string_decode(&p, payload);
}

/* Map the first line of a runner error to a status code. */
static te_errno
error_to_rc(const char *error)
{
    if (strstr(error, "Timeout") != NULL || strstr(error, "timeout") != NULL)
        return TE_RC(TE_TAPI, TE_ETIMEDOUT);
    if (strncmp(error, "no attribute", 12) == 0)
        return TE_RC(TE_TAPI, TE_ENOENT);
    return TE_RC(TE_TAPI, TE_EFAIL);
}

/*
 * Start a request: the object with its id and the command. The
 * caller adds the arguments and calls request_send().
 */
static void
request_begin(tapi_playwright *pw, te_json_ctx_t *ctx, const char *cmd)
{
    te_json_start_object(ctx);
    te_json_add_key(ctx, "id");
    te_json_add_integer(ctx, pw->next_id);
    te_json_add_key_str(ctx, "cmd", cmd);
}

/*
 * Finish the request, send it and wait for the response with the
 * matching id. The value goes to *value when it is not NULL.
 */
static te_errno
request_send(tapi_playwright *pw, te_json_ctx_t *ctx, te_string *req,
             const char *cmd, te_string *value, int timeout_ms)
{
    te_errno rc;
    te_string line = TE_STRING_INIT;
    te_string payload = TE_STRING_INIT;
    unsigned int id = pw->next_id++;
    unsigned int resp_id;
    bool ok;

    te_json_end(ctx);
    te_string_append(req, "\n");

    if (!pw->running)
    {
        ERROR("Playwright command '%s': the runner is not started", cmd);
        rc = TE_RC(TE_TAPI, TE_ENOTCONN);
        goto out;
    }

    rc = tapi_job_send(pw->in, req);
    if (rc != 0)
    {
        ERROR("Failed to send Playwright command '%s': %r", cmd, rc);
        goto out;
    }

    rc = tapi_job_receive_single(pw->resp, &line, timeout_ms);
    if (rc != 0)
    {
        ERROR("No response to Playwright command '%s': %r", cmd, rc);
        goto out;
    }

    rc = response_parse(line.ptr, &resp_id, &ok, &payload);
    if (rc != 0)
    {
        ERROR("Bad response to Playwright command '%s': %s", cmd, line.ptr);
        goto out;
    }

    if (resp_id != id)
    {
        ERROR("Response id %u to Playwright command '%s' (id %u)",
              resp_id, cmd, id);
        rc = TE_RC(TE_TAPI, TE_EPROTO);
        goto out;
    }

    if (!ok)
    {
        ERROR("Playwright command '%s' failed: %s", cmd, payload.ptr);
        rc = error_to_rc(payload.ptr);
        goto out;
    }

    if (value != NULL)
        te_string_append(value, "%s", payload.ptr);

out:
    te_string_free(&line);
    te_string_free(&payload);
    te_string_free(req);
    return rc;
}

/* Timeout for the response to an action: the action timeout plus slack. */
static int
response_timeout(const tapi_playwright *pw)
{
    return pw->opts.timeout_ms + RESPONSE_TIMEOUT_SLACK_MS;
}

/* A command with up to two string arguments and one result string. */
static te_errno
simple_command(tapi_playwright *pw, const char *cmd,
               const char *key1, const char *arg1,
               const char *key2, const char *arg2,
               te_string *value)
{
    te_string req = TE_STRING_INIT;
    te_json_ctx_t ctx = TE_JSON_INIT_STR(&req);

    request_begin(pw, &ctx, cmd);
    if (key1 != NULL)
        te_json_add_key_str(&ctx, key1, arg1);
    if (key2 != NULL)
        te_json_add_key_str(&ctx, key2, arg2);

    return request_send(pw, &ctx, &req, cmd, value, response_timeout(pw));
}

/* Path of the runner in the engine installation. */
static te_errno
runner_local_path(te_string *path)
{
    const char *install = getenv("TE_INSTALL");

    if (install == NULL)
    {
        ERROR("TE_INSTALL is not set, cannot find " RUNNER_NAME);
        return TE_RC(TE_TAPI, TE_ENOENT);
    }

    te_string_append(path, "%s/" RUNNER_INSTALL_DIR "/" RUNNER_NAME, install);
    return 0;
}

/* Copy the installed runner to the agent. */
static te_errno
runner_upload(tapi_playwright *pw)
{
    te_errno rc;
    te_string local = TE_STRING_INIT;

    rc = runner_local_path(&local);
    if (rc != 0)
        return rc;

    rc = rcf_ta_put_file(pw->ta, 0, local.ptr, pw->runner);
    if (rc != 0)
    {
        ERROR("Failed to copy %s to %s:%s: %r", local.ptr, pw->ta,
              pw->runner, rc);
    }
    else
    {
        pw->runner_uploaded = true;
    }

    te_string_free(&local);
    return rc;
}

/* See description in tapi_playwright.h */
te_errno
tapi_playwright_create(tapi_job_factory_t *factory,
                       const tapi_playwright_opts *opts,
                       tapi_playwright **pw)
{
    te_errno rc;
    tapi_playwright *session;
    te_string playwright_arg = TE_STRING_INIT;
    te_string browsers_arg = TE_STRING_INIT;
    const char *argv[6];
    unsigned int argc = 0;

    if (opts == NULL)
        opts = &tapi_playwright_default_opts;

    session = TE_ALLOC(sizeof(*session));
    session->opts = *opts;
    session->opts.node = TE_STRDUP(opts->node != NULL ? opts->node : "node");
    session->opts.playwright_dir = opts->playwright_dir != NULL ?
                                   TE_STRDUP(opts->playwright_dir) : NULL;
    session->opts.browsers_path = opts->browsers_path != NULL ?
                                  TE_STRDUP(opts->browsers_path) : NULL;
    session->opts.runner = NULL;
    session->opts.workdir = TE_STRDUP(opts->workdir != NULL ?
                                      opts->workdir : "/tmp");
    if (session->opts.timeout_ms == 0)
        session->opts.timeout_ms = DEFAULT_TIMEOUT_MS;
    session->ta = tapi_job_factory_ta(factory);
    session->next_id = 1;

    if (opts->runner != NULL)
    {
        session->runner = TE_STRDUP(opts->runner);
    }
    else
    {
        te_string path = TE_STRING_INIT;

        tapi_file_make_custom_pathname(&path, session->opts.workdir,
                                       "_" RUNNER_NAME);
        session->runner = path.ptr;
    }

    argv[argc++] = session->opts.node;
    argv[argc++] = session->runner;
    if (session->opts.playwright_dir != NULL)
    {
        te_string_append(&playwright_arg, "--playwright=%s",
                         session->opts.playwright_dir);
        argv[argc++] = playwright_arg.ptr;
    }
    if (session->opts.browsers_path != NULL)
    {
        te_string_append(&browsers_arg, "--browsers-path=%s",
                         session->opts.browsers_path);
        argv[argc++] = browsers_arg.ptr;
    }
    argv[argc] = NULL;

    rc = tapi_job_create(factory, NULL, session->opts.node, argv, NULL,
                         &session->job);
    if (rc != 0)
    {
        ERROR("Failed to create the Playwright runner job: %r", rc);
        goto fail;
    }

    rc = tapi_job_alloc_input_channels(session->job, 1, &session->in);
    if (rc == 0)
        rc = tapi_job_alloc_output_channels(session->job, 2, session->out);
    if (rc != 0)
    {
        ERROR("Failed to allocate the Playwright runner channels: %r", rc);
        goto fail;
    }

    rc = tapi_job_attach_filter(TAPI_JOB_CHANNEL_SET(session->out[0]),
                                "playwright-response", true, 0,
                                &session->resp);
    if (rc == 0)
    {
        rc = tapi_job_filter_add_regexp(session->resp,
                                        "^\\{\"id\":[0-9]+,.*\\}$", 0);
    }
    if (rc == 0)
    {
        rc = tapi_job_attach_filter(TAPI_JOB_CHANNEL_SET(session->out[1]),
                                    "playwright-stderr", false, TE_LL_WARN,
                                    NULL);
    }
    if (rc != 0)
    {
        ERROR("Failed to attach the Playwright runner filters: %r", rc);
        goto fail;
    }

    te_string_free(&playwright_arg);
    te_string_free(&browsers_arg);
    *pw = session;
    return 0;

fail:
    te_string_free(&playwright_arg);
    te_string_free(&browsers_arg);
    tapi_playwright_destroy(session);
    return rc;
}

/* See description in tapi_playwright.h */
te_errno
tapi_playwright_start(tapi_playwright *pw)
{
    te_errno rc;
    te_string req = TE_STRING_INIT;
    te_string version = TE_STRING_INIT;
    te_json_ctx_t ctx = TE_JSON_INIT_STR(&req);

    if (pw->running)
        return TE_RC(TE_TAPI, TE_EALREADY);

    if (pw->opts.runner == NULL && !pw->runner_uploaded)
    {
        rc = runner_upload(pw);
        if (rc != 0)
            return rc;
    }

    rc = tapi_job_start(pw->job);
    if (rc != 0)
    {
        ERROR("Failed to start the Playwright runner on %s: %r", pw->ta, rc);
        return rc;
    }
    pw->running = true;

    request_begin(pw, &ctx, "launch");
    te_json_add_key_str(&ctx, "browser", browser_names[pw->opts.browser]);
    te_json_add_key(&ctx, "headless");
    te_json_add_bool(&ctx, !pw->opts.headed);
    te_json_add_key(&ctx, "timeout");
    te_json_add_integer(&ctx, pw->opts.timeout_ms);

    /* A browser launch downloads nothing but may still take a while */
    rc = request_send(pw, &ctx, &req, "launch", &version,
                      response_timeout(pw) * 2);
    if (rc == 0)
        RING("Playwright on %s launched %s", pw->ta, version.ptr);

    te_string_free(&version);
    return rc;
}

/* See description in tapi_playwright.h */
te_errno
tapi_playwright_goto(tapi_playwright *pw, const char *url)
{
    te_errno rc;
    te_string status = TE_STRING_INIT;

    rc = simple_command(pw, "goto", "url", url, NULL, NULL, &status);
    if (rc == 0)
        RING("Playwright: %s -> HTTP %s", url, status.ptr);

    te_string_free(&status);
    return rc;
}

/* See description in tapi_playwright.h */
te_errno
tapi_playwright_click(tapi_playwright *pw, const char *selector)
{
    return simple_command(pw, "click", "selector", selector, NULL, NULL,
                          NULL);
}

/* See description in tapi_playwright.h */
te_errno
tapi_playwright_fill(tapi_playwright *pw, const char *selector,
                     const char *text)
{
    return simple_command(pw, "fill", "selector", selector, "text", text,
                          NULL);
}

/* See description in tapi_playwright.h */
te_errno
tapi_playwright_press(tapi_playwright *pw, const char *selector,
                      const char *key)
{
    return simple_command(pw, "press", "selector", selector, "key", key,
                          NULL);
}

/* See description in tapi_playwright.h */
te_errno
tapi_playwright_select(tapi_playwright *pw, const char *selector,
                       const char *value)
{
    return simple_command(pw, "select", "selector", selector, "value", value,
                          NULL);
}

/* See description in tapi_playwright.h */
te_errno
tapi_playwright_wait_for(tapi_playwright *pw, const char *selector,
                         tapi_playwright_state state)
{
    return simple_command(pw, "wait_for", "selector", selector,
                          "state", state_names[state], NULL);
}

/* See description in tapi_playwright.h */
te_errno
tapi_playwright_wait_load(tapi_playwright *pw, tapi_playwright_load state)
{
    return simple_command(pw, "wait_load", "state", load_names[state],
                          NULL, NULL, NULL);
}

/* See description in tapi_playwright.h */
te_errno
tapi_playwright_get_text(tapi_playwright *pw, const char *selector,
                         te_string *text)
{
    return simple_command(pw, "text", "selector", selector, NULL, NULL,
                          text);
}

/* See description in tapi_playwright.h */
te_errno
tapi_playwright_get_attr(tapi_playwright *pw, const char *selector,
                         const char *name, te_string *value)
{
    return simple_command(pw, "attr", "selector", selector, "name", name,
                          value);
}

/* See description in tapi_playwright.h */
te_errno
tapi_playwright_count(tapi_playwright *pw, const char *selector,
                      unsigned int *count)
{
    te_errno rc;
    te_string value = TE_STRING_INIT;

    rc = simple_command(pw, "count", "selector", selector, NULL, NULL,
                        &value);
    if (rc == 0)
        rc = te_strtoui(value.ptr, 10, count);

    te_string_free(&value);
    return rc;
}

/* See description in tapi_playwright.h */
te_errno
tapi_playwright_eval(tapi_playwright *pw, const char *script,
                     te_string *json)
{
    return simple_command(pw, "eval", "script", script, NULL, NULL, json);
}

/* See description in tapi_playwright.h */
te_errno
tapi_playwright_get_url(tapi_playwright *pw, te_string *url)
{
    return simple_command(pw, "url", NULL, NULL, NULL, NULL, url);
}

/* See description in tapi_playwright.h */
te_errno
tapi_playwright_get_title(tapi_playwright *pw, te_string *title)
{
    return simple_command(pw, "title", NULL, NULL, NULL, NULL, title);
}

/* See description in tapi_playwright.h */
te_errno
tapi_playwright_expect_text(tapi_playwright *pw, const char *selector,
                            const char *expected)
{
    te_errno rc;
    te_string text = TE_STRING_INIT;

    rc = tapi_playwright_get_text(pw, selector, &text);
    if (rc == 0 && strstr(text.ptr, expected) == NULL)
    {
        ERROR("Text of '%s' is \"%s\", expected it to contain \"%s\"",
              selector, text.ptr, expected);
        rc = TE_RC(TE_TAPI, TE_EFAIL);
    }

    te_string_free(&text);
    return rc;
}

/* See description in tapi_playwright.h */
te_errno
tapi_playwright_screenshot(tapi_playwright *pw, const char *name,
                           te_string *path)
{
    te_errno rc;
    te_string remote = TE_STRING_INIT;
    te_string local = TE_STRING_INIT;
    te_string suffix = TE_STRING_INIT;
    const char *dir = getenv("TE_LOG_DIR");

    if (dir == NULL)
        dir = getenv("TE_TMP");
    if (dir == NULL)
    {
        ERROR("Neither TE_LOG_DIR nor TE_TMP is set, nowhere to put "
              "the screenshot");
        return TE_RC(TE_TAPI, TE_ENOENT);
    }

    te_string_append(&suffix, "_%s.png", name);
    tapi_file_make_custom_pathname(&remote, pw->opts.workdir, suffix.ptr);
    tapi_file_make_custom_pathname(&local, dir, suffix.ptr);

    rc = simple_command(pw, "screenshot", "path", remote.ptr, NULL, NULL,
                        NULL);
    if (rc != 0)
        goto out;

    rc = rcf_ta_get_file(pw->ta, 0, remote.ptr, local.ptr);
    if (rc != 0)
    {
        ERROR("Failed to copy the screenshot %s:%s to %s: %r", pw->ta,
              remote.ptr, local.ptr, rc);
        goto out;
    }

    RING_ARTIFACT("Screenshot '%s': %s", name, local.ptr);
    if (path != NULL)
        te_string_append(path, "%s", local.ptr);

out:
    tapi_file_ta_unlink_fmt(pw->ta, "%s", remote.ptr);
    te_string_free(&remote);
    te_string_free(&local);
    te_string_free(&suffix);
    return rc;
}

/* See description in tapi_playwright.h */
te_errno
tapi_playwright_run_spec(tapi_playwright *pw, const char *project_dir,
                         const char *spec, const char *grep,
                         tapi_playwright_spec_result *result)
{
    te_errno rc;
    te_string req = TE_STRING_INIT;
    te_string value = TE_STRING_INIT;
    te_json_ctx_t ctx = TE_JSON_INIT_STR(&req);
    int consumed = 0;

    memset(result, 0, sizeof(*result));
    result->failures = (te_string)TE_STRING_INIT;

    request_begin(pw, &ctx, "run_spec");
    te_json_add_key_str(&ctx, "project_dir", project_dir);
    te_json_add_key_str(&ctx, "spec", spec);
    te_json_add_key_str(&ctx, "grep", grep);

    /* The whole test run happens behind this one response */
    rc = request_send(pw, &ctx, &req, "run_spec", &value,
                      tapi_job_get_timeout());
    if (rc != 0)
        goto out;

    if (sscanf(value.ptr, "passed=%u failed=%u skipped=%u flaky=%u%n",
               &result->passed, &result->failed, &result->skipped,
               &result->flaky, &consumed) != 4)
    {
        ERROR("Cannot parse the 'playwright test' summary: %s", value.ptr);
        rc = TE_RC(TE_TAPI, TE_EPROTO);
        goto out;
    }

    if (value.ptr[consumed] == ' ')
        te_string_append(&result->failures, "%s", value.ptr + consumed + 1);

    RING("playwright test in %s: %u passed, %u failed, %u skipped, %u flaky",
         project_dir, result->passed, result->failed, result->skipped,
         result->flaky);
    if (result->failed != 0)
    {
        ERROR("Failed Playwright tests: %s", result->failures.ptr);
        rc = TE_RC(TE_TAPI, TE_EFAIL);
    }

out:
    te_string_free(&value);
    return rc;
}

/* See description in tapi_playwright.h */
te_errno
tapi_playwright_stop(tapi_playwright *pw)
{
    te_errno rc;
    tapi_job_status_t status;

    if (!pw->running)
        return 0;

    rc = simple_command(pw, "close", NULL, NULL, NULL, NULL, NULL);
    if (rc != 0)
        WARN("Playwright did not close in an orderly way: %r", rc);

    rc = tapi_job_wait(pw->job, RUNNER_EXIT_TIMEOUT_MS, &status);
    if (rc != 0)
    {
        WARN("The Playwright runner did not exit, stopping it: %r", rc);
        rc = tapi_job_stop(pw->job, -1, -1);
    }

    pw->running = false;
    return rc;
}

/* See description in tapi_playwright.h */
void
tapi_playwright_destroy(tapi_playwright *pw)
{
    if (pw == NULL)
        return;

    if (pw->running)
        tapi_playwright_stop(pw);

    if (pw->job != NULL)
        tapi_job_destroy(pw->job, -1);

    if (pw->runner_uploaded)
        tapi_file_ta_unlink_fmt(pw->ta, "%s", pw->runner);

    free((char *)pw->opts.node);
    free((char *)pw->opts.playwright_dir);
    free((char *)pw->opts.browsers_path);
    free((char *)pw->opts.workdir);
    free(pw->runner);
    free(pw);
}
