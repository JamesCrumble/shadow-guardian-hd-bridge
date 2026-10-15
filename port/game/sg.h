#pragma once

/*
 * What the parts of this port need from each other: the handful of facts about
 * Shadow Guardian HD 1.0.1 that more than one file uses.
 */

#include <stddef.h>

struct so_module;
struct _JNIEnv;

/* The loaded guest library (libshadowguardian.so). */
so_module *sg_module(void);

/* Every entry point lives on one of four Java classes under this package. */
#define SG_JNI(cls, m) "Java_com_gameloft_android_ANMP_GloftSGHP_ML_" cls "_" m

/* ------------------------------------------------------------ paths */

/* Where the engine's "/sdcard/gameloft/games/GloftSGHP" lands: <game>/GloftSGHP.
 * A real directory; files on disk there win over the cache archive. */
const char *sg_data_dir(void);

/* Create the writable directories the engine expects to exist. */
void sg_prepare_storage(void);

/* ------------------------------------------------------- cache archive */

/*
 * The cache archive (GloftSGHP.zip, as Gameloft shipped it for manual install)
 * mounted read-only over sg_data_dir(). See game/zip_mount.cpp.
 *
 * sg_zip_mount_open: called once from main(); `zip_path` may be NULL, then the
 * first *.zip found in the game directory is used. Returns false when there is
 * no archive (the data then has to be unpacked on disk).
 */
bool sg_zip_mount_open(const char *zip_path);

/*
 * If `path` (an absolute host path under sg_data_dir()) is not on disk but is
 * in the archive, write a path that opens the archived contents into `buf` and
 * return it. NULL means "not ours" - the caller keeps its own answer.
 */
const char *sg_zip_mount_resolve(const char *path, char *buf, size_t bufsz);

/* Before `path` (under sg_data_dir()) is opened for writing with its contents
 * kept: copy the archived file to disk. */
bool sg_zip_mount_copy_up(const char *path);

/* Counters for the summary line. */
long sg_zip_mount_served(void);
long sg_zip_mount_inflated(void);

/* -------------------------------------------------------------- JNI */

/* The fake classes handed to the four nativeInit calls (jni/sg_classes.cpp). */
void *sg_class_shadowguardian(void);
void *sg_class_renderer(void);
void *sg_class_surfaceview(void);
void *sg_class_resloader(void);
void *sg_class_device(void);

/* What the engine's layout and the renderer agree on (jni/sg_classes.cpp). */
void sg_set_view_size(int w, int h);

/* Raised by ShadowGuardian.Exit() from the game. */
bool sg_exit_requested(void);
