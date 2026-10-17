package com.jamescrumble.shadowguardian;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.os.SystemClock;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructPollfd;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedList;
import java.util.List;
import java.util.ListIterator;
import java.util.Map;
import java.util.concurrent.TimeUnit;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipInputStream;

/**
 * Owns everything outside the UI: unpacking the sysroot, linking the player's
 * game files, running qemu-arm and talking to the port through the control
 * directory (commands in, frame.bin and status.json out, audio over a FIFO).
 */
final class GameProcess {
    interface Listener {
        void onStatus(String text);
        void onExit(int code);
    }

    private static final String NATIVE_LIB = "lib/armeabi-v7a/libshadowguardian.so";
    private static final int SAMPLE_RATE = 44100;

    private static final class Cmd {
        String line;
        final String key;
        Cmd(String line, String key) { this.line = line; this.key = key; }
    }

    private final Context ctx;
    private volatile Listener listener;
    private final boolean touchMode;
    private final File root;
    private final File control;
    private final File commandsFile;
    private final File statusFile;
    private final File fifo;
    private final File logFile;
    private final LinkedList<Cmd> queue = new LinkedList<>();
    private volatile boolean running;
    private volatile boolean muted = true;
    private Process process;
    private final GlBridge gl;
    private final PerfHint perf;
    private int renderW = 1280, renderH = 590;

    GameProcess(Context ctx, Listener listener) {
        this.ctx = ctx;
        this.listener = listener;
        root = ctx.getFilesDir();
        control = new File(root, "ctl");
        commandsFile = new File(control, "commands");
        statusFile = new File(control, "status.json");
        fifo = new File(root, "audio.pcm");
        File external = ctx.getExternalFilesDir(null);
        logFile = new File(external != null ? external : root, "sg.log");
        // pad_overlay.flag next to the log brings back the wrapper's own gamepad-style overlay.
        touchMode = !new File(logFile.getParentFile(), "pad_overlay.flag").exists();
        perf = new PerfHint(ctx, msg -> appendLog("[perf] " + msg));
        gl = new GlBridge(new File(root, "gl_cmd.pipe").getPath(), new File(root, "gl_reply.pipe").getPath(),
                msg -> appendLog("[gl] " + msg), perf::frame, perf::cpuLine);
        chooseRenderSize();
    }

    /**
     * Screen modes (SCREEN_MODE in the settings; stretch.flag next to the log still means "stretch"):
     *  - adapt (default): the engine keeps its own 5:3 layout, the picture fills the phone's whole screen, and the
     *    GL bridge undoes the stretching per draw - interface elements keep their proportions and sit at the
     *    left, centre or right edge, the 3D scene gets the wider field of view (portbase gl_bridge.cpp, ui_adapt);
     *  - fit: 5:3 with black bars at the sides;
     *  - stretch: 5:3 stretched over the whole screen, as the engine would do it.
     * The engine never sees the real aspect: Shadow Guardian picks its sprite set and lays out its interface by it,
     * and wider than 16:9 it breaks (buttons off screen, sprites_1_7 that a community cache does not have).
     * renderW x renderH is the panel (the surface and what the touch overlay maps against); logicalW x logicalH is
     * what the engine thinks the screen is. resolution.txt ("1280x590") next to the log overrides the logical size.
     */
    private void chooseRenderSize() {
        android.util.DisplayMetrics m = ctx.getResources().getDisplayMetrics();
        int longSide = Math.max(m.widthPixels, m.heightPixels), shortSide = Math.min(m.widthPixels, m.heightPixels);
        java.util.Map<String, String> st = SettingsActivity.load(ctx);
        int h = 720;
        try { h = Integer.parseInt(st.getOrDefault("RENDER_HEIGHT", "720")); } catch (NumberFormatException ignored) { }
        h = Math.max(240, Math.min(h, 2160)) & ~1;
        screenMode = st.getOrDefault("SCREEN_MODE", "adapt");
        if (new File(logFile.getParentFile(), "stretch.flag").exists()) screenMode = "stretch";
        if (!screenMode.equals("fit") && !screenMode.equals("stretch")) screenMode = "adapt";
        int w = Math.round(h * 5f / 3f) & ~1;
        String over = readText(new File(logFile.getParentFile(), "resolution.txt")).trim();
        java.util.regex.Matcher mt = java.util.regex.Pattern.compile("(\\d+)x(\\d+)").matcher(over);
        if (mt.matches()) {
            w = Integer.parseInt(mt.group(1));
            h = Integer.parseInt(mt.group(2));
        }
        logicalW = Math.max(320, Math.min(w, 4096));
        logicalH = Math.max(240, Math.min(h, 4096));
        if (screenMode.equals("fit")) {
            renderW = logicalW;
            renderH = logicalH;
        } else {
            renderW = Math.max(logicalW, Math.round(logicalH * (float) longSide / shortSide) & ~1);
            renderH = logicalH;
        }
    }

