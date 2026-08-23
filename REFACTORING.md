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

## Журнал сессий

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

Не-рендерная логика (input/keys/actions/build/core/layout) не трогается.
Структура файлов решается на R3 (возможен src/render/painter.cpp + view-файлы);
ранний сплит файлов НЕ делаем — сначала ядро, потом упаковка.

## Приёмка

- Дефолт (флаг=0) пиксель-в-пиксель и по таймингам = сегодняшний билд.
- Мастер-выключатель анимаций по-прежнему делает всё статичным.
- Ни одного m_renderPass.add из EXECUTION-контекста.
- Комментарии о внутренностях Hyprland — с файлом:строкой pinned 0.56.2.
