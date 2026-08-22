# REFACTORING v2 — план ужатия до ~5000 строк

## Суть текущего состояния

gloview — Hyprland-плагин «Mission Control» (сетка окон + стрип воркспейсов,
blur-бэкдроп, alt-tab). Ранний рефакторинг R1–R5/S1–S13 выполнен: blur-кэш с
ключом srcId+рецепт, Tween-часы, таблицы конфига в main.cpp, единый реестр
действий, event-loop pump. Открытый баг: 1–3 чёрных кадра при входе
(CANDIDATES.md; debug:pass=1 не помог ⇒ виновник НЕ simplify/damage — кадры
коммитятся кем-то до первого полного рендера; расследование в отдельном треке).
Объём сейчас: **5410 кодовых строк** (без комментариев/пустых).

## Цель

~5000 кодовых строк без потери функционала: минимум точек отказа, ноль
хардкода цветов, качество анимаций/блюра сохранено. Каждый шаг = зелёная
сборка + коммит + визуальная проверка (поведение не изменилось).

| Файл | Код | Файл | Код |
|---|---|---|---|
| overview_render.cpp | 861 | overview_keys.cpp | 405 |
| overview_core.cpp | 745 | blur.cpp | 365 |
| overview.hpp | 356 | tiles_render.cpp | 303 |
| overview_build.cpp | 467 | actions/main/layout/… | 1005 |

Не трогать: `blur.*` (самодостаточен), `layout.*` (чистая геометрия),
`cursor.*`, `overlay_gl.hpp`.

## Задачи

### C1. Цвета — полная перестройка (первой!)
Модель как в hyprbars: **ноль хардкода, всё через конфиг.**
- Удалить `schemeGradient()` и всю machinery `CConfigValue<IComplexConfigValue>`
  (whitelist градиентов Hyprland, ~70 строк + комментарии риска SIGABRT).
- Новый источник схемы: опция `palette` — map ключ→hex, задаётся из Lua:
  `palette = require("noctalia.noctalia-colors-extended")` (опционально;
  нет require — работают только литералы).
- Чтение любого цвета: значение опции — ключ палитры? берём его : иначе
  parseHexColor. Динамика: noctalia меняет тему → переоценка конфига → живое
  применение (читаем каждый кадр, как остальные cfg*).
- Инлайн-литералы в коде УДАЛИТЬ: `argb(0xff14181f,.08)` ×2 → новые опции
  `tile_backing_color` / `drag_backing_color`; `argb(0xcc11151c,e)` →
  `strip_win_backing_color`.
- Текущие дефолты (0x73070a10, 0x70000000, 0xf0ffffff, …) перенести в
  `~/.config/hypr/plugins/gloview.lua` явными значениями + palette=require.
  Кодовые фолбэки — нейтральные (чёрный/белый с альфой).
- README: раздел «Цвета» с примером hyprbars-стиля.
Оценка: **−80…100 строк**, исчезает самый хрупкий подсистемный путь.

### S1. Модель состояния — переосмысление (главный шаг)

Не выбрасывание, а смена модели — 5 подсистем:

**a) Плитки = часы.** Убрать m_reflow/m_reflowing/m_reflowRaw и ветвление в
tileBaseProgress(). Единая модель: плитка ВСЕГДА рисуется по `tileClock.raw()`;
любое изменение лейаута (open, drop, syncTiles, desktop-флип, close) = 
«перезадать target + перезапустить часы». Хром живёт на master-часах, плитки —
на своих; никаких режимных флагов. Оценка: −50…70, минус целый класс
рассинхронизаций chrome/tiles.

**b) Один Drag вместо 11 членов.** m_pressTile, m_dragging, m_pressX/Y,
m_grabDX/DY, m_dragX/Y, m_pressButton, m_pressStripItem, m_pressStripWin,
m_dragStripWin → `struct Drag{enum{None,Grid,Strip} src; int idx,win;
PHLWINDOWREF w; V2 press,grabOff,cur; int button;} dragging() = src!=None.
Уже дважды ловили баг «полунаследованного драга» (комментарии в open()) —
структура с одним полем src убивает класс насовсем. Оценка: −30…50 net.

**c) Double-click: 4 члена → 2.** m_pendingClickWin + таймер кодируют всё:
m_lastClickWin/m_lastClickTime лишние — «второй клик до срабатывания таймера»
проверяется наличием armed-таймера с тем же окном. Оценка: −15…25.

**d) canvasPos внутрь Tile.** m_canvasPos (map по raw-указателю — нарушение
собственного правила AGENTS про ABA!) → флаг `parked` + target прямо в Tile:
desktop-режим = «target не перезаписывать у parked». Один источник истины
«где превью». Оценка: −25…40.

**e) m_altTabRank map<void*,int> → vector<PHLWINDOWREF>** (порядок = ранг;
поиск линейный по ≤128 — бесплатно). Вторая raw-keyed карта исчезает.
StripItem: 4 bool → enum Kind{Ws,Plus,All}+virtual. dbg → dbgf(fmt,...).
Оценка: −20…35.

### C3. Хром плиток/стрипа — унификация
`drawPreviewTile` / `drawDragStripChrome` / card-chrome внутри `renderStrip`
→ одна параметризованная `drawCardChrome(box, opts{shadow,ring,backing})`.
Оценка: **−100…150**.

### C4. COverlayPass — компактнее
6 фаз оставить (z-порядок поверхностей честный), но switch → таблица
указателей на методы; общие guard'ы в конструктор. Оценка: **−20…30**.

### C5. Конфиг-хелперы после C1
`cfgColorScheme` вырождается в однострочник; сжать `parseHexColor`.
Оценка: **−15…25**.

### C6. Прочистка
Мёртвые члены overview.hpp, дублирующиеся guard-условия.
Оценка: **−40…60**.

**Итого: −475…700 → ≈4710…4935 строк.**

## Порядок

C1 (цвета) → C5 (конфиг-хелперы) → S1 (модель состояния: часы→драг→клик→
canvas→ранги) → C3 (хром) → C4 (фазы) → C6. После каждого шага: сборка без
warning, коммит `refactor(scope): …`, визуальная проверка.

## Отдельный трек (вне объёма)

Black blink при входе — см. CANDIDATES.md. Следующий шаг: выяснить, кто
коммитит первые кадры (trace вокруг `commitPendingAndDoExplicitSync` /
`scheduleFrame` в момент open()).

## Лог

- 2026-08-22: план зафиксирован, старт с C1.
