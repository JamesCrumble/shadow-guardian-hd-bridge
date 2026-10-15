# Shadow Guardian Bridge

**English:** run **Shadow Guardian HD 1.0.1** (Gameloft, 2011, 32-bit ARMv7) on 64-bit-only Android phones that have
no AArch32 support. The game runs under a patched `qemu-user`, its OpenGL ES calls are forwarded to the phone's real
GPU, and the game's cache zip is read in place — mounted as the data folder, never unpacked. Tested on a OnePlus 13s
(Snapdragon 8 Elite). No game files are included: you need your own copy.
Ready-made APK: [Releases](https://github.com/JamesCrumble/shadow-guardian-hd-bridge/releases).
The rest of this page is in Russian.

Запуск 32-битной **Shadow Guardian HD** (Gameloft, v1.0.1, только `armeabi-v7a`) на 64-битных Android-телефонах, в
которых нет поддержки AArch32. Проверено на OnePlus 13s (Snapdragon 8 Elite).

Игра исполняется в собственном пропатченном `qemu-arm` (linux-user), её вызовы OpenGL ES пробрасываются на настоящий
GPU телефона. Кэш игры (`GloftSGHP.zip`) не распаковывается: порт монтирует архив как папку данных.

**Игра не распространяется.** Нужны ваши собственные APK версии 1.0.1 и zip-архив кэша.

## Состояние (0.1)

- Титульный экран, меню, бой на первом уровне; 60 кадров/с.
- Звук, касания (родной сенсорный HUD игры), кнопка «Назад» (пауза в бою), сохранения.
- Картинка в пропорции 5:3, как у оригинала (интерфейс игры свёрстан под 800×480); по бокам поля.
- Не проверено: прохождение дальше первого уровня, долгая игра, сворачивание и возврат. Геймпад не подключён,
  вступительный ролик не проигрывается.

## Установка

1. Установите `ShadowGuardian-Bridge-<версия>.apk` (пакет `com.jamescrumble.shadowguardian`), запустите один раз и
   закройте: появится папка `/sdcard/Android/data/com.jamescrumble.shadowguardian/files/`.
2. Положите в неё два файла как есть: оригинальный APK игры 1.0.1 и `GloftSGHP.zip` (кэш, **не распаковывать**).
   Имена не важны. Распакованная папка `GloftSGHP/` вместо архива тоже подходит.
3. Откройте приложение и нажмите «Играть». Причина любой ошибки пишется на экране и в `sg.log` в той же папке.

`stretch.flag` в той же папке растягивает картинку на весь экран.

## Как это устроено

```
 APK-обёртка (Java, AArch64)                    дочерний процесс
 ┌──────────────────────────────┐   pipe   ┌──────────────────────────────────────────────┐
 │ MainActivity + SurfaceView   │ ◄──────► │ qemu-arm (наш патч, статический AArch64)       │
 │ GlBridge: исполняет GLES     │  команды │  └ shadowguardian (порт-лоадер, ARM32, glibc) │
 │ AudioTrack, касания, Back    │   GLES   │      ├ bionic ELF loader                      │
 │ PerfHint (ADPF, телеметрия)  │   PCM    │      ├ libshadowguardian.so (игра, движок pig)│
 └──────────────────────────────┘          │      ├ JNI и Android API на заглушках         │
                                           │      ├ GL-мост: сериализует вызовы GLES       │
                                           │      └ zip-монтирование кэша                  │
                                           └──────────────────────────────────────────────┘
```

- **Порт-лоадер** (`port/game/`, `port/portbase/`) загружает библиотеку игры, собранную под bionic, в процесс с glibc
  и подменяет libc, JNI и Android API. Каркас — из [NOVA3 Bridge](https://github.com/JamesCrumble/NOVA3-Bridge).
- **Слой игры** (`port/game/`): порядок вызовов Java-стороны игры (`GameRenderer`, `ShadowGuardian`, `GLResLoader`,
  `Device`), фиктивные Java-классы, касания `nativeOnTouch`, звук через Java `AudioTrack`.
- **Монтирование кэша** (`port/game/zip_mount.cpp`): файл, которого нет на диске, распаковывается из zip в память
  (memfd) при первом обращении; листинги каталогов отдаются из архива; запись всегда идёт на диск (с копированием
  из архива, если файл дописывается). Распакованные файлы на диске имеют приоритет.
- **GL-мост** и **qemu** — как в NOVA3 Bridge: GLES-вызовы идут через pipe и исполняются на GPU в `GLES30`; qemu 11.1.2
  с быстрым float/NEON и быстрым поиском переходов (`qemu/README.md`).

Состояние и найденное по ходу — в [PROJECT_STATE.md](PROJECT_STATE.md).

## Сборка

Сборка идёт в WSL (Ubuntu 20.04). В корень проекта кладутся (в git не входят): `jdk/` (JDK 17), `android-sdk/`
(build-tools 34.0.0, platform 34, platform-tools), `sdl-arm/` (SDL2 под armhf), `qemu/out/qemu-arm-F.stripped`;
нужен системный armhf-тулчейн (`g++-arm-linux-gnueabihf`).

```sh
source env.sh
tools/build_install.sh noinstall   # порт + port/ShadowGuardian-Bridge-debug.apk
tools/push_data.sh                 # APK игры и GloftSGHP.zip из gamebin/ на телефон
tools/launch.sh install            # установить и запустить
tools/pull_log.sh                  # лог -> logs/phone.log
tools/run_pc.sh [кадры]            # запуск на ПК под qemu-arm и llvmpipe (sgdata/: lib/, assets/, res/, *.zip)
tools/make_release.sh              # релизный APK, архив исходников и INSTALL.txt в release/<версия>/
```

Телефон из WSL виден через виндовый `adb.exe`: используйте обёртку `tools/adb`.

## Диагностика

Файлы рядом с логом на телефоне: `engine_env.txt` (`KEY=VALUE` в окружение порта), `resolution.txt`, `notrace.flag`,
`qemu_args.txt`. Полезные ключи: `SG_ZIP_TRACE=1` (каждое обращение к архиву), `SG_IO_TRACE_LIMIT=N` (сколько строк
`io:` писать), `SG_ZIP_CACHE_MB` (кэш распакованных файлов, 256), `SG_ZIP_SPILL=1` (распаковывать на диск вместо памяти).
Раз в 5 с в логе строка `AudioTrack: ... peak N` — уровень звука, который отдаёт игра.

## Другие игры

Тот же способ — qemu-user и мост GLES на GPU телефона:

- [NOVA3 Bridge](https://github.com/JamesCrumble/NOVA3-Bridge) — N.O.V.A. 3 1.0.7
- [Shadowgun Bridge](https://github.com/JamesCrumble/shadowgun-bridge) — SHADOWGUN 1.7.0 (Unity 5.3)
- [Mass Effect Infiltrator Bridge](https://github.com/JamesCrumble/mass-effect-infiltrator-bridge) — Mass Effect Infiltrator 1.0.58

## Лицензия и авторство

GPL-3.0, см. [LICENSE](LICENSE). Происхождение заимствованных частей — в [port/NOTICE.md](port/NOTICE.md). Патч qemu —
GPL-2.0-or-later, как сам QEMU.

Основа порт-лоадера (`port/portbase/`) — порт N.O.V.A. 3 для PortMaster автора EapRules. Слой игры, Android-мост,
патчи qemu и скрипты `tools/` — JamesCrumble.

Shadow Guardian © Gameloft. Проект не связан с правообладателем и не содержит файлов игры.
