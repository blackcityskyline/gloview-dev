# AGENTS.md — правила разработки gloview

Прочитай это перед любыми изменениями. Рабочая память рефакторинга и текущий
план живут в **REFACTORING.md** — начинай сессию с него, заканчивай обновлением.
Детали blur-пайплайна — в **CLAUDE.md**.

## Критически важная информация

1. **Плагин = `.so` в адресном пространстве Hyprland.** Любой краш, use-after-free
   или RASSERT внутри плагина роняет ВСЮ сессию пользователя. Рискованные изменения
   (хуки, порядок проходов, teardown) проверяй в nested Hyprland.
2. **Hot-reload:** `cmake --build build --target reload` — сам делает безопасный
   teardown (`hyprctl gloviewunload`, синхронный hardClose), выгружает обе копии
   (build + hyprpm-кэш) и грузит свежую. Не грузи `plugin load` вручную, пока
   загружена hyprpm-копия: два инстанса не могут поставить hook на
   `shouldRenderWindow` дважды — второй молча останется без хука.
3. **`-fno-gnu-unique` в CMakeLists не удалять.** Без него static-init guard'ы
   становятся STB_GNU_UNIQUE, glibc помечает .so NODELETE, `dlclose` не выгружает,
   и hot-reload молча гоняет СТАРЫЙ код.
4. **Хуки ставятся по demangled-signature substring** (`shouldRenderWindow`,
   `damageSurface`). Ломаются при смене ABI Hyprland (уже ломались на 0.56 —
   namespace move). После обновления Hyprland первым делом проверять их.
5. **currentFB нельзя использовать как источник блюра/полос:** фуллскрин-окно на
   активном воркспейсе идёт через «solitary client» fast path в обход
   `shouldRenderWindow`. Источники — только `backdropSource()` /
   `renderBackdropSource()` (см. комментарий в `renderBackdrop`).
6. **Координаты:** весь chrome пишется в monitor-local LOGICAL пикселях; перед
   любым immediate GL вызовом — через `pxb()/pxr()` (масштаб + `.round()`,
   round обязателен: pixman-регион renderBorder'а trunc'ает дробные боксы → дырки
   в тонких рамках). Live-поверхностиокон конвертируют себя сами.
   Всё, что рисует в приватный FBO, требует `RPT_EXPORT` + `fbSize=(W,H)`
   с сохранением/восстановлением окружающего состояния (см. существующие примеры).
7. **Конфиг читается только через `cfg*`-хелперы** (V2 `value()`), НЕ через
   deprecated `HyprlandAPI::getConfigValue()` — последний игнорирует Lua-конфиг.
8. **Порядок `paint()` менять осознанно** (`render/painter.cpp:185`): backdrop →
   tiles/carousel → ghosts → pulses → strip → strip windows → strip buttons →
   pulses → swapfx → drag → aboveLayers → cursor. Правило: сидящее ПОВЕРХ контента
   рисуется ПОСЛЕ него (кнопки — после своих поверхностей, иначе opaque-контент
   окон затирает их). Два осознанных исключения — в ARCHITECTURE.md: счётчик
   карусели рисуется ДО карточек, `pushOut()` двигает хром из paint.
9. **`m_pendingDeactivate`:** не деактивировать mid-frame — должен отрисоваться
   финальный кадр оверлея, иначе один прозрачный кадр (flicker).
10. **Заголовки Lua (`<lua.h>`, `<lauxlib.h>`) — всегда внутри `extern "C"`.**
    Без него C-функции манглятся в C++-имена и `dlopen` падает с
    `undefined symbol: _Z14luaL_checktype...` — ДО `PLUGIN_INIT`, то есть выглядит
    как «тихая ошибка конфига». Стоило суток. Файлы с Lua C API: `src/anim.cpp`,
    `src/clock/curves.cpp`.
11. **`const double x = cfg::…` в namespace scope — нельзя.** Инициализатор
    выполняется до `PLUGIN_INIT`, конфиг ещё не прочитан.
12. **`hl.config{…}` в пользовательском конфиге — только под `pcall`.** Без него
    ошибка одного ключа обрушивает загрузку конфига, Hyprland выходит молча, без
    падения и без отчёта. `hl.plugin.load` тоже под pcall — он глотает ошибку
    загрузки .so, и это выглядит как «конфиг не подхватился».

## Минные поля (семантика, которую легко сломать)

- `close()`: resume закрытия — `m_timeline.seek(1.0 - m_progress)`; при закрытии
  `m_progress = 1 - timeline.raw()` (инверсия направления!). Менять вместе.
- Blur-кэш: source-identity проверка выполняется КАЖДЫЙ кадр ВНЕ dirty-guard —
  единственный способ поймать смену источника (обои/видео/окно). В dirty-guard
  кэш не инвалидируется никогда.
- Кэш блюра — ЧИСТЫЙ блюр без dim и без анимации: dim рисуется отдельным rect'ом
  поверх блита, fade — только альфа блита `k=eased()`. `blur_strength` означает
  ТОЛЬКО радиус фильтра. Ключ кэша = srcId + passes/size/resolution/strength,
  проверяется КАЖДЫЙ кадр (живой конфиг). Не смешивать с fade-фактором.
- Границы перехода (политика, утверждена пользователем): реальные окна
  переключаются ЖЁСТКО — скрыты с первого кадра open, восстановлены только
  после полного окончания close-анимации. НИКАКИХ фейдов окна: во время глайда
  окно меняет размер и позицию, тающий оригинал под плывущим превью читается
  как задвоение. Бесшовность дают превью, попиксельно идентичные реальным
  окнам в natural-боксах на обоих концах. Фейд — только у блюра (кроссфейд
  бэкдропа). Превью не наследуют WINDOW_ALPHA_FADE в windowRealAlpha.
