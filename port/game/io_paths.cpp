/*
 * Path translation for Shadow Guardian HD 1.0.1.
 *
 * The engine builds every path itself; Java hands it nothing. Two roots, both
 * read out of the library's .rodata (`strings -a -t x libshadowguardian.so`):
 *
 *   360924  /sdcard/gameloft/games/GloftSGHP                 the data ("%s/" + name)
 *   3606cc  /data/data/com.gameloft.android.ANMP.GloftSGHP.ML  private files
 *
 * The first is where the original installer (or a player, per INSTALL.txt)
 * put the cache; here it maps to <game>/GloftSGHP, with the cache archive
 * mounted over it (game/zip_mount.cpp). The second maps to the writable
 * directory portbase picks.
 */

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "io_paths.h"
#include "trace.h"

#include "sg.h"

static const char kSdcard[]   = "/sdcard/gameloft/games/GloftSGHP";
static const char kDataData[] = "/data/data/com.gameloft.android.ANMP.GloftSGHP.ML";

const char *sg_data_dir(void)
{
    static char dir[PATH_MAX];
    if (!dir[0])
        snprintf(dir, sizeof(dir), "%s/GloftSGHP", io_game_dir());
    return dir;
}

static const char *reroot(const char *path, size_t prefix_len, const char *root,
                          char *buf, size_t bufsz)
{
    const char *tail = path + prefix_len;
    while (*tail == '/')
        tail++;
    if (*tail)
        snprintf(buf, bufsz, "%s/%s", root, tail);
    else
        snprintf(buf, bufsz, "%s", root);
    return buf;
}

static bool has_prefix(const char *path, const char *prefix, size_t n)
{
    return strncmp(path, prefix, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

const char *port_fix_path(const char *orig, char *buf, size_t bufsz)
{
    if (!orig || !*orig)
        return NULL;

    /* The engine also reaches some files as "./x" relative names; portbase's
     * fallback resolves those against the game directory. */
    if (has_prefix(orig, kSdcard, sizeof(kSdcard) - 1)) {
        reroot(orig, sizeof(kSdcard) - 1, sg_data_dir(), buf, bufsz);
        char mounted[PATH_MAX];
        const char *m = sg_zip_mount_resolve(buf, mounted, sizeof(mounted));
        if (m)
            snprintf(buf, bufsz, "%s", m);
        return buf;
    }

    if (has_prefix(orig, kDataData, sizeof(kDataData) - 1))
        return reroot(orig, sizeof(kDataData) - 1, io_writable_dir(), buf, bufsz);

    /* A name already under the data directory (the engine keeps what it got
     * and re-joins it) still has to see the archive. */
    const char *data = sg_data_dir();
    if (has_prefix(orig, data, strlen(data)))
        return sg_zip_mount_resolve(orig, buf, bufsz);

    /* Relative names: on the phone the engine's working directory is the data
     * directory, so "music/x.wav" means <data>/music/x.wav - on disk or in the
     * archive. Anything else keeps portbase's fallback (the game directory). */
    if (orig[0] != '/') {
        const char *rel = orig;
        while (rel[0] == '.' && rel[1] == '/')
            rel += 2;
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", data, rel);
        struct stat st;
        if (stat(full, &st) == 0) {
            snprintf(buf, bufsz, "%s", full);
            return buf;
        }
        const char *m = sg_zip_mount_resolve(full, buf, bufsz);
        if (m)
            return m;
        static int shown;
        if (shown++ < 16)
            trace("io: relative name '%s' is not in the data directory", orig);
    }

    return NULL;
}

/*
 * The same names, opened for writing: always a file on disk - never the
 * archive's memory copy, or the engine's settings and saves would vanish with
 * the process. With `keep` (r+, append) the archived contents are copied up
 * first. Directories on the way are created, as the original install had them.
 */
extern "C" const char *port_fix_path_write(const char *orig, char *buf, size_t bufsz, int keep)
{
    if (!orig || !*orig)
        return NULL;
    const char *data = sg_data_dir();
    if (has_prefix(orig, kSdcard, sizeof(kSdcard) - 1))
        reroot(orig, sizeof(kSdcard) - 1, data, buf, bufsz);
    else if (has_prefix(orig, kDataData, sizeof(kDataData) - 1))
        return reroot(orig, sizeof(kDataData) - 1, io_writable_dir(), buf, bufsz);
    else if (has_prefix(orig, data, strlen(data)))
        snprintf(buf, bufsz, "%s", orig);
    else if (orig[0] != '/') {
        const char *rel = orig;
        while (rel[0] == '.' && rel[1] == '/')
            rel += 2;
        snprintf(buf, bufsz, "%s/%s", data, rel);
    } else
        return NULL;

    if (keep)
        sg_zip_mount_copy_up(buf);
    return buf;
}

/* The engine names no /system/fonts path. */
const char *port_system_font(void)
{
    return NULL;
}

/* The files the game has written so far (its saves and settings), so a log
 * says what state a run started from. Archive listings are skipped. */
static void list_written(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent64 *e;
    while ((e = readdir64(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 3)
                list_written(p, depth + 1);
        } else {
            trace("storage: %s (%lld bytes)", p, (long long)st.st_size);
        }
    }
    closedir(d);
}

void sg_prepare_storage(void)
{
    mkdir(sg_data_dir(), 0777);
    mkdir(io_writable_dir(), 0777);
    list_written(sg_data_dir(), 0);
    list_written(io_writable_dir(), 0);
}