    private String screenMode = "adapt";
    private int logicalW = 1200, logicalH = 720;

    /** How much the engine's picture is stretched horizontally on the panel, inverted: 1 when it is not corrected. */
    private float adaptFactor() {
        if (!screenMode.equals("adapt") || renderW <= logicalW) return 1f;
        return ((float) logicalW / logicalH) / ((float) renderW / renderH);
    }

    /**
     * A touch at (xf, yf), fractions of the view, in what the port expects. When the picture is adapted the x goes in
     * panel pixels and the port maps it back through the interface layout it drew (input_bridge.cpp); otherwise it is
     * the engine's own logical pixel.
     */
    int[] mapTouch(float xf, float yf, int id, int action) {
        float w = adaptFactor() < 0.999f ? renderW : logicalW;
        return new int[] { Math.round(Math.max(0f, Math.min(1f, xf)) * w), Math.round(Math.max(0f, Math.min(1f, yf)) * logicalH) };
    }

    int renderWidth() { return renderW; }
    int renderHeight() { return renderH; }
    GlBridge gl() { return gl; }
    File logFile() { return logFile; }
    boolean isRunning() { return running; }
    boolean touchMode() { return touchMode; }
    void setListener(Listener l) { listener = l; }
    void setMuted(boolean m) { muted = m; }

    void start() throws Exception {
        stopStale();
        String migrated = SaveMigration.importIfPresent(ctx);
        if (migrated != null) appendLog("[wrapper] " + migrated);
        prepare();
        launch();
    }

    /** Returns {frame, menu}, or null before the port has written a status. */
    int[] readStatus() {
        String s = readText(statusFile);
        if (s.isEmpty()) return null;
        return new int[] { intField(s, "\"frame\":"), intField(s, "\"menu\":") };
    }

    /**
     * Queues one command line. Commands with a key (cursor, stick) replace an
     * older queued command of the same key, because the port consumes only one
     * line per frame and stale positions would only add lag.
     */
    void command(String line, String key) {
        synchronized (queue) {
            if (!running) return;
            if (key != null) {
                ListIterator<Cmd> it = queue.listIterator(queue.size());
                while (it.hasPrevious()) {
                    Cmd c = it.previous();
                    if (c.key == null) break;
                    if (c.key.equals(key)) { c.line = line; return; }
                }
            }
            queue.add(new Cmd(line, key));
            queue.notifyAll();
        }
    }

    void stop() {
        boolean wasRunning = running;
        running = false;
        gl.stop();
        synchronized (queue) { queue.notifyAll(); }
        if (!wasRunning) return;
        appendLine("quit");
        final Process p = process;
        if (p != null) {
            new Thread(() -> {
                try {
                    if (!p.waitFor(2, TimeUnit.SECONDS)) p.destroy();
                } catch (InterruptedException ignored) {
                    p.destroy();
                }
            }, "sg-stop").start();
        }
    }

    // A qemu left behind by a killed app would keep running on its own;
    // ask it to quit before the control files are recreated.
    private void stopStale() throws InterruptedException {
        if (!statusFile.exists() || System.currentTimeMillis() - statusFile.lastModified() > 5000) return;
        appendLine("quit");
        long end = System.currentTimeMillis() + 5000;
        while (System.currentTimeMillis() < end && !readText(statusFile).contains("\"stopped\"")) {
            Thread.sleep(100);
        }
    }

    private void prepare() throws Exception {
        listener.onStatus("Подготовка файлов…");
        File bin = new File(root, "bin");
        File sysroot = new File(root, "sysroot");

        String stamp = String.valueOf(ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0).lastUpdateTime);
        File stampFile = new File(root, "install.stamp");
        if (!stamp.equals(readText(stampFile).trim())) {
            deleteTree(bin);
            deleteTree(sysroot);
            stampFile.delete();
        }

