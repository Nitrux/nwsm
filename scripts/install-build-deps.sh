#!/usr/bin/env bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 <Nitrux Latinoamericana S.C. <hello@nxos.org>>

set -e

if [ "$EUID" -ne 0 ]; then
    APT_COMMAND="sudo apt"
else
    APT_COMMAND="apt"
fi

$APT_COMMAND update -q
$APT_COMMAND install -y --no-install-recommends \
    build-essential \
    checkinstall \
    cmake \
    dpkg-dev
