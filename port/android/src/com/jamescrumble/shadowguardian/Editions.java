package com.jamescrumble.shadowguardian;

import java.io.File;
import java.io.IOException;
import java.util.Arrays;
import java.util.Enumeration;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * The two builds of the game the bridge runs, told apart by what the player put in the folder:
 *
 *  - original: Gameloft's 1.0.1 APK and GloftSGHP.zip (the cache has shaders_galaxy_s/);
 *  - community: the UK+RU build (Russian and Ukrainian texts in the French and Italian slots, a patched
 *    library with their fonts); its cache zip holds the tree under gameloft/games/GloftSGHP/ and has no
 *    shaders_galaxy_s/.
 *
 * Files are recognised by content, not by name: the library's CRC in the APK, the shaders folder in the
 * cache. An APK with a CRC we do not know counts as original.
 */
final class Editions {
    static final String ORIGINAL = "original", COMMUNITY = "community";
    static final String[] ALL = { ORIGINAL, COMMUNITY };

    private static final String NATIVE_LIB = "lib/armeabi-v7a/libshadowguardian.so";
    private static final long CRC_COMMUNITY_LIB = 0x9cc2184aL;

    /** What was found for one edition. */
    static final class Files {
        File apk;
        File zip;
        File unpacked;

        boolean complete() { return apk != null && (zip != null || unpacked != null); }

        String missing() {
            StringBuilder sb = new StringBuilder();
            if (apk == null) sb.append("APK игры");
            if (zip == null && unpacked == null) sb.append(sb.length() > 0 ? " и " : "").append("zip-архив с кэшем");
            return sb.toString();
        }
    }

    private Editions() { }

    /** Scans the app's folder; the result has an entry for every edition (possibly empty). */
    static Map<String, Files> scan(File dir) {
        Map<String, Files> m = new LinkedHashMap<>();
        for (String e : ALL) m.put(e, new Files());
        File[] list = dir == null ? null : dir.listFiles();
        if (list == null) return m;
        Arrays.sort(list);
        for (File f : list) {
            String n = f.getName().toLowerCase();
            if (f.isFile() && n.endsWith(".apk")) {
                String e = editionOfApk(f);
                if (e != null && m.get(e).apk == null) m.get(e).apk = f;
            } else if (f.isFile() && n.endsWith(".zip")) {
                String e = editionOfCache(f);
                if (e != null && m.get(e).zip == null) m.get(e).zip = f;
            }
        }
        // A cache unpacked by hand: GloftSGHP/ (the original's layout) or gameloft/games/GloftSGHP/ (the community's).
        for (File d : new File[] { new File(dir, "GloftSGHP"), new File(dir, "gameloft/games/GloftSGHP") }) {
            if (!new File(d, "states").exists()) continue;
            Files fs = m.get(new File(d, "shaders_galaxy_s").isDirectory() ? ORIGINAL : COMMUNITY);
            if (fs.unpacked == null) fs.unpacked = d;
        }
        return m;
    }

    /** Scans every folder of the app (main first, then the other one); the first file found for a slot wins. */
    static Map<String, Files> scanAll(android.content.Context c) {
        Map<String, Files> all = scan(null);
        for (File dir : Storage.sources(c)) {
            Map<String, Files> m = scan(dir);
            for (String e : ALL) {
                Files a = all.get(e), f = m.get(e);
                if (a.apk == null) a.apk = f.apk;
                if (a.zip == null) a.zip = f.zip;
                if (a.unpacked == null) a.unpacked = f.unpacked;
            }
        }
        return all;
    }

    private static String editionOfApk(File apk) {
        try (ZipFile z = new ZipFile(apk)) {
            ZipEntry lib = z.getEntry(NATIVE_LIB);
            if (lib == null) return null;
            return lib.getCrc() == CRC_COMMUNITY_LIB ? COMMUNITY : ORIGINAL;
        } catch (IOException e) {
            return null;
        }
    }

    private static String editionOfCache(File zip) {
        boolean states = false, galaxy = false;
        try (ZipFile z = new ZipFile(zip)) {
            Enumeration<? extends ZipEntry> en = z.entries();
            while (en.hasMoreElements()) {
                String n = en.nextElement().getName();
                if (n.equals("states") || n.endsWith("/states")) states = true;
                else if (n.startsWith("shaders_galaxy_s/") || n.contains("/shaders_galaxy_s/")) galaxy = true;
            }
        } catch (IOException e) {
            return null;
        }
        return !states ? null : galaxy ? ORIGINAL : COMMUNITY;
    }

    /** The edition to run: the setting when it names one, else the first with everything in place. */
    static String choose(String setting, Map<String, Files> found) {
        if (ORIGINAL.equals(setting) || COMMUNITY.equals(setting)) return setting;
        for (String e : ALL) if (found.get(e).complete()) return e;
        return ORIGINAL;
    }

    static String title(String edition) {
        return COMMUNITY.equals(edition) ? "сообщество (RU+UK)" : "оригинал";
    }

    /**
     * The engine's language index for the phone's language, as the game's own ShadowGuardian.GetPhoneLanguage
     * maps it (0 English, 1 French, 2 Spanish, 3 Italian, 4 German, 5 Portuguese, 6 Japanese, 7 Korean);
     * in the community build slots 1 and 3 hold Russian and Ukrainian instead, so French and Italian phones
     * get English there.
     */
    static int phoneLanguage(String edition) {
        String l;
        try { l = java.util.Locale.getDefault().getISO3Language(); } catch (java.util.MissingResourceException e) { return 0; }
        if (COMMUNITY.equals(edition)) {
            if (l.equals("rus")) return 1;
            if (l.equals("ukr")) return 3;
            if (l.equals("fra") || l.equals("ita")) return 0;
        }
        switch (l) {
            case "fra": return 1;
            case "spa": return 2;
            case "ita": return 3;
            case "deu": return 4;
            case "por": case "bra": return 5;
            case "jpn": return 6;
            case "kor": return 7;
            default: return 0;
        }
    }
}
