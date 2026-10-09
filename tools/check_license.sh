#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
# check_license.sh - every tracked source file must carry the SPDX header.
set -u
# user/sysroot/ is a vendored musl sysroot (MIT, a separate work); its headers
# carry musl's own license, not MAKH's AGPL, so exclude the whole subtree.
missing=$(git ls-files '*.c' '*.h' '*.asm' '*.ld' '*.py' '*.sh' 'Makefile' \
          ':(exclude)user/sysroot' \
          | xargs grep -L "SPDX-License-Identifier: AGPL-3.0-only")
if [ -n "$missing" ]; then
    echo "missing SPDX header:"; echo "$missing"; exit 1
fi
echo "license headers: ok"
