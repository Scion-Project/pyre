/*
 * Host API of the Android display backend (ui/android.c), exported from
 * libqemu-system-<target>-shared.so for hosts that run QEMU in-process.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef UI_ANDROID_H
#define UI_ANDROID_H

typedef struct ANativeWindow ANativeWindow;

void qemu_android_display_set_window(ANativeWindow* window);
void qemu_android_display_set_resize_cb(void (*cb)(int w, int h));
void qemu_android_input_touch(int x, int y, int w, int h, int down);
int  qemu_android_input_key(const char* name, int down);

#endif
