#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved.
#
# Test of te_playwright_runner.cjs: the protocol, the commands and the
# error reports, against a static page in a temporary directory. It
# needs node and a directory with node_modules/playwright plus a
# Chromium that Playwright can find. Pass the directory in
# PLAYWRIGHT_DIR (default: the directory of this test).
#
# Usage: PLAYWRIGHT_DIR=<dir> ./runner.sh

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RUNNER="${SCRIPT_DIR}/../tapi_playwright/te_playwright_runner.cjs"
PLAYWRIGHT_DIR="${PLAYWRIGHT_DIR:-${SCRIPT_DIR}}"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

failures=0

fail() {
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}

ok() {
    echo "  ok: $*"
}

cat >"${WORK}/page.html" <<'EOF'
<!DOCTYPE html>
<html>
<head><title>Runner test page</title></head>
<body>
  <h1 id="title">Hello, TE</h1>
  <input id="name" type="text">
  <select id="choice">
    <option value="a">A</option>
    <option value="b">B</option>
  </select>
  <button id="go" onclick="document.getElementById('out').innerText =
      'Name: ' + document.getElementById('name').value +
      ', choice: ' + document.getElementById('choice').value">Go</button>
  <p id="out"></p>
  <ul><li>one</li><li>two</li><li>three</li></ul>
  <a id="link" href="/next" data-x="42">next</a>
</body>
</html>
EOF

# Requests, one per line. The last one closes the runner.
cat >"${WORK}/requests" <<EOF
{"id":1,"cmd":"launch","browser":"chromium","headless":true,"timeout":5000}
{"id":2,"cmd":"goto","url":"file://${WORK}/page.html"}
{"id":3,"cmd":"title"}
{"id":4,"cmd":"text","selector":"#title"}
{"id":5,"cmd":"fill","selector":"#name","text":"Ann \"Quote\" \\\\ tab\\tend"}
{"id":6,"cmd":"select","selector":"#choice","value":"b"}
{"id":7,"cmd":"click","selector":"#go"}
{"id":8,"cmd":"text","selector":"#out"}
{"id":9,"cmd":"count","selector":"li"}
{"id":10,"cmd":"attr","selector":"#link","name":"data-x"}
{"id":11,"cmd":"attr","selector":"#link","name":"missing"}
{"id":12,"cmd":"eval","script":"[1, 'x', {k: null}]"}
{"id":13,"cmd":"wait_for","selector":"#nosuch","state":"visible"}
{"id":14,"cmd":"nosuch"}
{"id":15,"cmd":"screenshot","path":"${WORK}/shot.png"}
{"id":16,"cmd":"url"}
{"id":17,"cmd":"close"}
EOF

node "${RUNNER}" "--playwright=${PLAYWRIGHT_DIR}" \
    <"${WORK}/requests" >"${WORK}/responses" 2>"${WORK}/stderr"
status=$?

echo "=== runner exit status"
test "${status}" -eq 0 && ok "exit status 0" || fail "exit status ${status}"

# Check that the response to request $1 is exactly $2.
expect() {
    local id="$1"; shift
    local want="$1"; shift
    local got

    got="$(grep "^{\"id\":${id}," "${WORK}/responses")"
    if test "${got}" = "${want}" ; then
        ok "response ${id}"
    else
        fail "response ${id} is ${got}, expected ${want}"
    fi
}

# Check that the response to request $1 is an error whose message
# starts with $2.
expect_error() {
    local id="$1"; shift
    local prefix="$1"; shift
    local got

    got="$(grep "^{\"id\":${id}," "${WORK}/responses")"
    case "${got}" in
        "{\"id\":${id},\"ok\":false,\"error\":\"${prefix}"*)
            ok "response ${id} is an error: ${prefix}" ;;
        *)
            fail "response ${id} is ${got}, expected an error '${prefix}...'" ;;
    esac
}

echo "=== responses"
case "$(grep '^{"id":1,' "${WORK}/responses")" in
    '{"id":1,"ok":true,"value":"chromium '*) ok "launch reports the browser" ;;
    *) fail "launch: $(grep '^{"id":1,' "${WORK}/responses")" ;;
esac
expect 2 '{"id":2,"ok":true,"value":"200"}'
expect 3 '{"id":3,"ok":true,"value":"Runner test page"}'
expect 4 '{"id":4,"ok":true,"value":"Hello, TE"}'
expect 5 '{"id":5,"ok":true,"value":""}'
expect 6 '{"id":6,"ok":true,"value":"b"}'
expect 7 '{"id":7,"ok":true,"value":""}'
# innerText renders the tab as a space; the value itself keeps it
expect 8 '{"id":8,"ok":true,"value":"Name: Ann \"Quote\" \\ tab end, choice: b"}'
expect 9 '{"id":9,"ok":true,"value":"3"}'
expect 10 '{"id":10,"ok":true,"value":"42"}'
expect_error 11 "no attribute 'missing' on '#link'"
expect 12 '{"id":12,"ok":true,"value":"[1,\"x\",{\"k\":null}]"}'
expect_error 13 "page.waitForSelector: Timeout 5000ms exceeded"
expect_error 14 "unknown command 'nosuch'"
expect 15 "{\"id\":15,\"ok\":true,\"value\":\"${WORK}/shot.png\"}"
test -s "${WORK}/shot.png" && ok "screenshot file exists" ||
    fail "no screenshot file"
expect 16 "{\"id\":16,\"ok\":true,\"value\":\"file://${WORK}/page.html\"}"
expect 17 '{"id":17,"ok":true,"value":""}'

echo "=== every response is one line with the expected shape"
bad="$(grep -v -E '^\{"id":[0-9]+,"ok":(true,"value"|false,"error"):"([^"\\]|\\.)*"\}$' \
        "${WORK}/responses")"
test -z "${bad}" && ok "all $(wc -l <"${WORK}/responses" | tr -d ' ') responses match" ||
    fail "unexpected output: ${bad}"

grep -q "te_playwright_runner: ready" "${WORK}/stderr" && ok "ready line on stderr" ||
    fail "no ready line on stderr"

echo
if test "${failures}" -eq 0 ; then
    echo "PASS: all checks succeeded"
    exit 0
fi
echo "FAILED: ${failures} check(s)"
exit 1
