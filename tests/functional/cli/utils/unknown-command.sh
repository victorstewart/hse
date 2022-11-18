#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
#
# Copyright (C) 2021-2022 Micron Technology, Inc. All rights reserved.

. common.subr

trap cleanup EXIT

output=$(cmd -e hse utils does-not-exist 2>&1)

echo "$output" | cmd grep -F "hse utils: invalid command 'does-not-exist', use -h for help"
