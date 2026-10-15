# Shadow Guardian Bridge (Shadow Guardian HD 1.0.1 -> Android APK для OnePlus 13s): состояние проекта

Цель: то же, что в `../nova3probe` (qemu-user в APK + мост GLES на GPU телефона). Каркас (portbase, обёртка,
qemu, инструменты) — из `../nova3probe` (решение пользователя 14.10: тот же разработчик, Gameloft). Слой игры
(`port/game/`) написан заново. Обновлено: 2026-10-15.

## Правила работы
- По-русски, кратко. Глобально ничего не ставить: тулчейны скопированы из соседних проектов.
- git: только локальный репозиторий (ветка master), удалённого нет.
- Кэш игры: zip кладётся как есть, порт монтирует его как папку данных (не распаковывать).

## Игра
- `gamebin/Shadow Guardian HD v1.0.1(All GPU noChk).apk` (пакет `com.gameloft.android.ANMP.GloftSGHP.ML`),
  `gamebin/GloftSGHP.zip` (3746 файлов, 1,28 ГБ распакованно, всё deflate).
- `libshadowguardian.so` (3,8 МБ): движок Gameloft **pig** (`pig::`, уровни `clara::`, звук `vox::`, Lua),
  только GLES2. Совпадений движка с соседними проектами нет. `libStormGLOFT.so` — сторонний хук мода
  «All GPU» (inline-хуки, GLSL-компилятор), порт его не грузит.
- Пути движка: `/sdcard/gameloft/games/GloftSGHP` (данные) -> `<game>/GloftSGHP`,
  `/data/data/<пакет>` -> записываемая папка (`game/io_paths.cpp`).
- Порядок запуска (dex): `JNI_OnLoad` -> `nativeGetInfo` -> `GLResLoader.nativeInit` -> `Device.nativeInit` ->
  `GameRenderer.nativeInit(производитель, ш, в, язык)` -> `ShadowGuardian.nativeInit` -> `nativeResize` ->
  `nativeRender`; кадр показывает порт после каждого `nativeRender` (`game/main.cpp`).
- Касания: `GameGLSurfaceView.nativeOnTouch(действие, x, y, id)`, 1 вниз / 2 движение / 0 вверх.
- Звук: Java `AudioTrack` (`game/jni/audio_track.cpp`, из Wild Blood).

## Монтирование zip (`game/zip_mount.cpp`)
- Файл, которого нет на диске под `<game>/GloftSGHP`, распаковывается из zip в memfd, путь открытия
  подменяется на `/proc/self/fd/N`. Диск приоритетнее архива. LRU распакованных файлов `SG_ZIP_CACHE_MB` (256).
- Если memfd/переоткрытие недоступно — распаковка по файлам в `<writable>/zipcache` (`SG_ZIP_SPILL=1` принудительно).
- Листинг каталогов: `opendir` на каталог из архива отдаёт теневой каталог `<writable>/.zipdirs/...` с пустыми
  файлами-именами (хук `port_fix_dir_path` в `portbase/src/symtab_io.cpp`). Без него у материалов не было
  проходов рендера (движок перечисляет `shaders_galaxy_s/`).
- Обёртка находит `*.zip` рядом с APK игры и передаёт путь в `SG_CACHE_ZIP`; распакованная папка `GloftSGHP/` тоже работает.

- Запись поверх архива: открытие на запись (`fopen` с w/a/+, `open` с O_CREAT/O_WRONLY/O_RDWR) всегда идёт в файл
  на диске; при сохранении содержимого (r+, a, без O_TRUNC) файл сначала копируется из архива. Хук
  `port_fix_path_write` (portbase `symtab_io.cpp`). До этого `settings` писался в memfd и терялся при выходе.
- Относительные имена (`music/x.wav`) ищутся в папке данных (на диске, затем в архиве).

## Правки portbase
- `__aeabi_{i,u,l,ul}divmod` -> libgcc (сгенерированные возвращали структуру через скрытый указатель -> SIGFPE),
  как в Shadowgun. `__aeabi_d2lz`, `operator new/new[]` nothrow, `std::nothrow`, `__aeabi_unwind_cpp_pr0/1`
  (`src/symtab_cxx.cpp`), `frexpf` (`src/symtab_libm.cpp`).
- Мост GL: `glStencilOpSeparate`, `glValidateProgram` (генератор), `glBindAttribLocation` (новая операция, как в ME).

## Сделано (2026-10-14)
- ПК (`tools/run_pc.sh`): 200 кадров без падений.
- Телефон (`tools/build_install.sh`, `tools/push_data.sh`, `tools/launch.sh`, `tools/pull_log.sh`):
  **главное меню, 60 fps** (`logs/phone_2.png`).

## Сессия 2026-10-15 (замечания пользователя: растянуто, нет «назад», нет звука)
- Растяжение: интерфейс игры свёрстан под 800x480 (5:3) и растягивается на заданный размер. Рендер теперь 5:3
  (1200x720), картинка по центру с чёрными полями (`MainActivity`: сцена с `SurfaceView` и оверлеем касаний).
  `stretch.flag` рядом с логом — на весь экран. При 5:3 игра сама берёт набор спрайтов `sprites_1_6`.
- Назад: Java игры передаёт в `nativeOnKeyDown/Up` сырые Android-коды (Back 4, Menu 82 у неё особые). Обёртка шлёт
  `key 4`; в лог добавлена строка `input: key N`. В бою есть и своя кнопка паузы (левый верхний угол).
- Звук: игра грузит `music/m_title.wav`, `sounds/*.wav` из архива; строка `AudioTrack: ... peak N` раз в 5 с.
  Прежняя «тишина» — прогоны при погашенном экране (игра на паузе). Ошибка `GS_SetupSound: IsMusicPlaying` —
  отладочное сообщение движка, не падение.
- Диагностика: `SG_IO_TRACE_LIMIT` (сколько строк `io:` писать), `SG_ZIP_TRACE=1` (каждое обращение к архиву),
  строки `io: fopen(...) for writing`, `storage: ...` (файлы игры на старте).
- Проверено на телефоне 15.10: пропорция 5:3 с полями, 60 fps; Back доходит до игры (`input: key 4`) и в бою
  открывает меню паузы; звук есть (пики PCM непрерывные, пользователь подтвердил на слух); `settings` пишется на
  диск; продолжение сохранённой игры работает (бой на первом уровне, HUD на месте).

## Не проверено / не сделано
- Прохождение уровней дальше первого, долгая игра без падений, пауза/возврат в приложение (сворачивание).
- `Game.cpp:896 Exp: res` при старте (что не загрузилось — не разобрано); лицензионная проверка ALC
  (`ConfigFile: server/key` пустые) — влияние не выяснено.
- Геймпад не подключён; ролик `video/logo.m4v` не проигрывается.
- Экран настроек и тексты обёртки остались от NOVA 3 (FOV, XML-профили и т. п. — к этой игре не относятся).
