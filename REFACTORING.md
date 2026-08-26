# REFACTORING v5 — ядро рендера «один художник»

Предыдущие планы выполнены и закоммичены: v4 (AN1–AN6 анимации + Bug B финал,
ed0f3d8), шаги A/B/C frost-underlay бисекта (d9a4aed). Их детали — в git log.

## Диагноз (почему баги были неизбежны)

Гибрид двух планировщиков: превью окон ставятся в очередь как
CSurfacePassElement на BUILD-времени (хук RENDER_LAST_MOMENT), а хром рисуется
immediate GL изнутри 6 фаз COverlayPass на EXECUTION-времени. Фазы Back /
Buttons / Mid / StripButtons / DragBack / Front существуют ТОЛЬКО чтобы хром
вклинивался между чужими элементами. Каждая фича = манипуляция порядком в
чужом конвейере; контент тянет всю машину Hyprland (blur-пути xray/live,
currentWindow, simplify/occlusion по ЧУЖИМ boundingBox'ам, damage-эры,
preBlur) — ни одним из этих состояний мы не управляем. Отсюда весь класс
«миганий/швов/призраков» и краши на ровном месте (шаг C упал именно здесь).

## Новое ядро

Один pass-элемент. Один стек вызовов. Прямой порядок рисователя. Всё окно
контента рисуется НЕМЕДЛЕННО внутри painter'а через публичный примитHyprland:

    IHyprRenderer::draw(const CSurfacePassElement::SRenderData&, const CRegion&)
    — Renderer.hpp:162, Renderer.cpp:859 → elementRenderer()->drawElement →
    preDrawSurface/drawSurface, СИНХРОННО со всей семантикой очереди:
      * ALPHA = alpha × fadeAlpha × m_alphaModifier × m_overallOpacity
        (ElementRenderer.cpp:247-248) — per-surface альфы бесплатны;
      * calculateUVForSurface — small()/viewporter/source/expand_undersized
        (ElementRenderer.cpp:53-154);
      * presentFeedback(when, monitor) — frame-done колбэки клиентам, включая
        discard-путь (ElementRenderer.cpp:390, SurfacePassElement.cpp:181);
      * blend-стейт + CANDISABLEBLEND (295-301, 388);
      * учёт async dmabuf-буферов для release/sync (404-406);
      * scoping clipBox / currentWindow / transform-push (394-411).
    Т.е. immediate-лист НЕ переопределяет НИЧЕГО. Painter задаёт только ПОРЯДОК.

draw() painter'а (z-слоты фиксированы):

    Z0 backdrop()            // кэш-блюр + dim (как сегодня)
    Z1 grid-tiles            // на плитку: frost/backing/border → content → chrome/label/✕
    Z2 strip                 // band → карточки → thumb'ы (тот же лист) → hints/pulses
    Z3 drag                  // drag-chrome → drag-content
    Z4 pulses/hints
    Z5 aboveLayers           // opted-in TOP/OVERLAY слои, тем же листом
    Z6 cursor                // HW/SW модуль
    tail: rearmanim()        // re-arm В КОНЦЕ исполнения пасса (не на build!)

Расширение = новая строка на своей z-позиции. Бисект краша/бага = одна строка
painter'а, один контекст исполнения.

### Три раздельных хранилища состояния (дисциплинарный закон)

| Хранилище | Что | Кто мутирует |
|---|---|---|
| Model | m_tiles/m_strip/m_drag/ws-refs | только события (input/rebuild/core); НИКОГДА paint |
| Clocks | Tween'ы (timeline/tileClock/populate/strip/pulses/newCard) | один раз за кадр в updateAnimation ДО paint; во время paint read-only |
| Pixels | кэш бэкдропа (m_blur, m_backdropSrcFB), label/glyph-кэши, snapshots | только в явные моменты захвата (open/config/layer-commit/build) |

Paint = чистая функция (Model, Clocks, Pixels) → пиксели. Любой визуальный баг
= инспекция одного из трёх хранилищ, а не гонка двух планировщиков.

### Жизненный цикл кадра

renderStage (BUILD): updateAnimation → updateHover/syncTiles/updateSnapshots/
ws-follow → добавить ОДИН PainterPass (+ ничего больше) → pendingDeactivate.
PainterPass::draw (EXECUTION): z-слоты выше. Пост-условие: busy → forceFullFrames.

### Контракт листа drawWindowContent(w, boxPx, clipPx, alpha, when, round, roundPow)

