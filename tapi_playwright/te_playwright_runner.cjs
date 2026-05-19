#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved.
//
// Playwright runner for the TE tapi_playwright library.
//
// The library starts this script on a Test Agent as a job and talks to
// it over stdin and stdout, one JSON object per line:
//
//   request:  {"id": 1, "cmd": "goto", "url": "http://..."}
//   response: {"id": 1, "ok": true, "value": "<string>"}
//             {"id": 1, "ok": false, "error": "<message>"}
//
// 'value' is a JSON string in every response. For 'eval' it holds the
// JSON text of the result. The runner handles requests one at a time,
// in order. It exits after 'close' or when stdin ends.
//
// Usage: node te_playwright_runner.cjs [--playwright=<dir>]
//                                      [--browsers-path=<dir>]
//
// --playwright names a directory that holds node_modules/playwright;
// without it node resolves the module the usual way. --browsers-path
// sets PLAYWRIGHT_BROWSERS_PATH before the module loads.

'use strict';

const path = require('path');
const readline = require('readline');
const { createRequire } = require('module');
const { spawn } = require('child_process');

function parseArgs(argv) {
    const args = {};

    for (const a of argv) {
        const m = /^--([a-z-]+)=(.*)$/.exec(a);
        if (!m) {
            process.stderr.write(`te_playwright_runner: bad argument '${a}'\n`);
            process.exit(2);
        }
        args[m[1]] = m[2];
    }
    return args;
}

const args = parseArgs(process.argv.slice(2));

if (args['browsers-path'])
    process.env.PLAYWRIGHT_BROWSERS_PATH = args['browsers-path'];

let playwright;
if (args.playwright) {
    const dir = path.resolve(args.playwright);
    playwright = createRequire(path.join(dir, 'package.json'))('playwright');
} else {
    playwright = require('playwright');
}

let browser = null;
let context = null;
let page = null;

function needPage() {
    if (page === null)
        throw new Error('browser is not launched');
    return page;
}

// Run 'playwright test' in a project directory and reduce the JSON
// report to one line: "passed=N failed=N skipped=N flaky=N" followed
// by the titles of the failed tests, one per "; ".
function runSpec(r) {
    return new Promise((resolve, reject) => {
        const argv = ['playwright', 'test', '--reporter=json'];

        if (r.spec)
            argv.push(r.spec);
        if (r.grep)
            argv.push('--grep', r.grep);
        if (r.project)
            argv.push('--project', r.project);

        const child = spawn('npx', argv, {
            cwd: r.project_dir,
            env: Object.assign({}, process.env, { CI: '1' }),
            stdio: ['ignore', 'pipe', 'pipe'],
        });
        let out = '';
        let err = '';

        child.stdout.on('data', (d) => { out += d; });
        child.stderr.on('data', (d) => { err += d; });
        child.on('error', reject);
        child.on('close', () => {
            let report;

            try {
                report = JSON.parse(out);
            } catch (e) {
                reject(new Error('playwright test gave no JSON report: ' +
                                 (err.trim().split('\n')[0] || e.message)));
                return;
            }

            const counts = { passed: 0, failed: 0, skipped: 0, flaky: 0 };
            const failures = [];
            const walk = (suite, titles) => {
                for (const spec of suite.specs || []) {
                    for (const test of spec.tests || []) {
                        const status = test.status;
                        const title = titles.concat(spec.title).join(' > ');

                        if (status === 'expected')
                            counts.passed++;
                        else if (status === 'skipped')
                            counts.skipped++;
                        else if (status === 'flaky')
                            counts.flaky++;
                        else {
                            counts.failed++;
                            failures.push(title);
                        }
                    }
                }
                for (const s of suite.suites || [])
                    walk(s, titles.concat(s.title));
            };

            for (const s of report.suites || [])
                walk(s, [s.title]);

            let line = `passed=${counts.passed} failed=${counts.failed} ` +
                       `skipped=${counts.skipped} flaky=${counts.flaky}`;
            if (failures.length > 0)
                line += ' ' + failures.join('; ');
            resolve(line);
        });
    });
}

const commands = {
    async launch(r) {
        const name = r.browser || 'chromium';
        const type = playwright[name];

        if (type === undefined)
            throw new Error(`unknown browser '${name}'`);

        browser = await type.launch({ headless: r.headless !== false });
        context = await browser.newContext();
        if (r.timeout)
            context.setDefaultTimeout(r.timeout);
        page = await context.newPage();
        return `${name} ${browser.version()}`;
    },

    async goto(r) {
        const resp = await needPage().goto(r.url,
                                           { waitUntil: r.wait_until || 'load' });
        return resp === null ? '' : String(resp.status());
    },

    async click(r) {
        await needPage().click(r.selector);
        return '';
    },

    async fill(r) {
        await needPage().fill(r.selector, r.text);
        return '';
    },

    async press(r) {
        await needPage().press(r.selector, r.key);
        return '';
    },

    async select(r) {
        const chosen = await needPage().selectOption(r.selector, r.value);
        return chosen.join(',');
    },

    async wait_for(r) {
        await needPage().waitForSelector(r.selector,
                                         { state: r.state || 'visible' });
        return '';
    },

    async wait_load(r) {
        await needPage().waitForLoadState(r.state || 'load');
        return '';
    },

    async text(r) {
        return await needPage().innerText(r.selector);
    },

    async attr(r) {
        const value = await needPage().getAttribute(r.selector, r.name);

        if (value === null)
            throw new Error(`no attribute '${r.name}' on '${r.selector}'`);
        return value;
    },

    async count(r) {
        return String(await needPage().locator(r.selector).count());
    },

    async eval(r) {
        const value = await needPage().evaluate(r.script);
        return JSON.stringify(value === undefined ? null : value);
    },

    async screenshot(r) {
        await needPage().screenshot({ path: r.path, fullPage: !!r.full_page });
        return r.path;
    },

    async url() {
        return needPage().url();
    },

    async title() {
        return await needPage().title();
    },

    async content() {
        return await needPage().content();
    },

    async run_spec(r) {
        return await runSpec(r);
    },

    async close() {
        if (browser !== null)
            await browser.close();
        browser = null;
        context = null;
        page = null;
        return '';
    },
};

function respond(obj) {
    process.stdout.write(JSON.stringify(obj) + '\n');
}

function firstLine(e) {
    return String(e && e.message ? e.message : e).split('\n')[0];
}

async function handle(line) {
    let request;

    line = line.trim();
    if (line === '')
        return false;

    try {
        request = JSON.parse(line);
    } catch (e) {
        respond({ id: 0, ok: false, error: `bad request: ${e.message}` });
        return false;
    }

    const fn = commands[request.cmd];
    if (fn === undefined) {
        respond({ id: request.id, ok: false,
                  error: `unknown command '${request.cmd}'` });
        return false;
    }

    try {
        const value = await fn(request);
        respond({ id: request.id, ok: true, value: String(value) });
    } catch (e) {
        respond({ id: request.id, ok: false, error: firstLine(e) });
        return false;
    }
    return request.cmd === 'close';
}

const rl = readline.createInterface({ input: process.stdin,
                                      crlfDelay: Infinity });
let queue = Promise.resolve();

rl.on('line', (line) => {
    queue = queue.then(async () => {
        if (await handle(line)) {
            rl.close();
            process.exit(0);
        }
    });
});

rl.on('close', () => {
    queue = queue.then(async () => {
        await commands.close();
        process.exit(0);
    });
});

process.stderr.write('te_playwright_runner: ready\n');