        File binary = new File(bin, "shadowguardian");
        if (!binary.exists()) {
            bin.mkdirs();
            try (InputStream in = new BufferedInputStream(ctx.getAssets().open("shadowguardian"))) {
                copy(in, new FileOutputStream(binary));
            }
        }

        File done = new File(sysroot, ".done");
        if (!done.exists()) {
            listener.onStatus("Распаковка системных библиотек…\nЭто делается один раз.");
            unzipAsset("sysroot.zip");
            done.createNewFile();
        }
        writeText(stampFile, stamp);
        // qemu-user refuses to load a guest program whose file lacks the execute bit.
        binary.setExecutable(true, true);
        new File(sysroot, "lib/ld-linux-armhf.so.3").setExecutable(true, true);

        // Which build of the game to run: GAME_EDITION in the settings ("auto" takes the first one found complete).
        // They share one game folder (progress carries over); the APK's files are re-extracted on a switch.
        File external = ctx.getExternalFilesDir(null);
        java.util.Map<String, Editions.Files> found = Editions.scan(external);
        edition = Editions.choose(SettingsActivity.load(ctx).get("GAME_EDITION"), found);
        Editions.Files files = found.get(edition);
        if (!files.complete()) {
            StringBuilder have = new StringBuilder();
            for (String e : Editions.ALL) {
                Editions.Files f = found.get(e);
                if (f.apk != null || f.zip != null || f.unpacked != null)
                    have.append("\n  ").append(Editions.title(e)).append(": ")
                            .append(f.complete() ? "всё на месте" : "нет " + f.missing());
            }
            throw new IOException("Для версии «" + Editions.title(edition) + "» не хватает: " + files.missing() + ".\n"
                    + "Положите файлы в папку:\n" + (external == null ? "Android/data/" + ctx.getPackageName() + "/files" : external.getPath())
                    + (have.length() > 0 ? "\n\nНайдено:" + have : "") + "\n\nВерсию можно сменить в настройках.");
        }
        editionNote = "edition " + edition + ": apk " + files.apk.getName()
                + ", cache " + (files.zip != null ? files.zip.getName() : files.unpacked.getPath());

        File game = gameDir();
        File libDir = new File(game, "lib/armeabi-v7a");
        libDir.mkdirs();

        // From the game's APK only the library, assets/ and the raw resources GLResLoader reads; once per APK.
        File apk = files.apk;
        String apkStamp = apk.length() + ":" + apk.lastModified();
        File apkStampFile = new File(game, "apk.stamp");
        if (!apkStamp.equals(readText(apkStampFile).trim()) || !new File(libDir, "libshadowguardian.so").exists()) {
            listener.onStatus("Извлечение файлов из APK игры…");
            deleteTree(new File(game, "assets"));
            deleteTree(new File(game, "res"));
            extractFromApk(apk, game);
            writeText(apkStampFile, apkStamp);
        }

