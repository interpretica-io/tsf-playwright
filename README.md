# tsf-playwright

Browser control for the OKTET Labs Test Environment (TE) through
[Playwright](https://playwright.dev/), packaged as an external TE
repository (consumed with the `TE_EXT_REPO` builder directive).

The library:

- `tapi_playwright` — engine-side TAPI. A test opens a browser on a
  Test Agent and drives it step by step: `tapi_playwright_goto()`,
  `tapi_playwright_click()`, `tapi_playwright_fill()`,
  `tapi_playwright_expect_text()`, `tapi_playwright_screenshot()` and
  the rest of `tapi_playwright.h`. Each step returns a status code and
  logs what it did, so a failure points at the step. A screenshot goes
  to the engine and into the log as a test artifact.
  `tapi_playwright_run_spec()` runs a ready `playwright test` project
  on the agent and reports the counts.

The TAPI talks to `te_playwright_runner.cjs`, a Node.js script that
ships with the library. The TAPI uploads the script to the agent with
RCF and runs it as a `tapi_job`; the two exchange one JSON line per
command.

## Agent host requirements

- Node.js 20 or newer (`node` on `PATH`, or name it in
  `tapi_playwright_opts.node`);
- the `playwright` npm package and a browser it can launch:

  ```sh
  mkdir -p /opt/playwright && cd /opt/playwright
  npm init -y && npm install playwright
  npx playwright install --with-deps chromium
  ```

  Name that directory in `tapi_playwright_opts.playwright_dir`. Without
  it node resolves `playwright` the usual way, from the directory of
  the uploaded runner (`workdir`, `/tmp` by default) upwards.

The test needs an RPC server on the agent for the job factory
(`tapi_job_factory_rpc_create()`): the runner needs stdin and stdout
channels, which the Configurator job factory does not provide.

## Usage

Declare the repository in an external libraries catalog and pass it to
`dispatcher.sh --external=<catalog.yml>`:

```yaml
repositories:
  - name: tsf_playwright
    url: https://github.com/interpretica-io/tsf-playwright.git
    ref: v1.0.0
    libs:
      - tapi_playwright
```

Bind the library to the engine platform in `builder.conf`:

```
TE_EXT_REPO_USE([tsf_playwright], [], [tapi_playwright])
```

Add `tapi_playwright` to the `te_libs` of the test suite and write the
test:

```c
#include "tapi_playwright.h"
#include "tapi_job_factory_rpc.h"

    CHECK_RC(rcf_rpc_server_create(ta, "pco_browser", &rpcs));
    CHECK_RC(tapi_job_factory_rpc_create(rpcs, &factory));

    opts = tapi_playwright_default_opts;
    opts.playwright_dir = "/opt/playwright";
    CHECK_RC(tapi_playwright_create(factory, &opts, &pw));
    CHECK_RC(tapi_playwright_start(pw));

    TEST_STEP("Log in");
    CHECK_RC(tapi_playwright_goto(pw, "http://192.168.1.1/"));
    CHECK_RC(tapi_playwright_fill(pw, "#user", "admin"));
    CHECK_RC(tapi_playwright_fill(pw, "#password", password));
    CHECK_RC(tapi_playwright_click(pw, "button[type=submit]"));
    CHECK_RC(tapi_playwright_expect_text(pw, "h1", "Status"));
    CHECK_RC(tapi_playwright_screenshot(pw, "status", NULL));

cleanup:
    tapi_playwright_destroy(pw);
```

Selectors are Playwright selectors: CSS, `text=...`, `xpath=...`,
`role=...`.

## Protocol

The runner reads one JSON object per line from stdin and writes one
per line to stdout:

```
{"id":1,"cmd":"goto","url":"http://example.com/"}
{"id":1,"ok":true,"value":"200"}
{"id":2,"cmd":"text","selector":"#missing"}
{"id":2,"ok":false,"error":"Timeout 30000ms exceeded."}
```

`value` is a string in every response; for `eval` it holds the JSON
text of the result. The commands are `launch`, `goto`, `click`,
`fill`, `press`, `select`, `wait_for`, `wait_load`, `text`, `attr`,
`count`, `eval`, `screenshot`, `url`, `title`, `content`, `run_spec`
and `close`; the script lists their arguments.

## Tests

`tests/runner.sh` drives the runner with node against a static page
and checks the responses. It needs a directory with
`node_modules/playwright` and Chromium:

```sh
PLAYWRIGHT_DIR=/opt/playwright ./tests/runner.sh
```

Requires TE with `TE_EXT_REPO` support.
