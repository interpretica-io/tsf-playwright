/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Playwright TAPI
 *
 * @defgroup tapi_playwright Browser control with Playwright (tapi_playwright)
 * @{
 *
 * Engine-side TAPI that drives a web browser on a Test Agent through
 * Playwright. The library starts a Node.js runner on the agent as a
 * job and sends it one command per step: open a page, click, fill a
 * field, read a text, take a screenshot. Each step returns a status
 * code, so a test can log and check every step on its own.
 *
 * The agent host needs Node.js, the 'playwright' npm package and the
 * browsers it downloads ('npx playwright install chromium'). The
 * runner script ships with the library; tapi_playwright_start()
 * uploads it to the agent.
 *
 * @code
 * tapi_job_factory_t *factory;
 * tapi_playwright *pw;
 * tapi_playwright_opts opts = tapi_playwright_default_opts;
 *
 * CHECK_RC(tapi_job_factory_rpc_create(rpcs, &factory));
 * opts.playwright_dir = "/opt/playwright";
 * CHECK_RC(tapi_playwright_create(factory, &opts, &pw));
 * CHECK_RC(tapi_playwright_start(pw));
 * CHECK_RC(tapi_playwright_goto(pw, "http://192.168.1.1/"));
 * CHECK_RC(tapi_playwright_fill(pw, "#user", "admin"));
 * CHECK_RC(tapi_playwright_click(pw, "button[type=submit]"));
 * CHECK_RC(tapi_playwright_expect_text(pw, "h1", "Status"));
 * CHECK_RC(tapi_playwright_screenshot(pw, "status", NULL));
 * tapi_playwright_destroy(pw);
 * @endcode
 */

#ifndef __TAPI_PLAYWRIGHT_H__
#define __TAPI_PLAYWRIGHT_H__

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "tapi_job.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Browser to launch. */
typedef enum tapi_playwright_browser {
    TAPI_PLAYWRIGHT_CHROMIUM,
    TAPI_PLAYWRIGHT_FIREFOX,
    TAPI_PLAYWRIGHT_WEBKIT,
} tapi_playwright_browser;

/** State of an element to wait for, see tapi_playwright_wait_for(). */
typedef enum tapi_playwright_state {
    TAPI_PLAYWRIGHT_ATTACHED,   /**< Present in the DOM */
    TAPI_PLAYWRIGHT_DETACHED,   /**< Absent from the DOM */
    TAPI_PLAYWRIGHT_VISIBLE,    /**< Present and visible */
    TAPI_PLAYWRIGHT_HIDDEN,     /**< Absent or not visible */
} tapi_playwright_state;

/** Load state of a page, see tapi_playwright_wait_load(). */
typedef enum tapi_playwright_load {
    TAPI_PLAYWRIGHT_LOAD,           /**< The 'load' event fired */
    TAPI_PLAYWRIGHT_DOMCONTENTLOADED, /**< The DOM is ready */
    TAPI_PLAYWRIGHT_NETWORKIDLE,    /**< No network traffic for 500 ms */
} tapi_playwright_load;

/** Options of a browser session. */
typedef struct tapi_playwright_opts {
    /** Node.js executable on the agent (@c NULL: "node"). */
    const char *node;
    /**
     * Directory on the agent that holds node_modules/playwright
     * (@c NULL: node resolves the module the usual way).
     */
    const char *playwright_dir;
    /**
     * Directory with the browsers on the agent, the value of
     * PLAYWRIGHT_BROWSERS_PATH (@c NULL: the Playwright default).
     */
    const char *browsers_path;
    /**
     * Runner script on the agent (@c NULL: the library uploads the
     * script installed with it).
     */
    const char *runner;
    /**
     * Directory on the agent for the uploaded runner and the
     * screenshots (@c NULL: "/tmp").
     */
    const char *workdir;
    /** Browser to launch. */
    tapi_playwright_browser browser;
    /** Run the browser with a window (@c false: headless). */
    bool headed;
    /** Timeout of one action in milliseconds (0: 30000). */
    unsigned int timeout_ms;
} tapi_playwright_opts;

/** Default options: headless Chromium, runner from the installation. */
extern const tapi_playwright_opts tapi_playwright_default_opts;

/** Browser session handle. */
typedef struct tapi_playwright tapi_playwright;

/** Result of a 'playwright test' run, see tapi_playwright_run_spec(). */
typedef struct tapi_playwright_spec_result {
    unsigned int passed;    /**< Tests that passed */
    unsigned int failed;    /**< Tests that failed */
    unsigned int skipped;   /**< Tests that were skipped */
    unsigned int flaky;     /**< Tests that passed on a retry */
    /** Titles of the failed tests, separated by "; " (may be empty). */
    te_string failures;
} tapi_playwright_spec_result;

/**
 * Create a browser session on the agent of @p factory.
 *
 * The runner job exists after this call; nothing runs until
 * tapi_playwright_start().
 *
 * @param      factory  Job factory (RPC factory: the runner needs
 *                      stdin and stdout channels)
 * @param      opts     Options (@c NULL: tapi_playwright_default_opts)
 * @param[out] pw       Session handle
 *
 * @return Status code.
 */
extern te_errno tapi_playwright_create(tapi_job_factory_t *factory,
                                       const tapi_playwright_opts *opts,
                                       tapi_playwright **pw);

/**
 * Upload the runner when needed, start it and launch the browser with
 * one page.
 *
 * @param pw    Session handle
 *
 * @return Status code.
 */
extern te_errno tapi_playwright_start(tapi_playwright *pw);

/**
 * Navigate the page to @p url and wait for the 'load' event.
 *
 * @param pw    Session handle
 * @param url   URL
 *
 * @return Status code.
 */
extern te_errno tapi_playwright_goto(tapi_playwright *pw, const char *url);

/**
 * Click the element that @p selector matches.
 *
 * @param pw        Session handle
 * @param selector  Playwright selector (CSS, text=, xpath=, ...)
 *
 * @return Status code.
 */
extern te_errno tapi_playwright_click(tapi_playwright *pw,
                                      const char *selector);

/**
 * Clear the input that @p selector matches and type @p text into it.
 *
 * @param pw        Session handle
 * @param selector  Playwright selector
 * @param text      Text to fill in
 *
 * @return Status code.
 */
extern te_errno tapi_playwright_fill(tapi_playwright *pw,
                                     const char *selector,
                                     const char *text);

/**
 * Press a key on the element that @p selector matches.
 *
 * @param pw        Session handle
 * @param selector  Playwright selector
 * @param key       Key name, for example "Enter" or "Control+a"
 *
 * @return Status code.
 */
extern te_errno tapi_playwright_press(tapi_playwright *pw,
                                      const char *selector,
                                      const char *key);

/**
 * Select the option with value @p value in the select element that
 * @p selector matches.
 *
 * @param pw        Session handle
 * @param selector  Playwright selector
 * @param value     Option value
 *
 * @return Status code.
 */
extern te_errno tapi_playwright_select(tapi_playwright *pw,
                                       const char *selector,
                                       const char *value);

/**
 * Wait until the element that @p selector matches reaches @p state.
 *
 * @param pw        Session handle
 * @param selector  Playwright selector
 * @param state     State to wait for
 *
 * @return Status code (TE_ETIMEDOUT when the action timeout passes).
 */
extern te_errno tapi_playwright_wait_for(tapi_playwright *pw,
                                         const char *selector,
                                         tapi_playwright_state state);

/**
 * Wait until the page reaches @p state.
 *
 * @param pw     Session handle
 * @param state  Load state to wait for
 *
 * @return Status code.
 */
extern te_errno tapi_playwright_wait_load(tapi_playwright *pw,
                                          tapi_playwright_load state);

/**
 * Get the inner text of the element that @p selector matches.
 *
 * @param      pw        Session handle
 * @param      selector  Playwright selector
 * @param[out] text      Text (appended to the string)
 *
 * @return Status code.
 */
extern te_errno tapi_playwright_get_text(tapi_playwright *pw,
                                         const char *selector,
                                         te_string *text);

/**
 * Get an attribute of the element that @p selector matches.
 *
 * @param      pw        Session handle
 * @param      selector  Playwright selector
 * @param      name      Attribute name
 * @param[out] value     Attribute value (appended to the string)
 *
 * @return Status code (TE_ENOENT when the element has no such attribute).
 */
extern te_errno tapi_playwright_get_attr(tapi_playwright *pw,
                                         const char *selector,
                                         const char *name,
                                         te_string *value);

/**
 * Count the elements that @p selector matches.
 *
 * @param      pw        Session handle
 * @param      selector  Playwright selector
 * @param[out] count     Number of elements
 *
 * @return Status code.
 */
extern te_errno tapi_playwright_count(tapi_playwright *pw,
                                      const char *selector,
                                      unsigned int *count);

/**
 * Evaluate a JavaScript expression in the page.
 *
 * @param      pw      Session handle
 * @param      script  Expression, for example "document.title"
 * @param[out] json    Result as JSON text (appended to the string);
 *                     "null" when the expression has no value
 *
 * @return Status code.
 */
extern te_errno tapi_playwright_eval(tapi_playwright *pw,
                                     const char *script,
                                     te_string *json);

/**
 * Get the current URL of the page.
 *
 * @param      pw   Session handle
 * @param[out] url  URL (appended to the string)
 *
 * @return Status code.
 */
extern te_errno tapi_playwright_get_url(tapi_playwright *pw, te_string *url);

/**
 * Get the title of the page.
 *
 * @param      pw     Session handle
 * @param[out] title  Title (appended to the string)
 *
 * @return Status code.
 */
extern te_errno tapi_playwright_get_title(tapi_playwright *pw,
                                          te_string *title);

/**
 * Check that the inner text of the element that @p selector matches
 * contains @p expected. On a mismatch the function logs both texts.
 *
 * @param pw        Session handle
 * @param selector  Playwright selector
 * @param expected  Expected substring
 *
 * @return Status code (TE_EFAIL on a mismatch).
 */
extern te_errno tapi_playwright_expect_text(tapi_playwright *pw,
                                            const char *selector,
                                            const char *expected);

/**
 * Take a screenshot of the page, copy it to the engine and log it as
 * a test artifact.
 *
 * The file goes to TE_LOG_DIR (or TE_TMP without it) under a unique
 * name that ends with "_<name>.png".
 *
 * @param      pw    Session handle
 * @param      name  Name for the log and the file
 * @param[out] path  Path of the copy on the engine (appended to the
 *                   string; may be @c NULL)
 *
 * @return Status code.
 */
extern te_errno tapi_playwright_screenshot(tapi_playwright *pw,
                                           const char *name,
                                           te_string *path);

/**
 * Run 'playwright test' in a project on the agent and report the
 * counts from its JSON report.
 *
 * The project must have '@playwright/test' installed; the runner
 * starts it with 'npx playwright test'. The session does not need a
 * launched browser for this call.
 *
 * @param      pw           Session handle
 * @param      project_dir  Project directory on the agent
 * @param      spec         Spec file or directory relative to the
 *                          project (@c NULL: the whole project)
 * @param      grep         Test title pattern (@c NULL: all tests)
 * @param[out] result       Counts and failures; free @a failures
 *                          with te_string_free()
 *
 * @return Status code (TE_EFAIL when some test failed).
 */
extern te_errno tapi_playwright_run_spec(tapi_playwright *pw,
                                         const char *project_dir,
                                         const char *spec,
                                         const char *grep,
                                         tapi_playwright_spec_result *result);

/**
 * Close the browser and wait for the runner to exit.
 *
 * @param pw    Session handle
 *
 * @return Status code.
 */
extern te_errno tapi_playwright_stop(tapi_playwright *pw);

/**
 * Stop the session when it runs, remove the uploaded runner and free
 * the handle.
 *
 * @param pw    Session handle (may be @c NULL)
 */
extern void tapi_playwright_destroy(tapi_playwright *pw);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TAPI_PLAYWRIGHT_H__ */

/**@} <!-- END tapi_playwright --> */