Один вызов renderWindowLive с внутренним выбором маршрута:
  - snapshot-режим: тот же SRenderData с texture=frozen → один draw;
  - live: breadthfirst по дереву поверхностей → draw на узел;
  - translate+scale modif: при immediate — save/set/restore
    m_renderData.renderModif вручную (drawHints делает ровно это,
    ElementRenderer.cpp:190-194); при очереди — hints-элементы как сегодня.
Все внешние текстуры за ok()-гейтами; SP поверхностей удерживается деревом
окон (Model) на время paint — lifetime безопасен.

### Верифицированные факты о каркасе кадра (pinned HLsrc 0.56.2)

* renderMonitor: beginRender очищает pass → renderWorkspace ставит стандартные
  элементы (bg/bottom/окна/top/overlay) → lockscreen+IME ДО LAST_MOMENT
  (Renderer.cpp:2176-2179) → SW-курсоры (2216) → RENDER_LAST_MOMENT (2227) —
  наш BUILD → endRender → GLRenderer::endRender:88 → m_renderPass.render().
* Pass.render (Pass.cpp:107-208): кэширует needsLiveBlur/precompute по ВСЕМ
  элементам ДО исполнения; disableSimplification ⇒ всем полный damage;
  исполнение = m_renderData.damage = elementDamage; draw(element).
  EK_CUSTOM → drawCustom (ElementRenderer.cpp:626) вызывает draw() и тут же
  исполняет возвращённых детей — но нам проще всё делать самим внутри draw().
* ГЛАВНАЯ МИНА: во время EXECUTION нельзя m_renderPass.add() — range-for по
  вектору элементов (Pass.cpp:187). Immediate-маршрут не добавляет НИЧЕГО.
* needsLiveBlur у painter'а покрывает backdrop+band одним элементом
  (boundingBox = весь монитор, как сейчас у фаз Back/Mid).
* disableSimplification()=true и undiscardable()=true остаются: чужие
  opaqueRegion'ы не должны опустошать damage (и в painter-мире они просто не
  попадают в pass-list вовсе).
* m_pendingDeactivate: painter НЕ проверяет m_active на EXECUTION (только
  owner!=null) — иначе финальный кадр закрытия станет прозрачным (мина
  уже существовала в фазах, сохранить семантику).

### Известное принятое ограничение

Локскрин/IME рисуются Hyprland'ом до LAST_MOMENT — перекрываются нами, как и
сегодня (Renderer.cpp:2176-2179). Не трогаем.

## Шаги (каждый: зелёная сборка + визуальная проверка + коммит)

| Шаг | Что | Гейт |
|---|---|---|
| R1 | renderWindowLive учится immediate-маршруту за флагом `immediate_surfaces` (дефолт 0); при флаге стрип-миниатюры рисуются из Phase::Mid (z-позиция идентична очереди) | стабильность стрипа + живое видео в миниатюрах; A/B переключением флага без пересборки |
| R2 | грид-плитки на immediate (Back рисует хром, контент следом тем же листом); мороз всегда под eligible | пиксель-паритет с очередью (A/B флагом) |
| R3 | схлопывание 6 фаз в один PainterPass (z-слоты); кнопки/hints/pulses/drag/cursor внутрь; aboveLayers тем же листом; snapshot-ветка на immediate; флаг удалить | полный цикл: open/close/drag/drop/swap/expo/desktop/alt-tab |
| R4 | чистка мёртвого кода (blockBlurOptimization-ветки и пр.), README | сборка без warning |

### Результаты

- **R1 реализован** (ждёт визуальной проверки): ветка маршрута внутри
  renderWindowLive (`execCtx && immediate_surfaces`), Phase::Mid вызывает
  renderStripWindows(true) при флаге; build-time вызов при флаге пропускается —
  взаимно исключающие условия, т.е. нет двойного рисования и нет m_renderPass.add
  из EXECUTION. Попутно закрыта ЛАТЕТНАЯ МИНА: после отката f66b9fe
  windowBlurEligible остался в anonymous namespace → tiles TU ссылался на
  НЕСУЩЕСТВУЮЩИЙ символ (nm: `U gloview::windowBlurEligible` в готовом .so).
  Вынесен в gloview-scope: T-символ, сборка без warning.

## A1 — ВЫПОЛНЕН: реестр кривых + Lua-кривые

Принцип: Clocks = чистое линейное время (Tween.raw), лист = данные
{enabled, ms, curve-id} (AN1), шейпинг = одна точка. Захардкожен только
Curve enum — его заменяет реестр:

    src/anim/curves.{hpp,cpp}   (файлы появятся в R3-сплите)
      evalCurve(id, t)                 — единственная точка шейпинга
      реестр: имя → native fn | lua ref (luaL_ref)

