# REFACTORING — рабочая память

Правила сессий и карта исходников — в AGENTS.md. Детали blur-пайплайна — в
CLAUDE.md. Этот файл: текущая архитектура, открытые вопросы, ловушки, журнал.

## Ядро рендера (v5, стабильно)

Один `PainterPass` (src/render/painter.cpp) вместо шести фаз COverlayPass:
BUILD-время только готовит состояние (clocks → hover/sync/snapshots →
ws-follow), EXECUTION рисует немедленно через z-слоты Z0 backdrop → Z1 tiles →
Z2 strip → Z2.5 swapfx → Z3 drag → Z4 pulses/aboveLayers → Z6 cursor, tail =
re-arm + teardown. Контент — через публичный `IHyprRenderer::draw(SRenderData,
CRegion)` (pinned HL 0.56.2, Renderer.hpp:162): очередь не переопределяется,
painter задаёт только ПОРЯДОК. Главное правило порядка: сидящее ПОВЕРХ
контента рисуется ПОСЛЕ него. Во время EXECUTION нельзя `m_renderPass.add()`
(range-for по элементам, Pass.cpp:187).

Три хранилища: **Model** (tiles/strip/drag — только события), **Clocks**
(anim/clocks.cpp — единственный домен времени; во время paint read-only),
**Pixels** (кэши блюра/лейблов/snapshots — только в моменты захвата).
Paint = чистая функция (Model, Clocks, Pixels) → пиксели.

## Модель анимаций (v2, декларативная)

Лист = `{enabled, speed, curve}` (`<leaf>_enabled/_speed/_curve`), база
каждого листа — в таблице kLeaves (config.cpp): effective = base/speed,
пол 16мс. Кривые — один реестр (anim/curves.cpp): нативные fn + Lua-функции +
Lua-безье `{type="bezier", points={{x,y},{x,y}} | {x1,y1,x2,y2}}` (Ньютон +
бисекция). Поверх ключей — императивные правила `gloview.animation{...}`
(AnimRule: enabled/speed/ms/curve/style; заданное выигрывает, остальное
наследуется). Стили типов: `<thing>_style` (ws_enter/ws_exit/grid_swap/
strip_swap), читаются только через cfg::animStyle(). Tween владеет стартом;
длительности резолвятся каждый кадр.

Листья: open, close, glide (плитка слот↔слот), appear (каскад новичков),
card («+»), pulse (кольцо), strip_step, ws_enter/ws_exit (смена воркспейса),
expo_in/expo_out (флип all↔one), swap_main/swap_partner (обмены),
drag (ЗАХВАТ И ОТПУСКАНИЕ превью — оба направления одним листом).

Переходные листья выбираются селекторами с приоритетом: флип all↔one
(`m_expoFlip`: expo_in/expo_out) > слайд смены воркспейса (`m_wsSlideDir`:
ws_enter/ws_exit) > appear. Обе стороны перехода едят ОДИН clock
(m_rebuildClock), каждая в своём окне; флаги сбрасываются, когда ОБА окна
закрылись. Все листья резолвятся ОДИН раз за кадр в updateAnimation (кэш
m_glide/m_entry/m_ghost/m_lift/m_enterStyle/m_exitStyle); paint конфиг не
читает. Предикат занятости насоса — ровно один: animBusy().

## Открытые вопросы

- Ховер-заливка плиток/карточек («Баг 2»): не подтверждена кодом — ждём скрин
  воспроизведения.
- Истинный халф-сплит при вставке («Баг 3»): ждёт layout-insert API
  (CFocusState::rawWindowFocus абортит на кросс-воркспейсном фокусе).
- Лаг курсора в all-workspaces idle: не воспроизводился инструментально;
  переоткрыть при жалобе.

## Ловушки (проверено)

