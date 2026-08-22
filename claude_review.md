Ran 11 commands, viewed 5 files, edited a file

Проанализировал четыре механизма баги, предложил протокол синхронизации логов.

Find buffer-age / swapchain / damage tracking members
Find CDamageRing implementation
Read DamageRing implementation

Декодировал механизм отслеживания повреждений буфера.

Find where buffer age is queried during beginRender
Read beginRender's buffer-age query and swapchain next()
Read damageMonitor implementation
Find CMonitor::addDamage
Read CMonitor::addDamage overloads

Исследовал механизмы повреждения буфера и выявил возможные race-условия в синхронизации рендера.

Check default of new_render_scheduling and how commit param is used
Check renderMonitor declaration default arg

Проследил рендеринг буферов и выявил пробел в механизме forceFullFrames.

Check ring length constant and all forceFullFrames usages
Read context around forceFullFrames=3 trigger in Monitor.cpp

Анализировал триггеры монитора и lifecycle события.

Find every write site of m_forceFullFrames across the whole source
Confirm m_forceFullFrames is a public field on CMonitor

Проанализировал четыре потенциальных причины, выявил буфер-свопчейн как главное подозрение.

confirm function name containing line 341 for citation accuracy
Add m_forceFullFrames=3 in open() to close the buffer-age/double-buffering gap, mirroring Hyprland's own idiom
overview_core.cpp
+21
-5
Generate unified diff of the proposed fix

Отличные новости про Bug B — и да, совпадение по математике покрытия было ключевым: раз kk+frost ≥ 1 держится весь close, а артефакт всё равно есть, это не «мало альфы», а именно смена контента, как вы и нашли. Хорошо, что windowBlurEligible() как точная копия защищённого shouldBlur решила это, не наступив на грабли двух прошлых попыток (я как раз читал этот комментарий про m_renderData.currentWindow — рад, что обошлось без него).

По Bug A прошёлся по приколотому источнику (0.56.2, клонировал реально, не по памяти) ещё раз, уже под refuted-solitary. Нашёл кое-что конкретное.

3) Swapchain / buffer-age — вот это, похоже, оно

CDamageRing (src/output/DamageRing.hpp/.cpp):

cpp
constexpr static int DAMAGE_RING_PREVIOUS_LEN = 3;

CRegion CDamageRing::getBufferDamage(int age) {
    CRegion damage = m_current;
    for (int i = 0; i < age - 1; ++i) {
        int j = (m_previousIdx + i) % DAMAGE_RING_PREVIOUS_LEN;
        damage.add(m_previous.at(j));
    }
    if (pixman_region32_n_rects(damage.pixman()) > 8) return damage.getExtents();
    return damage;
}

и beginRender() (Renderer.cpp:1763-1787):

cpp
m_currentBuffer = pMonitor->m_output->swapchain->next(&bufferAge);
...
damage = pMonitor->m_damage.getBufferDamage(bufferAge);
pMonitor->m_damage.rotate();   // m_current -> previous[0], m_current.clear()

Ключевое: rotate() сжирает m_current при каждом beginRender(), а не только при том, который вы считаете «нашим». Один вызов Overview::damage() (full-box в m_current) гарантированно покрывает ровно тот swapchain-image, который его первым и потребит. Для 2-3-буферного swapchain остальные образы восстанавливаются только через 3-глубокий ring — и это ровно совпадает с «1-3 кадра» из исходного репорта (DAMAGE_RING_PREVIOUS_LEN=3). Один преview-тайл в live-режиме продолжает коммитить и дамажить сам себя (hkDamageSurface явно пропускает live-tile damage) — отсюда caret-sized footprint именно вокруг одного окна, остальное — контент того самого buffer image, который в этот заход просто не попал под полную заливку.

Проверил: у Hyprland уже ЕСТЬ штатный паттерн ровно под этот класс бага — m_forceFullFrames (публичное поле, Monitor.hpp:93, public: с 63-й строки). Собрал все места записи по всему дереву:

Monitor.cpp:341          m_forceFullFrames = 3;  // "force 3 full frames to make sure there is no blinking due to double-buffering" — CMonitor::onConnect
PropRefresher.cpp        m_forceFullFrames = 2;  // x3, на изменение рендер-влияющих пропов
ConfigManager.cpp:1060   m_forceFullFrames = 2;  // полный конфиг-reload
ConfigManager.cpp:1064   mirror->m_forceFullFrames = 3;
CursorManager.cpp:320    m_forceFullFrames = 5;  // смена курсора/темы