- Конфиг не меняется: <leaf>_curve уже строка; реестр резолвит больше имён.
- Нативные кривые — указатели функций (linear/easeout/easeinout/back/...).
- Lua: gloview.curve("name", function(t) return ... end) — регистратор через
  addLuaFunction, eval = lua_rawgeti + pcall (~µs, единицы вызовов/кадр).
- Безопасность: pcall-ошибка/nil/NaN → linear + однократный dbg. Результат
  НЕ клампится (overshoot легален).
- Запреты: никакого шейпинга в painter/paint; никаких кривых внутри Tween;
  keyframe-DSL не нужен (Lua-функция покрывает любую форму).
- Реализация: anim/curves.{hpp,cpp}, namespace gloview::curves (не `anim` —
  коллизия с методом Overview::anim). Нативные: linear/easeout/easeinout/
  back. Lua: hl.plugin.gloview.curve(name, fn) → luaL_ref; eval = rawgeti +
  pcall; ошибка/не-число/неизвестное имя → easeout + warn-once (в лог
  Hyprland, не в dbg-файл). AnimCfg.curve теперь std::string; enum Curve и
  curveFromName удалены. README: раздел "Custom curves (Lua)".

## R3 — ВЫПОЛНЕН: один PainterPass + сплит src/render/, src/anim/