        // The cache. The zip lies next to the APK as it is: the port mounts it over the data directory and reads
        // entries out of it on demand (game/zip_mount.cpp), nothing is unpacked. A folder unpacked by hand next to
        // the APK works as well.
        cacheZip = files.zip;
        File dataDir = new File(game, "GloftSGHP");
        if (cacheZip == null) {
            if (!isSymlink(dataDir)) deleteTree(dataDir);
            linkOrCopy(files.unpacked, dataDir);
        } else if (isSymlink(dataDir)) {
            dataDir.delete();
        }
    }

    private String edition = Editions.ORIGINAL, editionNote = "";

    /**
     * One game folder for both editions, so progress is shared: the engine writes settings, a and rls into
     * GloftSGHP/ and its saves/ here, in the same format in both builds. Only lib/, assets/ and res/ belong to
     * the edition's APK, and they are extracted again when the APK changes (apk.stamp).
     */
    private File gameDir() {
        return new File(root, "game");
    }

    private File cacheZip;

    private static boolean isSymlink(File f) {
        try {
            return android.system.OsConstants.S_ISLNK(Os.lstat(f.getPath()).st_mode);
        } catch (Exception e) {
            return false;
        }
    }

    /** lib/armeabi-v7a/libshadowguardian.so, assets/ and res/{drawable,raw}/ of the game's APK into dir. */
    private void extractFromApk(File apk, File dir) throws IOException {
        String rootPath = dir.getCanonicalPath() + File.separator;
        boolean lib = false;
        try (ZipFile z = new ZipFile(apk)) {
            java.util.Enumeration<? extends ZipEntry> en = z.entries();
            while (en.hasMoreElements()) {
                ZipEntry e = en.nextElement();
                String n = e.getName();
                if (e.isDirectory()) continue;
                boolean want = n.equals(NATIVE_LIB) || n.startsWith("assets/")
                        || n.startsWith("res/drawable/") || n.startsWith("res/raw/");
                if (!want) continue;
                File out = new File(dir, n);
                if (!out.getCanonicalPath().startsWith(rootPath)) throw new IOException("bad zip entry " + n);
                out.getParentFile().mkdirs();
                try (InputStream in = new BufferedInputStream(z.getInputStream(e))) {
                    copy(in, new FileOutputStream(out));
                }
                if (n.equals(NATIVE_LIB)) lib = true;
            }
        }
        if (!lib) throw new IOException("В APK нет " + NATIVE_LIB + "\nНужен APK Shadow Guardian HD 1.0.1: " + apk.getName());
    }

    private void launch() throws Exception {
        listener.onStatus("Запуск игры…");
        File qemu = new File(ctx.getApplicationInfo().nativeLibraryDir, "libqemu.so");
        if (!qemu.exists()) throw new IOException("Нет libqemu.so в " + qemu.getParent());

        control.mkdirs();
        statusFile.delete();
        commandsFile.delete();
        commandsFile.createNewFile();
        fifo.delete();
        Os.mkfifo(fifo.getPath(), 0600);
        File glCmd = new File(root, "gl_cmd.pipe"), glReply = new File(root, "gl_reply.pipe");
        glCmd.delete();
        glReply.delete();
        Os.mkfifo(glCmd.getPath(), 0600);
        Os.mkfifo(glReply.getPath(), 0600);
        logFile.delete();
        appendLog("[wrapper] " + editionNote);
        gl.start();

        ProcessBuilder b = new ProcessBuilder(qemu.getPath());
        // Diagnostics: extra qemu arguments, one per line, from qemu_args.txt next to the log.
        for (String arg : readText(new File(logFile.getParentFile(), "qemu_args.txt")).split("\n")) {
            if (!arg.trim().isEmpty()) b.command().add(arg.trim());
        }
        // Android hides /proc/sys/vm/mmap_min_addr, so qemu would try to reserve guest memory at
        // 0x1000 and be refused; a non-zero guest base sidesteps that.
        // The default "any" CPU makes qemu emulate NEON/VFP through slow helpers; a real Cortex-A15
        // profile is several times faster on the game's own code.
        b.command().add("-cpu");
        b.command().add("cortex-a15");
        b.command().add("-B");
        b.command().add("0x100000000");
        b.command().add("-L");
        b.command().add(new File(root, "sysroot").getPath());
        b.command().add(new File(root, "bin/shadowguardian").getPath());
        b.command().add(gameDir().getPath());
        Map<String, String> env = b.environment();
        env.put("HOME", new File(root, "home").getPath());
        env.put("SDL_VIDEODRIVER", "offscreen");
        env.put("SDL_AUDIODRIVER", "disk");
        env.put("SDL_DISKAUDIOFILE", fifo.getPath());
        // The port's OpenSL ES writes the game's mixer blocks straight into the FIFO (see audioLoop());
        // SDL's disk device above is only its fallback.
        env.put("SG_AUDIO_FIFO", fifo.getPath());
        env.put("SG_CONTROL_DIR", control.getPath());
        if (cacheZip != null) env.put("SG_CACHE_ZIP", cacheZip.getPath());
        // OpenGL ES calls are executed by the app on the GPU (GlBridge); nothing renders inside qemu.
        env.put("SG_GL_BRIDGE", "1");
        env.put("SG_GL_CMD", glCmd.getPath());
        env.put("SG_GL_REPLY", glReply.getPath());
        // The engine's screen is the logical one; a wider panel is mapped onto by the port (SCALE=stretch) and corrected
        // by the GL bridge in adapt mode.
        env.put("SG_WIDTH", String.valueOf(logicalW));
        env.put("SG_HEIGHT", String.valueOf(logicalH));
        if (renderW != logicalW || renderH != logicalH) {
            env.put("SG_PANEL_W", String.valueOf(renderW));
            env.put("SG_PANEL_H", String.valueOf(renderH));
            env.put("SG_SCALE", "stretch");
            if (screenMode.equals("adapt")) env.put("SG_UI_ADAPT", "1");
        }
        env.put("SG_FB_PROBE_INTERVAL", "0");
        env.put("SG_CURSOR", "0");
        // An ordinary touch phone to the engine: its own on-screen controls, no PowerA pad until one is used.
        if (touchMode) env.put("SG_TOUCH", "1");
        // The engine sizes its quality profile by GPU name; gpu_name.txt next to the log overrides the real one.
        String gpu = readText(new File(logFile.getParentFile(), "gpu_name.txt")).trim();
        if (!gpu.isEmpty()) env.put("SG_GL_RENDERER", gpu);
        // xml_override.txt ("old=>new" per line) edits the engine's XML, quality profiles included, before parsing;
        // xmldump.flag writes every parsed document to xmldump/.
        File dir = logFile.getParentFile();
        File overrides = new File(dir, "xml_override.txt");
        if (overrides.exists()) env.put("SG_XML_OVERRIDES", overrides.getPath());
        if (new File(dir, "xmldump.flag").exists()) env.put("SG_XML_DUMP", new File(dir, "xmldump").getPath());
        // engine_env.txt: KEY=VALUE per line, added to the port's environment (SG_PHYSICS_THREAD=1, ...).
        for (String line : readText(new File(dir, "engine_env.txt")).split("\n")) {
            int eq = line.indexOf('=');
            if (eq > 0 && !line.trim().startsWith("#")) env.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
        }
        // settings.txt is written by SettingsActivity and wins over engine_env.txt (RENDER_HEIGHT is ours, not the engine's).
        for (java.util.Map.Entry<String, String> e : SettingsActivity.load(ctx).entrySet())
            if (!e.getKey().equals("RENDER_HEIGHT") && !e.getKey().startsWith("Q_") && !e.getKey().equals("CPU_PRIME")
                    && !e.getKey().equals("AUDIO_LATENCY_MS") && !e.getKey().equals("PERF_HINT")
                    && !e.getKey().equals("GAME_EDITION") && !e.getKey().equals("GAME_LANG") && !e.getKey().equals("SCREEN_MODE")) env.put(e.getKey(), e.getValue());
        // Language: GAME_LANG is an engine index, or "auto" for the phone's language.
        int lang = Editions.phoneLanguage(edition);
        try { lang = Integer.parseInt(SettingsActivity.load(ctx).getOrDefault("GAME_LANG", "auto")); } catch (NumberFormatException ignored) { }
        env.put("SG_LANG_INDEX", String.valueOf(lang));
        appendLog("[wrapper] language index " + lang);
        applyQualityOverrides(dir, overrides, env);
        // Our qemu: guest syscall counts every 10 s in the log (which ones leave translated code most often).
        env.put("QEMU_SYSCALL_STATS", "1");
        // Guest-function profile: qemu samples the game's PC (tools/guest_prof.py maps it to functions).
        // engine_env.txt with QEMU_GUEST_PROF=0 turns it off.
        env.put("QEMU_GUEST_PROF", "1");
        // glibc 2.35+ registers rseq at startup; Android's seccomp filter may kill the process for it.
        env.put("GLIBC_TUNABLES", "glibc.pthread.rseq=0");
        File home = new File(root, "home");
        home.mkdirs();
        // Loader trace stays on while the Android build is being brought up; add notrace.flag next to the log to mute it.
        if (!new File(logFile.getParentFile(), "notrace.flag").exists()) env.put("LOADER_TRACE", "1");
        // Diagnostics: run diag_cmd.txt with sh instead of the game, same uid and environment.
        File diag = new File(logFile.getParentFile(), "diag_cmd.txt");
        if (diag.exists()) {
            env.put("QEMU", qemu.getPath());
            env.put("STRACE", new File(ctx.getApplicationInfo().nativeLibraryDir, "libmstrace.so").getPath());
            env.put("ROOT", root.getPath());
            b.command("/system/bin/sh", "-c", readText(diag));
        }
        // cpu_mask.txt next to the log (a hex mask, e.g. "c0" = cores 6-7): qemu and all its threads stay on those
        // cores. The frequency limiter caps clusters differently from moment to moment, and the scheduler keeps
        // moving the game's one busy thread onto whichever is free - often a capped one.
        String mask = readText(new File(logFile.getParentFile(), "cpu_mask.txt")).trim();
        if ("1".equals(SettingsActivity.load(ctx).get("CPU_PRIME"))) mask = "c0";
        if (diag.exists()) mask = "";
        if (mask.matches("[0-9a-fA-F]{1,4}")) {
            b.command().add(0, "/system/bin/taskset");
            b.command().add(1, mask);
            appendLog("[wrapper] qemu pinned to cpu mask 0x" + mask);
        }
        b.redirectErrorStream(true);
        b.redirectOutput(ProcessBuilder.Redirect.appendTo(logFile));

        running = true;
        final long startedAt = SystemClock.elapsedRealtime();
        appendLog("[wrapper] " + b.command());
        process = b.start();
        final Process p = process;
        int fpsLimit = 60;
        try { fpsLimit = Integer.parseInt(SettingsActivity.load(ctx).getOrDefault("FPS_LIMIT", "60")); } catch (NumberFormatException ignored) { }
        gl.setFrameLimit(fpsLimit);
        perf.start(p, "1".equals(SettingsActivity.load(ctx).get("PERF_HINT")), fpsLimit);
        new Thread(this::writerLoop, "sg-commands").start();
        new Thread(this::audioLoop, "sg-audio").start();
        new Thread(() -> {
            int code = -1;
            try { code = p.waitFor(); } catch (InterruptedException ignored) { }
            appendLog("[wrapper] qemu exited with code " + code
                    + (code > 128 ? " (signal " + (code - 128) + ")" : "")
                    + " after " + (SystemClock.elapsedRealtime() - startedAt) + " ms");
            if (running) {
                running = false;
                listener.onExit(code);
            }
        }, "sg-watch").start();
    }

    /**
     * Writes whatever is queued in one go. The port paces the lines itself (touch lines several per
     * frame, everything else one per frame), so the pause here only lets moves coalesce.
     */
    private void writerLoop() {
        StringBuilder batch = new StringBuilder();
        try (FileOutputStream out = new FileOutputStream(commandsFile, true)) {
            while (running) {
                batch.setLength(0);
                synchronized (queue) {
                    while (queue.isEmpty() && running) queue.wait();
                    if (!running) break;
                    for (Cmd c : queue) batch.append(c.line).append('\n');
                    queue.clear();
                }
                out.write(batch.toString().getBytes("US-ASCII"));
                Thread.sleep(4);
            }
        } catch (Exception ignored) {
        }
    }

    /**
     * Plays what the port writes into the FIFO (its OpenSL ES, one mixer block per write).
     *
     * The AudioTrack is the clock. The pipe is shrunk to one page and the track buffer to the menu's
     * AUDIO_LATENCY_MS (the device minimum by default), so the port's writer blocks until the speaker has
     * consumed what came before and the game's mixer is called back at exactly that rate. With a 64 KB pipe
     * and a 64 KB track buffer, and a writer paced by its own sleep, the sound drifted most of a second late.
     */
    private void audioLoop() {
        int min = AudioTrack.getMinBufferSize(SAMPLE_RATE, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        // 0 (the default) is the smallest buffer the device allows.
        int latencyMs = 0;
        try { latencyMs = Integer.parseInt(SettingsActivity.load(ctx).getOrDefault("AUDIO_LATENCY_MS", "0")); } catch (NumberFormatException ignored) { }
        int bufBytes = Math.max(min, (SAMPLE_RATE * 4 * Math.max(0, Math.min(latencyMs, 1000)) / 1000) & ~3);
        AudioTrack track = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_GAME)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setAudioFormat(new AudioFormat.Builder()
                        .setSampleRate(SAMPLE_RATE)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                .setBufferSizeInBytes(bufBytes)
                .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build();
        FileDescriptor fd = null;
        try {
            // O_RDWR keeps the FIFO open without a writer yet and never reports EOF,
            // so the loop can poll with a timeout and notice when the game stops.
            fd = Os.open(fifo.getPath(), OsConstants.O_RDWR | OsConstants.O_NONBLOCK, 0);
            int pipe = -1;
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                try { pipe = Os.fcntlInt(fd, 1031 /* F_SETPIPE_SZ */, 4096); } catch (ErrnoException ignored) { }
            }
            appendLog("[audio] track buffer " + bufBytes + " bytes (" + bufBytes * 1000L / (SAMPLE_RATE * 4) + " ms, min " + min
                    + "), pipe " + pipe + " bytes");
            StructPollfd[] polls = { new StructPollfd() };
            polls[0].fd = fd;
            polls[0].events = (short) OsConstants.POLLIN;
            byte[] buf = new byte[16384];
            boolean playing = false;
            int underruns = 0;
            long nextReport = SystemClock.elapsedRealtime() + 10000;
            while (running) {
                polls[0].revents = 0;
                if (Os.poll(polls, 100) <= 0) continue;
                int n;
                try {
                    n = Os.read(fd, buf, 0, buf.length);
                } catch (ErrnoException e) {
                    if (e.errno == OsConstants.EAGAIN) continue;
                    break;
                } catch (InterruptedIOException e) {
                    break;
                }
                if (n <= 0) continue;
                // While the game is loading (or the app is in the background) its audio is replaced by
                // silence rather than dropped: the track has to stay the game mixer's clock.
                if (muted) java.util.Arrays.fill(buf, 0, n, (byte) 0);
                if (!playing) { track.play(); playing = true; }
                track.write(buf, 0, n);
                long now = SystemClock.elapsedRealtime();
                if (now >= nextReport) {
                    nextReport = now + 10000;
                    int u = track.getUnderrunCount();
                    if (u != underruns) appendLog("[audio] speaker ran dry " + (u - underruns) + " time(s) in 10 s");
                    underruns = u;
                }
            }
        } catch (ErrnoException ignored) {
        } finally {
            if (fd != null) try { Os.close(fd); } catch (ErrnoException ignored) { }
            track.release();
        }
    }

    private void unzipAsset(String name) throws IOException {
        String rootPath = root.getCanonicalPath() + File.separator;
        try (ZipInputStream z = new ZipInputStream(new BufferedInputStream(ctx.getAssets().open(name)))) {
            ZipEntry e;
            while ((e = z.getNextEntry()) != null) {
                File out = new File(root, e.getName());
                if (!out.getCanonicalPath().startsWith(rootPath)) throw new IOException("bad zip entry " + e.getName());
                if (e.isDirectory()) { out.mkdirs(); continue; }
                out.getParentFile().mkdirs();
                copy(z, new FileOutputStream(out), false);
            }
        }
    }

    private void extractNative(File apk, File out) throws IOException {
        try (ZipFile z = new ZipFile(apk)) {
            ZipEntry e = z.getEntry(NATIVE_LIB);
            if (e == null) throw new IOException("В APK нет " + NATIVE_LIB + ".\nНужна версия 1.0.7.");
            if (out.exists() && out.length() == e.getSize()) return;
            copy(z.getInputStream(e), new FileOutputStream(out));
        }
    }

    /** Every *.obb of a cache zip into dir, whatever folder it sits in inside the archive. */
    private void extractObbs(File zip, File dir) throws IOException {
        int count = 0;
        try (ZipFile z = new ZipFile(zip)) {
            java.util.Enumeration<? extends ZipEntry> en = z.entries();
            while (en.hasMoreElements()) {
                ZipEntry e = en.nextElement();
                String n = e.getName();
                if (e.isDirectory() || !n.endsWith(".obb")) continue;
                String name = n.substring(n.lastIndexOf('/') + 1);
                File out = new File(dir, name), tmp = new File(dir, name + ".part");
                long total = e.getSize(), done = 0, shown = -1;
                try (InputStream in = z.getInputStream(e); OutputStream os = new FileOutputStream(tmp)) {
                    byte[] buf = new byte[1 << 20];
                    for (int r; (r = in.read(buf)) > 0; ) {
                        os.write(buf, 0, r);
                        done += r;
                        long pct = total > 0 ? done * 100 / total : 0;
                        if (pct != shown) {
                            shown = pct;
                            listener.onStatus("Распаковка кэша: " + name + " " + pct + "%");
                        }
                    }
                }
                if (!tmp.renameTo(out)) throw new IOException("Не удалось записать " + out.getPath());
                count++;
            }
        }
        if (count == 0) throw new IOException("В zip нет файлов .obb\nНужен кэш игры 1.0.7: " + zip.getName());
    }

    // The OBBs are over 2 GB; a symlink saves copying them into private storage.
    private void linkOrCopy(File src, File dst) throws IOException {
        if (dst.length() == src.length() && dst.length() > 0) return;
        dst.delete();
        try {
            Os.symlink(src.getPath(), dst.getPath());
        } catch (ErrnoException ignored) {
        }
        if (dst.length() != src.length()) {
            dst.delete();
            listener.onStatus("Копирование " + src.getName() + "…");
            copy(new FileInputStream(src), new FileOutputStream(dst));
        }
    }

    void log(String line) { appendLog("[app] " + line); }

    private void appendLog(String line) {
        try (FileOutputStream out = new FileOutputStream(logFile, true)) {
            out.write((line + "\n").getBytes("UTF-8"));
        } catch (IOException ignored) {
        }
    }

    private void appendLine(String line) {
        try (FileOutputStream out = new FileOutputStream(commandsFile, true)) {
            out.write((line + "\n").getBytes("US-ASCII"));
        } catch (IOException ignored) {
        }
    }

    private static int intField(String s, String key) {
        int i = s.indexOf(key);
        if (i < 0) return 0;
        i += key.length();
        int j = i;
        while (j < s.length() && Character.isDigit(s.charAt(j))) j++;
        try { return Integer.parseInt(s.substring(i, j)); } catch (NumberFormatException e) { return 0; }
    }

    /**
     * The menu's quality options are text substitutions on the engine's option XML (the same mechanism as
     * xml_override.txt, whose lines are kept): "old=>new" per line, applied to every document before parsing.
     */
    private void applyQualityOverrides(File dir, File manual, java.util.Map<String, String> env) {
        java.util.Map<String, String> m = SettingsActivity.load(ctx);
        StringBuilder sb = new StringBuilder(readText(manual));
        if (sb.length() > 0 && sb.charAt(sb.length() - 1) != '\n') sb.append('\n');
        int n = 0;
        if ("0".equals(m.get("Q_SHADOWS"))) { sb.append("name=\"Shadows\" value=\"true\"=>name=\"Shadows\" value=\"false\"\n"); n++; }
        if ("0".equals(m.get("Q_POST"))) {
            for (String fx : new String[] { "Refracted Objects", "Global DOF", "Height Fog", "Radial Blur", "Heat Haze", "Hurt Distort", "Hurt Old" }) {
                sb.append("name=\"Post Effects - " + fx + "\" value=\"true\"=>name=\"Post Effects - " + fx + "\" value=\"false\"\n");
            }
            n++;
        }
        String lod = m.get("Q_LOD");
        if (lod != null && !lod.equals("100")) {
            try {
                sb.append("name=\"LOD distance factor\" range=\"[0.01,1.0]\" value=\"1.000000\"=>name=\"LOD distance factor\" range=\"[0.01,1.0]\" value=\""
                        + String.format(java.util.Locale.US, "%.3f", Integer.parseInt(lod) / 100f) + "\"\n");
                n++;
            } catch (NumberFormatException ignored) { }
        }
        String det = m.get("Q_DETAILS");
        if (det != null && !det.equals("0")) {
            sb.append("name=\"Details level\" range=\"[0,2]\" value=\"0\"=>name=\"Details level\" range=\"[0,2]\" value=\"" + det + "\"\n");
            n++;
        }
        if (n == 0) return;
        File out = new File(dir, "xml_override_menu.txt");
        try (java.io.FileOutputStream fo = new java.io.FileOutputStream(out)) {
            fo.write(sb.toString().getBytes("UTF-8"));
            env.put("SG_XML_OVERRIDES", out.getPath());
        } catch (IOException e) {
            appendLog("[app] quality overrides not written: " + e);
        }
    }

    private static String readText(File f) {
        if (!f.isFile()) return "";
        try (FileInputStream in = new FileInputStream(f)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            return out.toString("UTF-8");
        } catch (IOException e) {
            return "";
        }
    }

    private static void writeText(File f, String text) throws IOException {
        try (FileOutputStream out = new FileOutputStream(f)) { out.write(text.getBytes("UTF-8")); }
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        try (InputStream i = in) { copy(i, out, true); }
    }

    private static void copy(InputStream in, OutputStream out, boolean unused) throws IOException {
        byte[] buf = new byte[65536];
        int n;
        try {
            while ((n = in.read(buf)) >= 0) if (n > 0) out.write(buf, 0, n);
        } finally {
            out.close();
        }
    }

    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) for (File c : children) deleteTree(c);
        f.delete();
    }
}
