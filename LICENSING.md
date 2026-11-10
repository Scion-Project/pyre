# Licensing

This file is a plain-English map of who may use what under which licence. It does not replace the licence files.

The repository is a fork of ChefKiss Inferno, itself a QEMU fork; its top-level `LICENSE`/`COPYING*` files (GPL-2.0, AGPL-3.0 and the others QEMU carries) are not changed.

## Original work: MIT-0

Files that the Scion project wrote entirely from scratch (every commit by the project's authors, no third-party notice, nothing ported or derived from another codebase) are licensed **MIT-0** (MIT No Attribution, SPDX `MIT-0`; text in `LICENSES/MIT-0.txt`). Each such file carries `SPDX-License-Identifier: MIT-0` in its first lines, except the files listed in the table below, which cannot hold a comment.

MIT-0 paths (top-level location, number of files):

- `.github`: 2 files
- `docs`: 5 files

The full per-file list is in the pull request that introduced this file, and every file's own first lines are authoritative.

## Files that keep their existing licence

- Anything not listed above, and any file that has no explicit `SPDX-License-Identifier: MIT-0` tag, stays under the repository's existing licence, as before.
- Everything else in the tree: QEMU and ChefKiss Inferno files, and the project's own new C/assembly files that live inside that tree and carry GPL-2.0-or-later, LGPL-2.1-or-later or AGPL-3.0-or-later notices, are left as they are (see the PR).