- Очередь поверхностей и флаг immediate_surfaces УДАЛЕНЫ: весь контент
  (грид, ghost'ы, стрип-миниатюры, драг, aboveLayers) рисуется немедленно
  через лист window_content.cpp; шесть фаз COverlayPass заменены одним
  PainterPass (painter.cpp) с фиксированными z-слотами Z0..Z5 + tail
  (rearm + teardown). Правило порядка: «сидящее ПОВЕРХ контента рисуется
  ПОСЛЕ него» — единственное правило вместо фазовой машины.
- renderStage = чистый BUILD: clocks → hover/sync/snapshots → ws-follow →
  один PainterPass → forceFullFrames. Прекрасный след F-трейса сохранён;
  PRE-probe замороженного Bug A удалён (протокол остался в CANDIDATES.md).
- Мёртвый код не перенесён: windowBlurEligible (осиротел после переработки
  frost), Overview::closing(), миграционные execCtx/immediateSurfaces,
  hints-элементы, COverlayPass. Stale-комментарий «0.55.4» → 0.56.2.
- Структура: render/{painter,backdrop,window_content(.hpp),tile_view,
  strip_view,fx,gl_util.hpp} + anim/clocks.cpp (eased/animDuration/
  tileProgress/tileAppear/currentBox/updateAnimation/newCardScale/
  animateStripTo — единственный домен, трогающий время). A1 добавит сюда
  реестр кривых.
- Порядок Z4/Z5: above-layers теперь ПОД курсором (в queue-эпоху HUD
  очередью попадал над SW-курсором случайно; курсор сверху — корректно).

## C1/D1 — ВЫПОЛНЕНЫ: config-схема и debug как модули

config/config.{hpp,cpp}: опции сгруппированы по доменам (grid/strip/look/
colors/blur/anim/keys/behavior/layer/debug), каждая — типизированный live-
хэндл через V2-values. Скрытые пофреймовые издержки убраны: hash-lookup со
временной строкой на каждый cfgInt и re-parse hex на каждый cfgColor —
теперь разыменование указателя + кэш распарсенного цвета (re-parse только
при смене литерала). Схема-таблица = single source of record.

СЕССИОНАЛЬНАЯ МИНА (найдена по coredump, закрыта): CIntValue/CStringValue
хранят имя как const char*; .c_str() временного std::string виснет сразу
после смерти временного → commence() абортит на мусорном имени при
регистрации → детерминированный краш сессии при загрузке плагина. Ключи в
схеме — ТОЛЬКО литералы; ловушка задокументирована у таблицы.

debug/log.{hpp,cpp}: гейт, файл /tmp/gloview.log, бутстрап-окно после
загрузки — модуль, не методы сессии.

## S1/M1/I1 — ВЫПОЛНЕНЫ: полная иерархия каталогов

session/session.cpp (lifecycle+hooks), input/{mouse,keys}.cpp,
actions/actions.cpp, build/build.cpp, model/model.hpp (Tile/StripWin/
StripItem/LabelTex/Ghost/WinPulse/Drag — чистые данные), anim/clocks.hpp
(Tween+AnimCfg), render/backdrop.hpp (BlurCache). overview.hpp — тонкий
владелец трёх хранилищ + декларации. Правило слоёв: main → домены →
примитивы → платформа; painter только читает.

## СЛЕДУЮЩАЯ СЕССИЯ — план (ветка feat/drag-anim, не влита в main)

Состояние: SwapFX-система, стили свапов, зоны от реальных слотов, слайд
воркспейсов, конфиг-группы анимаций (ws_enter/ws_out/swap_main/
swap_partner/expo_in/expo_out/drag_lift) — закоммичены. Краш грид→пустая
карточка (UAF + кросс-воркспейсный фокус) — закрыт.

### Баг 1 (головной): ghost'ы all→one / card-click невидимы
Факты из трейсов: ghost'ы СОЗДАЮТСЯ (gh=2/4, pop=1), renderGhosts вызывает
renderWindowLive КАЖДЫЙ кадр (зонд "ghost DRAW" + корректные боксы),
dmg полный — а пикселей нет. Зонд rWL (renderWindowLive логает бокс/альфу/
texOK главного узла, arm через dbgRWLOn/Off в renderGhosts) — УСТАНОВЛЕН.
Следующий шаг: один репро (открыть оверлей → тоггл), rg "rWL win" лог:
  * строк НЕТ → renderWindowLive не вызывается для узлов (breadthfirst
    пуст / ранняя защита);
  * строки есть, texOK=1, боксы валидны → пиксели теряются ПОСЛЕ вызова
    draw: проверять drawTex-клиппинг (visibleRegion поверхности: окна
    только что покинули рендер — регион может быть сброшен Hyprland'ом),
    renderModif, blend-стейт;
  * alpha=0 или бокс вырожден → математика (проверено — нет).
Гипотеза #1: visibleRegion поверхности сбрасывается, когда окно перестаёт
рендериться (suppressed) → клип-регион пуст → drawTex молчит. Фикс:
для ghost-вызовов подавать явный clipRegion (полный бокс) через
SRenderData.clipRegion / CTexPassElement, минуя visibleRegion.
После фикса: убрать TEMP-зонды (dbgLogged в Landing/SwapFX/Ghost, rWL
пробу, dbgRWLOn/Off), merge feat/drag-anim -> main.

### Баг 2: ховер-заливка плиток/карточек
Не подтверждена кодом (hover-заливки на плитках нет; кольца — stroke).
Статус: ждём скрин состояния (/tmp/hover.png) при воспроизведении.
Подозреваемые: полоса стрипа (accent 14%) сквозь прозрачное тело + обои.

### Баг 3: LMB-зоны — истинный халф-сплит
Визуал теперь = полный слот выбранного окна (честно). Истинный
халф-сплит (вставка в дерево раскладки с делением соседа) ждёт
layout-insert API — в контексте плановой переработки компоновки.

### Ловушка сессии (ЗАПОМНИТЬ)
Релоады из шелла с протухшим HYPRLAND_INSTANCE_SIGNATURE уходят в мёртвый
сокет молча: сборка "ok", а живая сессия держит старый .so — все фиксы
"не применяются". ПЕРЕД тест-раундом: sig=$(ls -t /run/user/1000/hypr |
head -1) и экспорт; после багофикса — выверка по маркеру в F-трейсе
(BUILD=) или по новому конфиг-ключу через getoption.

## Журнал сессий

- 2026-08-24: R3 закоммичен (918f979: PainterPass + сплит + чистка include,
  138→55 hyprland-include в старых TU). A1 закоммичен: реестр кривых +
  hl.plugin.gloview.curve; R4-хвост (последние stale-комментарии очереди,
  аудит конфиг-ключей — осиротевших нет, 2 deprecated no-op намеренно).

- 2026-08-23: v5 спека записана; контракты верифицированы по pinned HLsrc 0.56.2.
  Главная находка: IHyprRenderer::draw(SRenderData, CRegion) (Renderer.hpp:162) —
  публичный немедленный прогон одного surface-элемента со всей семантикой очереди
  (альфа-модификаторы, UV/small, presentFeedback, blend, async-buffers), поэтому
  immediate-лист НЕ переопределяет Hyprland. R1 реализован за флагом.
- 2026-08-23 (живой тест R1): стрип на immediate работает (LMB/RMB драги,
  видео живое). Найдено и закрыто: frost_underlay не был зарегистрирован в
  kIntCfg после отката f66b9fe (kill-switch не работал) — зарегистрирован.
- **Гейзенбаг «пропажа UI при драге с грида»**: наблюдался только в первой
  живой сессии после большого перерыва (шаги B/C+R1 взлетели разом; флаги
  переключались eval'ом на живом оверлее). Пропал начисто после чистого
  reload; в инструментированной сборке (tiles/strip/ws/liveWs/drag в F-строке)
  не воспроизводится ни разу. Билд-сторона была жива даже в сломанный период
  (F-кадры 16мс, полный damage). Гипотезы: протухшее состояние между
  eval-переключениями маршрута на живом оверлее / двойной инстанс плагина.
  Инструментация оставлена; переоткрыть при рецидиве.
- **«Одно окно темнее» + «острые углы»**: НЕ баг плагина — у пользователя два
  терминала с разными цветовыми схемами/прозрачностью (windows-2.png: обе
  плитки круглые, левый с welcome-баннером). Одиночный dim в frost-блоке
  оставлен (пиксельный паритет плитки с фоном — цель шага C; двойной dim
  делал frost-плитку объективно темнее окружения).
- Решения: frost_underlay остаётся default=1 (переработка — зона R2), юзер
  держит 0 в gloview.lua до R2; eval-переключения флагов на живом оверлее
  больше не считать достоверным A/B — только через reload.
- Открыт: лаг курсора в all-workspaces (проверить: idle-expo 5с → полный
  damage в логе? HW/SW курсор фактически активен?).
- 2026-08-23 (вечер, разрешение «гейзенбагов»): ВСЕ три симптома («пропажа
  UI при драге», «острые углы», «окно темнее») жили в одной семье —
  state-corruption после сна/config-reload, воскрешавшая frost_underlay=1
  (eval-значения не переживают перечитывание Lua-конфига). frost=0 мгновенно
  лечит углы/затемнение; драг после чистого reload жив. Двойного инстанса
  при hyprctl reload НЕТ (plugin list = 1). Дефолт frost_underlay → 0
  (артефакты в поле; переработка — зона R2; без него смысл исчезает:
  backdrop в painter рисуется первым в том же проходе).
- **«10x GPU»**: замер юзера был из сломанной итерации (протёкшие реальные
  окна → нативный blur-behind на каждом терминале поверх живых обоев).
  Idle-замер A/B (hyprctl gloview, 6с, руки убраны): 355 vs 348 кадров,
  full 36 vs 39 — конвейер идентичен. ~60fps непрерывно в idle = анимированные
  обои, не плагин. НО: во время АКТИВНОГО драга GPU ×10 — каждый mousemove →
  damage()=damageMonitor + forceFullFrames → полная рекомпозиция ~105Мп ×
  слои × 60fps. Тот же механизм = лаг курсора в expo (ховер-изменения тоже
  дамажат весь монитор). ФИКС В ПЛАН (R2-пакет): точечный damage при
  драге/ховере — union(старый бокс плитки, новый бокс, курсор) вместо
  монитора; пересмотреть forceFullFrames-чёрч при драге.
- hyprctl getoption читает V2-значения плагина — штатный способ проверки
  живых параметров.
- 2026-08-23 (ночь): R2 закоммичен (8433f55) + переработка frost (4b5fa8d).
  Гейт R2 пройден (пиксель-паритет A/B, население, драги). Найден и починен
  регресс финального кадра: teardown переехал в хвост painter'а
  (finishPendingDeactivate, Phase::Front) — queue-маршрут переживал очистку
  Model на build-времени за счёт копий данных в элементах, immediate — нет.
  Frost переписан как ЛОКАЛЬНАЯ ПЕРЕРИСОВКА финального фона (blit кэш-блюра
  @1.0 через UV sub-rect + dim @глобальной e, оба скруглённые квадры без
  scissor) — сняло семейство «dim скачет/плавает + ghost-углы + провал блюра
  10→5-6→10»: старые альфы были инвертированы (blit (1-e) давал sharp-утечку
  C·e·(1-e), dim 1.0 — двойной-dim и скачок на границе e≥0.999).
  frost_underlay → deprecated no-op. По итогам живого теста юзера: «всё
  последовательно и слитно».

Не-рендерная логика (input/keys/actions/build/core/layout) не трогается.
Структура файлов решается на R3 (возможен src/render/painter.cpp + view-файлы);
ранний сплит файлов НЕ делаем — сначала ядро, потом упаковка.

## Приёмка

- Дефолт (флаг=0) пиксель-в-пиксель и по таймингам = сегодняшний билд.
- Мастер-выключатель анимаций по-прежнему делает всё статичным.
- Ни одного m_renderPass.add из EXECUTION-контекста.
- Комментарии о внутренностях Hyprland — с файлом:строкой pinned 0.56.2.
