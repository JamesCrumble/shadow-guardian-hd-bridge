/*
 * The cache archive, mounted read-only over the engine's data directory.
 *
 * Shadow Guardian reads flat files under /sdcard/gameloft/games/GloftSGHP
 * (anims/, models/, textures/, sprites_1_7, states, ...). Gameloft shipped that
 * tree as GloftSGHP.zip for a manual install: 3746 entries, 1.28 GB unpacked,
 * every one deflated. The player drops the zip next to the game and nothing is
 * unpacked to disk: when the engine opens a name under the data directory that
 * is not on disk, the entry is inflated into an anonymous memory file (memfd)
 * and the open is pointed at /proc/self/fd/<n>. Through that path open(),
 * fopen(), stat() and lseek() all behave as on a plain file - each open gets
 * its own file description and offset - so none of the libc thunks has to know
 * the archive exists.
 *
 * Why /proc/self/fd and not a dup: port_fix_path only rewrites the *name*, and
 * the thunks then call the host's open/fopen/stat with it. qemu-user passes
 * /proc/self/fd/<n> straight to the kernel (its realpath() is only used when
 * the result is under /proc, which a memfd's link target is not).
 *
 * Disk wins over the archive, file by file. A player who unpacked the cache by
 * hand, or the engine writing its own files (saves, iglu.bin) into the same
 * directory, never reaches the archive. Directories of the archive are created
 * empty on disk at mount time so that stat()/mkdir() on them behave.
 *
 * Inflated entries stay open in a small LRU (SG_ZIP_CACHE_MB, default 256), so
 * a file the engine opens, closes and opens again - it probes with stat()
 * first - is inflated once. Evicting closes only our descriptor; an open the
 * engine still holds keeps its copy alive.
 *
 * If the kernel refuses memfd or the /proc reopen, entries are inflated into
 * <writable>/zipcache instead (same overlay, costs card space).
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <map>
#include <string>
#include <unordered_map>

#include <zip.h>

#include "io_paths.h"
#include "port_env.h"
#include "trace.h"

#include "sg.h"

#ifndef SYS_memfd_create
#define SYS_memfd_create 385 /* ARM EABI */
#endif
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 1
#endif