Комментарий на 341-й строке — дословно то, что мы наблюдаем. Это ЕДИНСТВЕННЫЙ штатный механизм в кодовой базе под «сейчас буфер-эйдж трекингу верить нельзя, продавите N кадров». Ни один плагинный хук туда не попадает — это чисто наш пробел, не Hyprland-side.

Предлагаю и уже применил в рабочей копии (только к open(), ничего не трогает у close()/hardClose() — по ТЗ это только Bug A):

diff
   m->m_blurFBDirty = true;
 
+  // Force 3 consecutive full-monitor-damage frames, the same public knob
+  // Hyprland itself sets on every other "swapchain buffer-age tracking can no
+  // longer be trusted" transition (CMonitor::onConnect, prop-refresh/config
+  // reload, cursor-theme change — grep m_forceFullFrames across Hyprland src).
+  // ...
+  m->m_forceFullFrames = 3;
+
   // Clear drag/press state: ...

Дёшево, безопасно (публичное поле, тот же паттерн что уже используется для m_blurFBDirty парой строк выше), одноразовое (не перевзводится, пока оверлей активен), и если гипотеза неверна — просто пара лишних полных перерисовок при открытии, поведенчески незаметно. Это первое, что стоит прогнать в следующей сессии — дешевле любого другого пункта.

1) DS-leave — подтверждённый пробел, но, похоже, не ваш кейс

handleDSleave() (Monitor.cpp:2235) сбрасывает m_lastScanout/m_directScanoutIsActive/DRM-формат и ставит m_blurFBDirty=true — но не трогает m_forceFullFrames. Это asymmetм относительно остальных четырёх мест выше: ровно такой же класс «буферы могли протухнуть», а сети нет. Реальный гэп в Hyprland, стоит завести апстрим-репорт независимо от вашего кейса.

НО: и m_solitaryClient, и m_directScanoutIsActive/canAttemptDirectScanoutFast() строго завязаны на Fullscreen::controller()->getFullscreenWindow() (FSMODE_FULLSCREEN, FullscreenController.cpp:228) — без фуллскрин-окна DS в принципе никогда не включается. Раз воспроизводится с нулём фуллскринов — этот путь для вашего repro закрыт. Единственное, что стоит домерить: проверьте dso/soli из PRE-probe (не только overlay-кадров) во всех 6 сессиях — если там тоже 0 на всём окне ±300мс, можно вычеркнуть пункт 1 окончательно.

2) needsFrame/hasChanged() vs 8ms-помпа

В дефолтном (не-new_render_scheduling) пути onFrame() рендерит синхронно и однопоточно: open() — синхронный вызов из keybind-диспетчера, полностью завершается (включая damage()) до того, как event loop вообще может отдать управление кадровому колбэку output'а. Гонки тут логически не вижу — m_current физически не может быть прочитан/провёрнут между вашим damage() и следующим реальным рендером, потому что между ними физически нет промежуточного beginRender(), если только что-то ещё синхронно не рендерит в этом же тике (маловероятно и не специфично для тоггла). Низкий приоритет, если вы не гоняете что-то нестандартное здесь.

4) commitPendingAndDoExplicitSync

Проверил: весь onSyncFired/m_pendingThird/deferred-commit путь целиком под newSchedulingEnabled(), который требует render:new_render_scheduling (дефолт false, ConfigValues.cpp:571) + explicit-sync + !m_directScanoutIsActive. Если у вас этот конфиг не включён явно — этот путь физически не участвует, можно вычеркнуть. Стоит подтвердить одной строкой (hyprctl getoption render:new_render_scheduling), но приоритет ниже пункта 3.

Протокол корреляции для следующего захвата

Самое надёжное — не визуальный маркер (человеческая реакция/муxing jitter съедят точность), а привязка по абсолютному wall-clock, а не по steady_clock:

В dbg()/трейсере рядом с существующим steady_clock-штампом добавить std::chrono::system_clock::now() в epoch-миллисекундах на каждой F-строке (и на PRE-probe тоже) — если хотите, могу сразу накидать патч, это тривиально и ничего не меняет в поведении, только формат лога.
Запуск записи с фиксацией эпохи старта:
   date +%s.%N > /tmp/rec_start.txt && wf-recorder -f /tmp/blink.mp4
После записи:
   ffprobe -v error -select_streams v -show_entries frame=pts_time -of csv=p=0 /tmp/blink.mp4

даёт PTS каждого кадра относительно rec_start; abs_time = rec_start_epoch + pts_time.
4. Сопоставляете abs_time каждого PNG-кадра с epoch-штампами F-строк в /tmp/gloview.log напрямую, без всякого «на глаз посчитать от маркера».

Погрешность — только jitter старта процесса записи (обычно единицы-десятки мс), что на порядок меньше межкадрового интервала при 60Гц (~16.7мс) и уж тем более длительности анимации (360мс по умолчанию) — этого достаточно, чтобы однозначно пиновать конкретный кадр. Визуальный маркер (типа моргания на весь экран специальным цветом ровно в момент вызова damage() в open()) можно оставить как избыточную перекрёстную проверку, но, думаю, не обязателен при epoch-привязке.

Готов сразу накидать патч на epoch-логирование, если он нужен для следующего захвата — просто скажите.

фикси, подгоняй патч, отдам агенту на мердж

Ran a command

Только один сфокусированный диф, ничего лишнего не наросло. Собираю в чистый git-applyable патч с правильными путями (src/..., как в вашем CMakeLists.txt).

Ran 5 commands, read a file · 1 note

Патч готов, проверен git apply --check и git apply на чистую копию (src/overview_core.cpp из вашего аплоада) — накатывается без фаззов, hunk сидит ровно на void Overview::open().

Сообщение коммита (для агента, в вашем стиле type(scope): summary):

fix(render): force 3 full-damage frames on overview open

Hyprland's own swapchain buffer-age tracking (CDamageRing, 3-deep ring)
only reconstructs a buffer image's missing history if it's reused within
DAMAGE_RING_PREVIOUS_LEN=3 renders of the last full damage. A single
Overview::damage() call in open() guarantees a correct repaint for
whichever ONE swapchain image consumes it before rotate() clears
m_current; the other image(s) in a 2-3 buffer swapchain can still show
a stale (worst case: never-composited-into) buffer for up to 3 frames -
matching the reported "1-3 frame black flash" exactly.

m_forceFullFrames is the same public knob Hyprland itself sets on every
other "buffer-age tracking can no longer be trusted" transition
(CMonitor::onConnect: 3, config/prop reload: 2, cursor theme change: 5 -
see m_forceFullFrames across src/). This is the missing counterpart for
gloview's own open() transition. One-shot, not re-armed while active;
harmless if wrong (a few extra full-frame paints at open, nothing else).

Что стоит приложить рядом, раз это уходит агенту:

В REFACTORING.md/CANDIDATES.md у вас (мне их не давали, поэтому не могу апдейтить сам) — по вашему же чеклисту это отдельный пункт лога сессии. Текст для записи:

Bug A: solitary-client refuted by trace (soli=0/dso=0, zero-fullscreen repro). Root cause: CDamageRing (3-deep ring) not fully re-primed by open()'s single damage() call across a 2-3 image swapchain. Fix: m->m_forceFullFrames=3 in open() (7151c89-adjacent). Needs frame-capture confirmation next session.

Билд и хот-релоад я тут не гонял — в песочнице нет заголовков Hyprland/линковки, только исходники под чтение. Синтаксически чисто (баланс скобок сверил построчно, дельта та же, что и в оригинале), но cmake --build build --target reload и реальный тоггл — на вашей стороне перед мерджем, как и весь остальной чек-лист AGENTS.md.
Это не отменяет протокол корреляции (epoch + ffprobe) из прошлого сообщения — патч однострочный и дешёвый, но подтверждение по кадрам всё равно нужно, чтобы закрыть Bug A окончательно, а не просто "вроде не мигает".

Инструментацию с epoch-таймстампами в лог не трогал — вы её не подтвердили явно, а мешать её в тот же патч не стал (один логический шаг = один коммит, по вашему же AGENTS.md). Если нужна для следующего захвата — соберу отдельным патчем по первому слову.
