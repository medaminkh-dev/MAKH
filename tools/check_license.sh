#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
# check_license.sh - every tracked source file must carry the SPDX header.
set -u
missing=$(git ls-files '*.c' '*.h' '*.asm' '*.ld' '*.py' '*.sh' 'Makefile' \
          | xargs grep -L "SPDX-License-Identifier: AGPL-3.0-only")
if [ -n "$missing" ]; then
    echo "missing SPDX header:"; echo "$missing"; exit 1
fi
echo "license headers: ok"
