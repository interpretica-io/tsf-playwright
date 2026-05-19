# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved.
#
# External TE library (see TE_EXT_REPO): engine-side TAPI that drives
# a browser on a Test Agent through Playwright. Built as a shared
# library on the engine platform. The runner script that the TAPI
# uploads to the agent installs into share/tsf-playwright.

if get_option('engine')
    build_lib_shared = true
endif
build_lib_static = not get_option('engine')
install_lib = install_dev

headers += files(
    'tapi_playwright.h',
)

sources += files(
    'tapi_playwright.c',
)

te_libs += [
    'tapi_job',
    'tapi',
    'rcfapi',
    'logger_core',
    'tools',
]

if get_option('engine')
    install_data(
        files('te_playwright_runner.cjs'),
        install_dir: join_paths(get_option('datadir'), 'tsf-playwright'),
    )
endif
