/*
 * Stand-in for QEMU's qemu/osdep.h when a hw/ source that only needs the
 * standard C headers is built into a host-side test (see run-host-tests.sh).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
