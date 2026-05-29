#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved.
//
// Test of the runner's network/HAR/trace capture. A bait page leaks a
// canary token out through fetch() (in the request body) and gets it
// echoed back (in the response body); the test asserts that:
//   - network_grep finds the canary (whichever channel carried it),
//   - network_dump has the request with the plaintext bodies,
//   - trace_stop writes a non-empty trace zip,
//   - a HAR is written on close and contains the canary.
// The browser hands us the plaintext before TLS, so this is exactly the
// leak-detection an strace on the same process could not do through an
// HTTPS connection.
//
// It needs node and a directory with node_modules/playwright plus a
// Chromium that Playwright can find. Pass the directory in
// PLAYWRIGHT_DIR (default: the tests directory).
//
// Usage: PLAYWRIGHT_DIR=<dir> node capture_test.cjs [<runner.cjs>]
'use strict';

const http = require('http');
const os = require('os');
const path = require('path');
const fs = require('fs');
const { spawn } = require('child_process');

const RUNNER = process.argv[2] ||
    path.join(__dirname, '..', 'tapi_playwright', 'te_playwright_runner.cjs');
const PLAYWRIGHT_DIR = process.env.PLAYWRIGHT_DIR || __dirname;
const CANARY = 'ZZCANARY7f3a9c';
const WORK = fs.mkdtempSync(path.join(os.tmpdir(), 'pw-capture-'));
const harPath = path.join(WORK, 'session.har');
const tracePath = path.join(WORK, 'trace.zip');

const server = http.createServer((req, res) => {
    if (req.url === '/collect') {
        let body = '';
        req.on('data', (d) => { body += d; });
        req.on('end', () => {
            res.setHeader('content-type', 'application/json');
            res.end(JSON.stringify({ ok: true, echo: CANARY + '-back' }));
        });
        return;
    }
    res.setHeader('content-type', 'text/html');
    res.end(`<!DOCTYPE html><html><head><title>bait</title></head><body>
      <h1 id="t">bait</h1>
      <script>
        fetch('/collect', { method: 'POST',
              headers: { 'content-type': 'application/x-www-form-urlencoded' },
              body: 'secret=${CANARY}' })
          .then(r => r.text()).then(() => { document.title = 'done'; });
      </script></body></html>`);
});

function listen() {
    return new Promise((resolve) => server.listen(0, '127.0.0.1',
        () => resolve(server.address().port)));
}

let idc = 0;
const pending = new Map();
let child;

function send(obj) {
    return new Promise((resolve, reject) => {
        const id = ++idc;
        pending.set(id, { resolve, reject });
        child.stdin.write(JSON.stringify(Object.assign({ id }, obj)) + '\n');
    });
}

async function main() {
    const port = await listen();
    child = spawn(process.execPath, [RUNNER, '--playwright=' + PLAYWRIGHT_DIR],
                  { stdio: ['pipe', 'pipe', 'inherit'] });
    let buf = '';
    child.stdout.on('data', (d) => {
        buf += d;
        let nl;
        while ((nl = buf.indexOf('\n')) !== -1) {
            const line = buf.slice(0, nl).trim();
            buf = buf.slice(nl + 1);
            if (line === '') continue;
            const r = JSON.parse(line);
            const p = pending.get(r.id);
            if (p) {
                pending.delete(r.id);
                r.ok ? p.resolve(r.value) : p.reject(new Error(r.error));
            }
        }
    });

    const fails = [];
    const check = (cond, msg) => {
        console.log((cond ? '  ok: ' : 'FAIL: ') + msg);
        if (!cond) fails.push(msg);
    };

    await send({ cmd: 'launch', browser: 'chromium', headless: true,
                 capture_network: true, record_trace: true,
                 har_path: harPath });
    await send({ cmd: 'goto', url: `http://127.0.0.1:${port}/` });
    await send({ cmd: 'wait_load', state: 'networkidle' });

    const grep = await send({ cmd: 'network_grep', needle: CANARY });
    check(Number(grep) >= 2,
          `network_grep found the canary ${grep} time(s)`);

    const dump = JSON.parse(await send({ cmd: 'network_dump' }));
    const collect = dump.find((e) => e.url && e.url.endsWith('/collect'));
    check(!!collect, 'network_dump has the /collect request');
    check(collect && collect.method === 'POST', 'method recorded as POST');
    check(collect && collect.request_body.includes(CANARY),
          'captured request body carries the canary (plaintext egress)');
    check(collect && collect.response_body &&
          collect.response_body.includes(CANARY),
          'captured response body carries the echoed canary');

    await send({ cmd: 'trace_stop', path: tracePath });
    check(fs.existsSync(tracePath) && fs.statSync(tracePath).size > 0,
          'trace zip written');

    await send({ cmd: 'close' });
    check(fs.existsSync(harPath), 'HAR written on close');
    if (fs.existsSync(harPath)) {
        check(fs.readFileSync(harPath, 'utf8').includes(CANARY),
              'HAR contains the canary');
    }

    server.close();
    fs.rmSync(WORK, { recursive: true, force: true });
    if (fails.length === 0) {
        console.log('\nPASS: capture works');
        process.exit(0);
    }
    console.log(`\nFAILED: ${fails.length} check(s)`);
    process.exit(1);
}

main().catch((e) => { console.error(e); process.exit(2); });