- Релоад из шелла с протухшим HYPRLAND_INSTANCE_SIGNATURE уходит в мёртвый
  сокет МОЛЧА: сборка «ok», живая сессия держит старый .so. Перед тестом:
  `export HYPRLAND_INSTANCE_SIGNATURE=$(ls -t /run/user/1000/hypr | head -1)`;
  после фикса — выверка по F-трейсу или новому ключу через getoption.
- eval-переключения флагов на живом оверлее — недостоверный A/B; только reload.
- Ключи конфиг-схемы — ТОЛЬКО строковые литералы (CIntValue хранит const
  char*; c_str() временного std::string виснет → краш при регистрации).
- hyprctl getoption читает V2-значения плагина — штатная проверка живых
  параметров.

## Журнал сессий

- 2026-08-26 (ночь, 3): анимационная модель v2 (71eae61) — единые поля
  {enabled,speed,curve} вместо _ms/-1/duration, безье из Lua
  (`gloview.curve(name,{bezier|fn})`), правила `gloview.animation{...}`
  поверх ключей; lift→drag (захват+отпускание одним листом), new_card→card.
  Зонд DRAG LIFT в updateHover — для проверки стрип-драга в живой сессии.
- 2026-08-26 (ночь, 2): симметричный lift. Все ручные отпускания (грид в
  пустоту, миниатюра на свою карточку/мимо, перелёты на карточки) летят на
  листе `lift` — те же ручки, что и захват; возврат стрипа раньше был
  честный snap-back (`damage(); return`). Грид возвращается через
  beginSwapFX(Horizontal) с settle natural=target (FX садится точно в слот),
  остальные плитки едут своим replayReflow. Группа `drop` удалена за
  ненадобностью (последний потребитель — ручные посадки); свапы остаются на
  swap_main/swap_partner. Новая ручка look.drag_size уже была; в lua добавлены
  swap_*_enabled.
- 2026-08-26 (ночь): механизм захвата драга (lift). Захват (порог ~8px) теперь
  стартует ПОЛЁТ превью из его слота к якорю у курсора: model::Drag.fromBox
  фиксируется в момент lift, dragVisualBox() интерполирует from→цель на листе
  `lift` (цель едет за курсором — полёт естественно догоняет), chrome и
  контент грида и стрипа едят ОДИН бокс. Размер превью в фазе драга —
  `look.drag_size` (доля слота, 0.55 по умолчанию, грид и стрип единообразно;
  desktop-mode не сжимается). Точки посадки при релизе честные — из
  визуального бокса, даже если отпустили посреди полёта.
- 2026-08-26 (вечер): хардненинг anim-ядра после крит-ревью — единый предикат
  насоса animBusy() (превью три копии предиката уже разъехались: tick насоса
  не видел swapfx и мог разоружиться посреди полёта), stall-guard теперь
  накрывает ВСЕ часы, Tile.appear помечается settled (раньше плитки вечно
  резолвили конфиг после въезда), prune swapfx по собственному fx.ms, fade —
  чистая альфа без pop-scale, _enabled для swap_main/swap_partner. Стилевые
  ключи переименованы: *_anim → *_style (ws_enter/ws_exit/grid_swap/
  strip_swap); парсинг стилей — один parseSwapStyle; lerp переехал в
  layout.hpp; покадровый кэш листьев убрал аллокации из per-tile пути.
- 2026-08-26: найдено и закрыто (5dee8fc) — группа анимаций f987711 была
  проводкой мёртвым кодом: флаг направления never armed (oldWs захватывался
  после reassign m_workspace), expo-половины и lift-clock ни к чему не были
  подключены, expo_*_enabled не регистрировались (unknown config key),
  dropOnStripCard терял return true. Живой тест: слайды воркспейсов и expo
  работают.
- 2026-08-24/23: R3 (PainterPass + сплит src/), A1 (реестр кривых + Lua),
  C1/D1 (config/debug модули), S1/M1/I1 (иерархия каталогов) — детали в git
  log; план v5 выполнен полностью.