- Tween: длительности читаются на месте КАЖДЫЙ кадр (живой отклик на смену конфига),
  Tween владеет только стартовой точкой.

## Сборка и проверка

```bash
cmake --build build -j$(nproc)          # цель build/gloview.so; зелёная сборка = минимум
cmake --build build --target reload     # пересобрать + горячо перезагрузить в живой Hyprland
cd build && ctest --output-on-failure  # 4 шва: layout_carousel, recency, carousel_exit, config_keys
```

Правку `src/anim.cpp` / `src/clock/curves.cpp` (границы `extern "C"`) hot-reload
не подхватит — нужен перезапуск сессии: сбой на `dlopen`, а не в рантайме.

**Падение без coredump — читай первым:**

```bash
ls -t ~/.cache/hyprland/hyprlandCrashReport*.txt | head -1        # точный стек
tail -100 /run/user/1000/hypr/$(ls -t /run/user/1000/hypr | head -1)/hyprland.log
```

`Hyprland --verify-config` как диагностика бесполезен: роняет композитор и БЕЗ
плагина, подсовывая чужие core-дампы. Не делай `rm -rf /run/user/1000/hypr/*` —
вместе с мусором уходят логи живых и упавших сессий.

Окружение базовой сессии: Hyprland 0.56.2, lua 5.5.1, GLES 3.2, C++23.
Критерий готовности шага: сборка без error/warning + поведение не изменилось
(если шаг не багфикс/фича). Визуальные проверки перечислены в REFACTORING.md.

## Карта исходников (`src/`)

19 `.cpp`, плоско: слои из `ARCHITECTURE.md`. Подробности — там.

| Файл | Ответственность |
|---|---|
| `overview.hpp` | класс Overview, состояние сессии, единственный владелец `m_active` |
| `main.cpp` | регистрация: config-таблицы, единый реестр действий kActions |
| `anim.hpp` / `anim.cpp` | **реестр анимаций**: enum Id, ключи конфига, дефолты, параметры формы, Lua-правила. `config.*` о нём не знает |
| `config/config.hpp` / `.cpp` | типизированная схема значений + таблицы `kInts`/`kFloats`/`kStrs`/`kColors` |
| `session/session.cpp` | lifecycle (open/close/hardClose/deactivate), хуки, геометрия стрипа |
| `build/build.cpp` | buildTiles/buildStrip/layoutTiles/replayReflow/syncTiles |
| `input/mouse.cpp`, `input/keys.cpp` | мышь, клавиатура, alt-tab, digit-jump |
| `actions/actions.cpp` | drop/swap/add workspace, switchToWorkspace, stepWorkspace |
| `render/painter.cpp` | renderStage + PainterPass, единственная точка регистрации прохода |
| `render/backdrop.cpp` | бэкдроп: композит, кэш, prime-фаза, отложенное открытие |
| `render/tile_view.cpp`, `carousel_view.cpp`, `strip_view.cpp`, `fx.cpp` | хром плиток, карусель, стрип, эффекты |
| `render/window_content.cpp` | контент окна через публичный `IHyprRenderer::draw` |
| `clock/clock.cpp`, `clocks.cpp`, `curves.cpp` | домен времени: tween, прогрессы, resolved anim cfg, реестр кривых |
| `pixel/cache.cpp` | кэши: подписи, глифы, снапшоты |
| `model/` | сцена: tiles, strip, drag, recency, effects |
| `blur.*` | self-contained dual-Kawase+gaussian (детали в CLAUDE.md) |
| `cursor.*` | HW/SW курсор поверх оверлея (следствие RENDER_LAST_MOMENT) |
| `layout.hpp/.cpp` | чистая геометрия лейаута, без Hyprland (единственный юнит-домен) |
| `tests/` | ctest: `layout_carousel_check`, `recency_check`, `carousel_exit_check`, `config_keys_check` |

## Правила работы

- **Сессии:** начал — прочёл REFACTORING.md; сделал шаг — отметил в нём же
  (чекбокс, строка в «Результаты», запись в «Лог сессий»).
- **Один логический шаг = один коммит.** Коммит только после зелёной сборки.
  Стиль сообщений: `type(scope): summary` на английском, тело объясняет why
  (примеры в `git log`).
- **Комментарии в коде** — английские, документируют «почему» и верифицированные
  факты о внутренностях Hyprland (файл:строка источника). Археологию удалённых
  багов в комментариях не плодить — для этого git history. Общие эссе — в общий
  заголовок (`src/render/gl_util.hpp` как образец), не копировать между TU.
- **Дедупликация прежде копипасты:** повторившийся 3+ раз паттерн → хелпер
  (`captureCurrentBoxes`, `releaseNewWorkspaces` — образцы). Хит-тесты — через
  `LRect::contains`.
- **Новые карты по окнам** не ключевые raw-указателями (ABA при переиспользовании
  адреса) — PHLWINDOWREF или явный жизненный цикл с очисткой.
- Стиль: 2 пробела, clang-format-подобный (как в большинстве файлов);
  `layout.*` исторически 4 пробела — не переформатировать целиком.
- Без эмодзи. README/CLAUDE.md обновлять, если меняется пользовательское
  поведение или добавляются конфиги.

## Чеклист перед коммитом

- [ ] сборка зелёная (error И warning)
- [ ] нет обращений к удалённым членам/API (grep по имени)
- [ ] новые конфиги: в `src/config/config.cpp` (`kInts`/`kFloats`/`kStrs`/`kColors`)
      или `src/anim.cpp` (`kRows`), описаны в README, зелёный `ctest` (ловит
      «поле есть, регистрации нет»)
- [ ] поведение по умолчанию не изменилось (если это не задумано)
- [ ] REFACTORING.md обновлён
