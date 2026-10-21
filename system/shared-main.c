/*
 * Entry point of libqemu-system-<target>-shared.so (meson option
 * system_shared_lib), for hosts that run QEMU inside their own process
 * instead of exec'ing it, such as the ios-on-pixel Android app.
 *
 * It is system/main.c with main() renamed, so the executable's own build of
 * main.c stays untouched. Like main(), qemu_android_main() ends in exit().
 */
#define main qemu_android_main
int main(int argc, char** argv);
#include "main.c"