namespace {

struct Cached {
    int       fd;          /* our descriptor (memfd or file) */
    long long bytes;
    unsigned  last_use;
};

zip_t      *g_zip;
std::string g_prefix;      /* entry-name prefix that maps to sg_data_dir(), e.g. "GloftSGHP/" */
std::string g_root;        /* sg_data_dir() */
bool        g_use_memfd;
std::string g_spill_dir;   /* fallback when memfd is unavailable */

/* Directory listings: relative directory ("" = the data directory itself) ->
 * child name -> is a directory. Built once from the archive's names. */
std::unordered_map<std::string, std::map<std::string, bool>> g_dirs;
std::string g_shadow_root; /* <writable>/.zipdirs: the listings, as empty files */

std::unordered_map<zip_int64_t, Cached> g_cache;
long long   g_cache_bytes;
long long   g_cache_budget;
unsigned    g_clock;
long        g_served;
long        g_inflated;

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

bool ends_with(const std::string &s, const char *tail)
{
    size_t n = strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

void mkdirs(const std::string &path)
{
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur += path[i];
        if (path[i] == '/' && i > 0)
            mkdir(cur.c_str(), 0777);
    }
    mkdir(path.c_str(), 0777);
}

/* The archive may hold the tree at its root or under a wrapper folder
 * ("GloftSGHP/", "gameloft/games/GloftSGHP/", ...). The engine's marker file
 * "states" sits at the top of the data directory, so whatever precedes it is
 * the prefix. */
bool find_prefix(void)
{
    zip_int64_t n = zip_get_num_entries(g_zip, 0);
    for (zip_int64_t i = 0; i < n; i++) {
        const char *name = zip_get_name(g_zip, i, 0);
        if (!name)
            continue;
        std::string s(name);
        if (s == "states" || ends_with(s, "/states")) {
            g_prefix = s.substr(0, s.size() - strlen("states"));
            return true;
        }
    }
    return false;
}

/* Every component of an archive name becomes a child of its parent. */
void index_name(const std::string &rel)
{
    size_t start = 0;
    std::string parent;
    while (start < rel.size()) {
        size_t slash = rel.find('/', start);
        bool dir = slash != std::string::npos;
        std::string part = rel.substr(start, dir ? slash - start : std::string::npos);
        if (part.empty())
            break;
        auto &kids = g_dirs[parent];
        bool &is_dir = kids[part];
        is_dir = is_dir || dir;
        if (!dir)
            break;
        parent = parent.empty() ? part : parent + "/" + part;
        start = slash + 1;
    }
}

void create_directories(void)
{
    zip_int64_t n = zip_get_num_entries(g_zip, 0);
    long made = 0;
    for (zip_int64_t i = 0; i < n; i++) {
        const char *name = zip_get_name(g_zip, i, 0);
        if (!name || strncmp(name, g_prefix.c_str(), g_prefix.size()) != 0)
            continue;
        std::string rel(name + g_prefix.size());
        index_name(rel);
        size_t slash = rel.rfind('/');
        if (slash == std::string::npos || slash == 0)
            continue;
        std::string dir = g_root + "/" + rel.substr(0, slash);
        struct stat st;
        if (stat(dir.c_str(), &st) != 0) {
            mkdirs(dir);
            made++;
        }
    }
    if (made)
        trace("zip mount: created %ld directories of the archive under %s", made, g_root.c_str());
}

/* Can a memfd be reopened by name? Checked once: SELinux policy differs
 * between Android releases, and a refusal here is better than one at the
 * first texture. */
bool probe_memfd(void)
{
    int fd = (int)syscall(SYS_memfd_create, "sg-probe", MFD_CLOEXEC);
    if (fd < 0) {
        trace("zip mount: memfd_create unavailable (%s)", strerror(errno));
        return false;
    }
    bool ok = write(fd, "ok", 2) == 2;
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    int again = ok ? open(path, O_RDONLY) : -1;
    char buf[2] = {0, 0};
    ok = again >= 0 && read(again, buf, 2) == 2 && buf[0] == 'o' && buf[1] == 'k';
    if (!ok)
        trace("zip mount: reopening a memfd through %s failed (%s)", path, strerror(errno));
    if (again >= 0)
        close(again);
    close(fd);
    return ok;
}

void evict_until(long long need)
{
    while (!g_cache.empty() && g_cache_bytes + need > g_cache_budget) {
        auto victim = g_cache.begin();
        for (auto it = g_cache.begin(); it != g_cache.end(); ++it)
            if (it->second.last_use < victim->second.last_use)
                victim = it;
        close(victim->second.fd);
        g_cache_bytes -= victim->second.bytes;
        g_cache.erase(victim);
    }
}

/* Inflate entry `index` into a fresh descriptor. -1 on failure. */
int inflate_entry(zip_int64_t index, const std::string &rel, long long size)
{
    int fd;
    std::string spill;
    if (g_use_memfd) {
        fd = (int)syscall(SYS_memfd_create, rel.c_str(), MFD_CLOEXEC);
    } else {
        spill = g_spill_dir + "/" + rel;
        size_t slash = spill.rfind('/');
        mkdirs(spill.substr(0, slash));
        fd = open((spill + ".part").c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }
    if (fd < 0) {
        trace("zip mount: no descriptor for %s: %s", rel.c_str(), strerror(errno));
        return -1;
    }

    zip_file_t *zf = zip_fopen_index(g_zip, index, 0);
    if (!zf) {
        close(fd);
        return -1;
    }

    static char buf[256 * 1024];
    long long total = 0;
    zip_int64_t n;
    bool ok = true;
    while ((n = zip_fread(zf, buf, sizeof(buf))) > 0) {
        const char *p = buf;
        while (n > 0) {
            ssize_t w = write(fd, p, (size_t)n);
            if (w <= 0) { ok = false; break; }
            p += w; n -= w; total += w;
        }
        if (!ok)
            break;
    }
    if (n < 0)
        ok = false;
    zip_fclose(zf);

    if (!ok || total != size) {
        trace("zip mount: inflating %s failed (%lld of %lld bytes)", rel.c_str(), total, size);
        close(fd);
        if (!spill.empty())
            unlink((spill + ".part").c_str());
        return -1;
    }
    if (!spill.empty())
        rename((spill + ".part").c_str(), spill.c_str());
    g_inflated++;
    return fd;
}

} /* namespace */

bool sg_zip_mount_open(const char *zip_path)
{
    g_root = sg_data_dir();

    std::string path = zip_path && *zip_path ? zip_path : "";
    if (path.empty()) {
        /* First *.zip in the game directory. */
        DIR *d = opendir(io_game_dir());
        if (d) {
            struct dirent64 *e;
            while ((e = readdir64(d)) != NULL) {
                std::string name(e->d_name);
                if (ends_with(name, ".zip") || ends_with(name, ".ZIP")) {
                    path = std::string(io_game_dir()) + "/" + name;
                    break;
                }
            }
            closedir(d);
        }
    }
    if (path.empty()) {
        trace("zip mount: no cache archive - the data must be unpacked in %s", g_root.c_str());
        return false;
    }

    int err = 0;
    g_zip = zip_open(path.c_str(), ZIP_RDONLY, &err);
    if (!g_zip) {
        trace("zip mount: cannot open %s (libzip error %d)", path.c_str(), err);
        return false;
    }
    if (!find_prefix()) {
        trace("zip mount: %s has no 'states' file - not the Shadow Guardian cache", path.c_str());
        zip_close(g_zip);
        g_zip = NULL;
        return false;
    }

    g_cache_budget = (long long)port_getenv_long("ZIP_CACHE_MB", 256) << 20;
    g_use_memfd = !port_getenv("ZIP_SPILL") && probe_memfd();
    if (!g_use_memfd) {
        g_spill_dir = std::string(io_writable_dir()) + "/zipcache";
        mkdirs(g_spill_dir);
    }

    mkdirs(g_root);
    create_directories();
    g_shadow_root = std::string(io_writable_dir()) + "/.zipdirs";

    trace("zip mount: %s (%lld entries, prefix '%s') over %s, %s, cache %lld MB",
          path.c_str(), (long long)zip_get_num_entries(g_zip, 0), g_prefix.c_str(),
          g_root.c_str(), g_use_memfd ? "in memory" : ("spilled to " + g_spill_dir).c_str(),
          g_cache_budget >> 20);
    return true;
}

const char *sg_zip_mount_resolve(const char *path, char *buf, size_t bufsz)
{
    if (!g_zip || !path)
        return NULL;

    size_t rn = g_root.size();
    if (strncmp(path, g_root.c_str(), rn) != 0 || path[rn] != '/')
        return NULL;

    struct stat st;
    if (stat(path, &st) == 0)
        return NULL; /* on disk: the disk answers */

    /* Collapse the doubled slashes the engine produces by gluing "%s/" onto
     * names that already start with one. */
    std::string rel;
    for (const char *p = path + rn + 1; *p; p++)
        if (!(*p == '/' && (rel.empty() || rel.back() == '/')))
            rel += *p;
    if (rel.empty())
        return NULL;

    std::string entry = g_prefix + rel;

    pthread_mutex_lock(&g_lock);

    zip_int64_t index = zip_name_locate(g_zip, entry.c_str(), 0);
    if (index < 0)
        index = zip_name_locate(g_zip, entry.c_str(), ZIP_FL_NOCASE);
    if (index < 0) {
        pthread_mutex_unlock(&g_lock);
        if (port_getenv("ZIP_TRACE"))
            trace("zip mount: '%s' is neither on disk nor in the archive", rel.c_str());
        return NULL;
    }

    auto it = g_cache.find(index);
    int fd;
    if (it != g_cache.end()) {
        fd = it->second.fd;
        it->second.last_use = ++g_clock;
    } else {
        zip_stat_t zs;
        zip_stat_init(&zs);
        if (zip_stat_index(g_zip, index, 0, &zs) != 0 || !(zs.valid & ZIP_STAT_SIZE)) {
            pthread_mutex_unlock(&g_lock);
            return NULL;
        }
        long long size = (long long)zs.size;
        evict_until(size);
        fd = inflate_entry(index, rel, size);
        if (fd < 0) {
            pthread_mutex_unlock(&g_lock);
            return NULL;
        }
        g_cache[index] = Cached{fd, size, ++g_clock};
        g_cache_bytes += size;
        if (g_inflated <= 8)
            trace("zip mount: inflated %s (%lld bytes)", rel.c_str(), size);
        else if (g_inflated == 9)
            trace("zip mount: further inflations counted, not listed");
    }
    g_served++;
    if (port_getenv("ZIP_TRACE"))
        trace("zip mount: served %s", rel.c_str());

    if (g_use_memfd)
        snprintf(buf, bufsz, "/proc/self/fd/%d", fd);
    else
        snprintf(buf, bufsz, "%s/%s", g_spill_dir.c_str(), rel.c_str());

    pthread_mutex_unlock(&g_lock);
    return buf;
}

/*
 * opendir() on a directory the archive has entries in: answer with a shadow
 * directory holding an empty file (or directory) per name - the archive's
 * names plus whatever is on disk - so the engine's own listing sees the
 * archive. It enumerates shaders_galaxy_s/ and the like and then opens each
 * name under the data directory, which sg_zip_mount_resolve() serves.
 * Without this every material came out with no render pass.
 */
extern "C" const char *port_fix_dir_path(const char *fixed, char *buf, size_t bufsz)
{
    if (!g_zip || !fixed)
        return NULL;
    size_t rn = g_root.size();
    if (strncmp(fixed, g_root.c_str(), rn) != 0 || (fixed[rn] != '/' && fixed[rn] != '\0'))
        return NULL;

    std::string rel;
    for (const char *p = fixed + rn; *p; p++)
        if (!(*p == '/' && (rel.empty() || rel.back() == '/')))
            rel += *p;
    while (!rel.empty() && rel.back() == '/')
        rel.pop_back();

    pthread_mutex_lock(&g_lock);
    auto it = g_dirs.find(rel);
    if (it == g_dirs.end()) {
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    std::string shadow = g_shadow_root + (rel.empty() ? "" : "/" + rel);
    mkdirs(shadow);
    auto touch = [&](const std::string &name, bool dir) {
        std::string p = shadow + "/" + name;
        if (dir) {
            mkdir(p.c_str(), 0777);
        } else {
            int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
            if (fd >= 0)
                close(fd);
        }
    };
    for (const auto &kid : it->second)
        touch(kid.first, kid.second);

    /* What is on disk lists too (the engine's own files, a partial unpack). */
    DIR *d = opendir(fixed);
    if (d) {
        struct dirent64 *e;
        while ((e = readdir64(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            touch(e->d_name, e->d_type == DT_DIR);
        }
        closedir(d);
    }
    pthread_mutex_unlock(&g_lock);

    static int shown;
    if (shown++ < 8)
        trace("zip mount: listing %s from the archive (%zu names)", rel.empty() ? "/" : rel.c_str(),
              it->second.size());
    snprintf(buf, bufsz, "%s", shadow.c_str());
    return buf;
}

/*
 * Copy-up: before a file of the archive is opened for writing with its
 * contents kept (r+, a), put those contents on disk at `path`. Disk wins over
 * the archive from then on. True when the file is on disk afterwards or the
 * archive has no such entry (a new file).
 */
bool sg_zip_mount_copy_up(const char *path)
{
    struct stat st;
    if (!g_zip || !path || stat(path, &st) == 0)
        return true;
    size_t rn = g_root.size();
    if (strncmp(path, g_root.c_str(), rn) != 0 || path[rn] != '/')
        return true;
    std::string rel;
    for (const char *p = path + rn + 1; *p; p++)
        if (!(*p == '/' && (rel.empty() || rel.back() == '/')))
            rel += *p;

    pthread_mutex_lock(&g_lock);
    zip_int64_t index = zip_name_locate(g_zip, (g_prefix + rel).c_str(), 0);
    bool ok = true;
    if (index >= 0) {
        std::string tmp = std::string(path) + ".part";
        int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        zip_file_t *zf = fd >= 0 ? zip_fopen_index(g_zip, index, 0) : NULL;
        ok = zf != NULL;
        static char buf[64 * 1024];
        zip_int64_t n = 0;
        while (ok && (n = zip_fread(zf, buf, sizeof(buf))) > 0)
            ok = write(fd, buf, (size_t)n) == n;
        if (n < 0)
            ok = false;
        if (zf)
            zip_fclose(zf);
        if (fd >= 0)
            close(fd);
        if (ok)
            ok = rename(tmp.c_str(), path) == 0;
        else
            unlink(tmp.c_str());
        trace("zip mount: copied %s out of the archive for writing (%s)", rel.c_str(), ok ? "ok" : "failed");
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

long sg_zip_mount_served(void)   { return g_served; }
long sg_zip_mount_inflated(void) { return g_inflated; }
