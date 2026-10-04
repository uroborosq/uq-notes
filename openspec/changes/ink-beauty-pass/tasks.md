# ink-beauty-pass: план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** линия пера без пятен и пунктира на любом масштабе, толщина без
запаздывания с заострением новых штрихов, круглые концы и точки от касания,
честный замер задержки до показа (`proposal.md`, `design.md`, `specs/`).

**Architecture:** пять шагов, каждый выпускается отдельной версией и
проверяется на офлайн-стенде `tools/stroke-lab` и тестах QtTest. Шаг 0
переносит стенд и меряет задержку; шаг 1 чинит давление и вводит закон
толщины в штрихе (схема `.uqn` 3); шаг 2 делает круглые торцы и точки;
шаг 3 заменяет `QSGFlatColorMaterial` своим `InkMaterial` с точным покрытием
кромки; шаг 4 выключает MSAA.

**Tech Stack:** C++20, Qt 6.11 (Quick scene graph, RHI, ShaderTools/qsb,
Sql, Test), ink-stroke-modeler, Wayland `wp_presentation`, CMake/Ninja.

---

## Перед началом

- Сборке нужны пакеты из `README.md` и для шага 3 `qt6-shadertools`. В среде,
  где составлялся план, не было `qpdf` (`libqpdf`), поэтому приложение и тесты
  холста, экспорта, графа сцены там не собирались: `sudo pacman -S qpdf`
  до шага 1.
- Сборка и тесты из корня репозитория:

  ```sh
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUQ_BUILD_STROKE_LAB=ON
  cmake --build build -j$(nproc)
  ctest --test-dir build --output-on-failure
  ```

  `-DUQ_BUILD_STROKE_LAB=ON` появляется в шаге 0; до него опцию не передавать.
- Работа идёт в ветке `ink-beauty-pass`. Прототипы, из которых переносится
  код, лежат в локальных ветках `exp/*` и читаются через `git show` и
  `git diff`; в рабочее дерево они не выкладываются.
- Ожидаемые числа стенда и результаты прогонов в шагах сняты при составлении
  плана на копиях кода. Что проверено, а что нет, сказано в начале каждого
  шага. Если число прогона расходится с планом больше указанного допуска, это
  повод остановиться и разобраться, а не подгонять критерий.
- Гейт, не пройденный при исполнении, останавливает шаг: результат
  записывается в `design.md`, решение принимает пользователь.
- Сообщения коммитов - по-русски; финальный коммит шага - заголовок «<что>,
  версия x.y.z» и абзацы прозой, как `git show 7bc0af2`. Каждый коммит
  заканчивается строкой `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

| Шаг | Группы | Решения | Версия |
|---|---|---|---|
| 0 | 1-2 | D1: стенд, задержка до показа | 0.7.0 (без смены) |
| 1 | 3-5 | D2, D3: давление, закон толщины | 0.8.0 |
| 2 | 6-9 | D4, D5: торцы, точки; гейт D6 только повторяется | 0.9.0 |
| 3 | 10-14 | D7: InkMaterial, точные стыки, контур экспорта | 0.10.0 |
| 4 | 15-16 | D8: MSAA выключен | 0.11.0 |


# Шаг 0: стенд и задержка до показа

> **Шаг 0 (design.md D1, D9).** Стенд stroke-lab в основном дереве с метриками
> всех прототипов и честный замер задержки «метка события → показ». Версия
> остаётся 0.7.0. Всё ниже проверено на копии деревьев в песочнице: рецепт
> слияния воспроизведён с нуля, итоговые файлы совпали побайтно (контрольные
> суммы в 1.16), baseline на собранном стенде дал числа design.md, код группы 2
> собран и `tst_framestats` прошёл (сам `uqnotes_core` в песочнице не собран:
> на машине нет `libqpdf`, см. «Чего раздел не покрывает»).
>
> Блоки кода стоят с первой колонки, вне отступа пунктов списка: их можно
> копировать в оболочку как есть (heredoc-терминаторы `EOF` и отступы Python
> не сдвинуты). Команды групп 1 и 2 выполняются из корня репозитория в одной
> оболочке: переменная `SAMPLES` из 1.4 и функция `labpatch` из 1.6 нужны
> дальше.

## 1. Стенд stroke-lab в репозитории

**Files:**
- Create (из `exp/stroke-lab` 720b497): `tools/stroke-lab/{CMakeLists.txt,README.md,corpus.cpp,corpus.h,lab.h,main.cpp,metrics.cpp,metrics.h,raster.cpp,raster.h,render.cpp,render.h,variants/baseline.cpp}`
- Create (из веток прототипов): `tools/stroke-lab/endcheck.{h,cpp}` (`exp/apps-survey`), `tools/stroke-lab/aabench.cpp` (`exp/dry-refine`), `tools/stroke-lab/aaraster.{h,cpp}`, `tools/stroke-lab/rastermetrics.{h,cpp}` (`exp/antialias`)
- Modify: `CMakeLists.txt` (опция `UQ_BUILD_STROKE_LAB`), `tools/stroke-lab/{main.cpp,metrics.cpp,metrics.h,corpus.cpp,corpus.h,render.cpp,raster.cpp,raster.h,CMakeLists.txt,README.md}`
- Test: прогон `stroke-lab --variants baseline` против чисел design.md (гейт D9, шаг 0)

Что выяснено до плана и на чём держится рецепт:

- `exp/stroke-lab` = один коммит 720b497 поверх 7bc0af2 (= `master`), а
  `ink-beauty-pass` = `master` + только `openspec/changes/ink-beauty-pass/**`.
  `git diff master exp/stroke-lab --stat`: корневой `CMakeLists.txt` (+7: опция
  и `add_subdirectory`) и 13 файлов `tools/stroke-lab`. Пути не пересекаются,
  поэтому `git cherry-pick --no-commit 720b497` проходит без конфликтов. Он
  надёжнее `git checkout exp/stroke-lab -- tools/stroke-lab`: checkout не
  принёс бы опцию в корневой CMake, а cherry-pick при неожиданном расхождении
  остановится с конфликтом, а не молча перезапишет файл. `--no-commit` - чтобы
  коммит получил наше сообщение со строкой Co-Authored-By.
- Все ветки прототипов ответвлены от 720b497, их правки инфраструктуры стенда
  накладываются диффами `git diff exp/stroke-lab exp/<ветка>` в фиксированном
  порядке; конфликты возникают только в `main.cpp` и `metrics.cpp` (списки
  столбцов, ключи командной строки, блоки `m[...]`), все разрешения даны ниже
  текстом.
- `exp/dry-refine` и `exp/rnote` оба пишут ключи `width_start_ratio` и
  `width_end_ratio` с разным смыслом. Числа design.md (конец 0.95 → 0.70,
  эталон 0.68) - это смысл `exp/rnote` (толщина последних ~2 px к медиане
  штриха), поэтому имена остаются за ним, а метрики `exp/dry-refine`
  переименовываются в `width_touch_ratio` / `width_lift_ratio`.
- Не переносится: `variants/*` кроме `baseline.cpp` (их приносят шаги 1-3);
  `sampler-test/` и цель `stroke-lab-sampler-test` из `exp/apps-survey` (это
  подмена `StrokeSampler` сэмплером `apps_survey_core.h`, без варианта она не
  собирается); `gpu/` и цель `aa-gpu-bench` из `exp/antialias` (рисует полосой
  варианта `aa-fringe` через `variants/aa_fringe.h` и ищет вариант
  `"aa-fringe"` в реестре, поэтому переносится в шаге 3 вместе с ним и служит
  гейтам шагов 3 и 4). `stroke-lab-aabench` из `exp/dry-refine` от вариантов не
  зависит и переносится сейчас.
- Корпус после слияния - 29 сэмплов (`dots_tap` из `exp/outline`, `dots_taps`
  из `exp/google-ink`). Зерно у каждого сэмпла своё, остальные 27 не меняются
  ни на бит, но средние по корпусу сдвигаются, поэтому числа design.md
  сверяются по прежнему списку из 27 сэмплов.

- [x] 1.1 **Проверить исходное состояние.** Рабочее дерево чистое (кроме неотслеживаемого `.claude/worktrees/`), ветка `ink-beauty-pass`, ветки прототипов на месте.
```sh
cd /home/dmitriy.tikhomirov/desktop/owned/uq-notes
git status --short                       # ожидание: только "?? .claude/worktrees/"
git branch --show-current                # ожидание: ink-beauty-pass
git rev-parse --short exp/stroke-lab     # ожидание: 720b497
git merge-base HEAD exp/stroke-lab | cut -c1-7   # ожидание: 7bc0af2
git diff --stat master HEAD | tail -1    # ожидание: 4 files changed, 610 insertions(+) - только openspec
for b in rnote outline google-ink trajectory apps-survey latency dry-refine antialias; do \
  printf '%-12s %s\n' $b "$(git merge-base exp/stroke-lab exp/$b | cut -c1-7)"; done
# ожидание: у всех 720b497
pkg-config --exists libqpdf && echo qpdf-ok   # нужен для сборки из корня; без него см. 1.3
```

- [x] 1.2 **Перенести стенд cherry-pick'ом без коммита.**
```sh
git cherry-pick --no-commit 720b497
git diff --cached --stat | tail -1   # ожидание: 14 files changed, 2957 insertions(+)
```
  Затем в корневом `CMakeLists.txt` уточнить комментарий к опции (в пакет не входит). Заменить блок, пришедший из 720b497:
```cmake
# Офлайн-стенд сравнения конвейеров чернил (tools/stroke-lab). Обычно
# собирается отдельно: cmake -S tools/stroke-lab -B build-lab.
option(UQ_BUILD_STROKE_LAB "Build the offline stroke lab" OFF)
```
  на:
```cmake
# Офлайн-стенд сравнения конвейеров чернил (tools/stroke-lab): метрики, по
# которым принимаются решения ink-beauty-pass. В пакет не входит, поэтому по
# умолчанию выключен; можно собрать и отдельно: cmake -S tools/stroke-lab.
option(UQ_BUILD_STROKE_LAB "Build the offline stroke lab" OFF)
```
  Блок `if(UQ_BUILD_STROKE_LAB) add_subdirectory(tools/stroke-lab) endif()` после `add_subdirectory(src)` остаётся как пришёл. `git add CMakeLists.txt`.

- [x] 1.3 **Собрать стенд из основного дерева.**
```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUQ_BUILD_STROKE_LAB=ON
cmake --build build -j$(nproc) --target stroke-lab
ls build/tools/stroke-lab/stroke-lab    # ожидание: файл есть
```
  Ожидание: конфигурация и сборка без ошибок. Если `cmake` падает на `pkg_check_modules(QPDF ...)` (на машине нет `libqpdf`), это не стенд: поставить `qpdf` (`sudo pacman -S qpdf`). Пока его нет, стенд проверяется отдельной сборкой `cmake -S tools/stroke-lab -B build-lab -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-lab`, а во всех командах ниже `build/tools/stroke-lab/` заменяется на `build-lab/`.

- [x] 1.4 **Гейт: baseline 720b497 воспроизводит числа design.md.**
```sh
SAMPLES=ref_line,ref_loop,ref_zigzag,ref_spiral,ref_letter_e,ref_fast_circle,ref_corner_v,ref_corner_n,ref_cardiogram,word_minimum,word_hello,word_privet,digits_2025,word_minimum_small,small_o_8,small_o_12,small_o_20,small_a_12,small_e_10,corner_N,corner_M,corner_check,fast_eeee,fast_scribble,slow_line_tremor,slow_arc_tremor,slow_hello
rm -rf /tmp/lab-720b497
./build/tools/stroke-lab/stroke-lab --variants baseline --no-png --samples $SAMPLES --out /tmp/lab-720b497 >/dev/null
python3 - /tmp/lab-720b497/summary.json <<'EOF'
import json, sys
s = json.load(open(sys.argv[1]))['baseline']['s1']
want = {
    'centerline_dev_mean_px@mean': (0.1068, 5e-5),
    'corner_dist_mean_px@mean': (0.5618, 5e-5),
    'corner_dist_max_px@max': (1.413, 1e-9),
    'facet_count_2deg@sum': (2818, 0),
    'curvature_tv_excess_per100px@mean': (3.252, 5e-4),
    'settled_mutations@sum': (0, 0),
}
bad = [f'{k}={s.get(k)}' for k, (v, tol) in want.items() if s.get(k) is None or abs(s[k] - v) > tol]
print('cpu us/event', round(s['cpu_us_per_event_mean@mean'], 2))
print('FAIL ' + ', '.join(bad) if bad else 'OK')
EOF
```
  Ожидание: `cpu us/event` около 2.2 (зависит от машины; в песочнице 2.19) и `OK`. `/tmp/lab-720b497/summary.json` не удалять: это эталон для 1.16. При `FAIL` дальше не идти: стенд или `src/ink` расходятся с 720b497 (проверить `git diff 720b497 -- tools/stroke-lab src/ink`, ожидание - пусто).

- [x] 1.5 **Промежуточный коммит.**
```sh
git commit -F - <<'EOF'
Стенд stroke-lab в основном дереве

Офлайн-стенд из exp/stroke-lab (720b497) переезжает в tools/stroke-lab.
Собирается с -DUQ_BUILD_STROKE_LAB=ON, по умолчанию выключен и в пакет не
входит. Baseline воспроизводит числа design.md: отклонение осевой 0.1068 px,
вершина острия до 1.413 px, 2818 граней, ни одной изменённой окончательной
точки.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

- [x] 1.6 **Завести функцию наложения правок ветки.** В той же оболочке (zsh или bash), из корня репозитория:
```sh
labpatch() {
  git diff exp/stroke-lab "exp/$1" -- tools/stroke-lab \
    ':!tools/stroke-lab/variants' ':!tools/stroke-lab/sampler-test' ':!tools/stroke-lab/gpu' \
    ':!tools/stroke-lab/CMakeLists.txt' ':!tools/stroke-lab/README.md' \
    | git apply -3
}
```
  `CMakeLists.txt` и `README.md` стенда исключены: они пишутся целиком в 1.15 и 1.17. После каждого наложения - `git add tools/stroke-lab`: `git apply -3` отказывается работать с файлом, у которого есть неиндексированные правки. Сообщения git могут быть локализованы; важно, «чисто» или «с конфликтами».

- [x] 1.7 **`exp/rnote`: профиль толщины и `--min-width-factor`.**
```sh
labpatch rnote && git add tools/stroke-lab
```
  Ожидание: `main.cpp`, `metrics.cpp` наложены чисто.

- [x] 1.8 **`exp/outline`: `raster_vs_ideal/truth`, `width_cv`, сэмпл `dots_tap`.**
```sh
labpatch outline && git add tools/stroke-lab
```
  Ожидание: `corpus.cpp`, `main.cpp` чисто.

- [x] 1.9 **`exp/google-ink`: сэмпл `dots_taps`.**
```sh
labpatch google-ink && git add tools/stroke-lab
```
  Ожидание: `corpus.cpp` чисто.

- [x] 1.10 **`exp/trajectory`: `--noise`, `--rate`, `--libinput-smoothing`.** (Ветка в задании не названа, но это инфраструктура корпуса, без вариантов: нужна, чтобы проверить на стенде утверждение D6 про перо с меньшим шумом.)
```sh
labpatch trajectory
```
  Ожидание: `corpus.cpp`, `corpus.h` чисто, `main.cpp` с одним конфликтом - блок объявлений ключей. Весь блок от `<<<<<<< ours` до `>>>>>>> theirs` включительно заменить на:
```cpp
    QCommandLineOption minWidthOpt(QStringLiteral("min-width-factor"),
                                   QStringLiteral("Нижний предел эталонного закона толщины w·(f + (1−f)·p)"),
                                   QStringLiteral("f"), QStringLiteral("0.25"));
    QCommandLineOption noiseOpt(QStringLiteral("noise"),
                                QStringLiteral("Шум датчика σ, px (по умолчанию 0.05)"),
                                QStringLiteral("px"), QStringLiteral("0.05"));
    QCommandLineOption rateOpt(QStringLiteral("rate"), QStringLiteral("Частота пера, Гц (481)"),
                               QStringLiteral("hz"), QStringLiteral("481"));
    QCommandLineOption libinputOpt(QStringLiteral("libinput-smoothing"),
                                   QStringLiteral("Эмулировать среднее 4 отсчётов libinput"));
    QCommandLineOption noPngOpt(QStringLiteral("no-png"), QStringLiteral("Только метрики"));
    QCommandLineOption listOpt(QStringLiteral("list"), QStringLiteral("Список вариантов и сэмплов"));
    parser.addOptions({variantsOpt, outOpt, samplesOpt, scalesOpt, widthOpt, dprOpt, repsOpt,
                       minWidthOpt, noiseOpt, rateOpt, libinputOpt, noPngOpt, listOpt});
```
  Затем `git add tools/stroke-lab`.

- [x] 1.11 **`exp/apps-survey`: `--endcheck`, ошибки толщины по зонам, толщина в `settled_mutations`.**
```sh
labpatch apps-survey
```
  Ожидание: созданы `endcheck.cpp`, `endcheck.h`; конфликты - два в `main.cpp`, один в `metrics.cpp`. Разрешение по порядку блоков в файле.
  `main.cpp`, блок 1 (в `kColumns`, после `{"width_mean_ratio", ...}`) заменить на (подпись `"w cv"` у `width_body_cv` меняется на `"w body cv"`: `"w cv"` ниже займёт `width_cv` из `exp/outline`):
```cpp
    {"width_track_rms_rel", Column::Mean, "w track"},
    {"width_end_ratio", Column::Mean, "w end"},
    {"width_rise_px", Column::Mean, "w rise px"},
    {"width_start_err_px", Column::Mean, "w start err"},
    {"width_end_err_px", Column::Mean, "w end err"},
    {"width_body_err_px", Column::Mean, "w body err"},
    {"width_step_max_px", Column::Max, "w step max"},
    {"width_body_cv", Column::Mean, "w body cv"},
```
  `main.cpp`, блок 2 (хвост `parser.addOptions`) заменить на одну строку:
```cpp
                       minWidthOpt, noiseOpt, rateOpt, libinputOpt, noPngOpt, listOpt, endOpt});
```
  `metrics.cpp`, блок 1 (после `m["width_mean_ratio"] = ...`) - обе стороны подряд:
```cpp
    m["width_rise_px"] = r(riseV.mean(), 2);
    m["width_rise_truth_px"] = r(riseGt.mean(), 2);
    m["width_fall_px"] = r(fallV.mean(), 2);
    m["width_fall_truth_px"] = r(fallGt.mean(), 2);
    m["width_start_ratio"] = r(startRatioV.mean());
    m["width_start_ratio_truth"] = r(startRatioGt.mean());
    m["width_end_ratio"] = r(endRatioV.mean());
    m["width_end_ratio_truth"] = r(endRatioGt.mean());
    m["width_track_rms_px"] = r(trackAbs.mean(), 4);
    m["width_track_rms_rel"] = r(trackRel.mean(), 4);
    m["width_start_err_px"] = r(widthStartErr.mean(), 4);
    m["width_end_err_px"] = r(widthEndErr.mean(), 4);
    m["width_body_err_px"] = r(widthBodyErr.mean(), 4);
    m["width_step_max_px"] = r(widthStepMax, 4);
    m["width_step_max_truth_px"] = r(widthStepMaxGt, 4);
    m["width_body_cv"] = r(widthCv.mean(), 4);
    m["width_body_cv_truth"] = r(widthCvGt.mean(), 4);
```
  Затем `git add tools/stroke-lab`.

- [x] 1.12 **`exp/latency`: эмуляция кадров и лента мокрых кадров.**
```sh
labpatch latency && git add tools/stroke-lab
```
  Ожидание: `main.cpp`, `metrics.cpp`, `metrics.h`, `render.cpp` чисто.

- [x] 1.13 **`exp/dry-refine`: ошибка толщины, сдвиг давления, `stroke-lab-aabench`.**
```sh
labpatch dry-refine
```
  Ожидание: создан `aabench.cpp`; в `metrics.cpp` два конфликта.
  Блок 1 (объявление `Stats` в начале `computeMetrics`) заменить на:
```cpp
        widthHf, widthErr, widthErrRel, widthTouch, widthLift, widthEndJump, widthEndJumpGt,
        pressureLag, pressureErr;
    // Профиль толщины у концов (apps-survey): ошибка толщины относительно
    // эталона в той же точке эталона (проекция), отдельно в первых и последних
    // kEndZone px дуги эталона и в теле; наибольший скачок толщины на 1 px
    // дуги; коэффициент вариации толщины в теле.
    Stats widthStartErr, widthEndErr, widthBodyErr, widthCv, widthCvGt;
    double widthStepMax = 0.0, widthStepMaxGt = 0.0;
```
  Блок 2 (запись ключей после `m["width_mean_ratio"] = ...`) заменить на:
```cpp
    m["width_rise_px"] = r(riseV.mean(), 2);
    m["width_rise_truth_px"] = r(riseGt.mean(), 2);
    m["width_fall_px"] = r(fallV.mean(), 2);
    m["width_fall_truth_px"] = r(fallGt.mean(), 2);
    m["width_start_ratio"] = r(startRatioV.mean());
    m["width_start_ratio_truth"] = r(startRatioGt.mean());
    m["width_end_ratio"] = r(endRatioV.mean());
    m["width_end_ratio_truth"] = r(endRatioGt.mean());
    m["width_track_rms_px"] = r(trackAbs.mean(), 4);
    m["width_track_rms_rel"] = r(trackRel.mean(), 4);
    m["width_start_err_px"] = r(widthStartErr.mean(), 4);
    m["width_end_err_px"] = r(widthEndErr.mean(), 4);
    m["width_body_err_px"] = r(widthBodyErr.mean(), 4);
    m["width_step_max_px"] = r(widthStepMax, 4);
    m["width_step_max_truth_px"] = r(widthStepMaxGt, 4);
    m["width_body_cv"] = r(widthCv.mean(), 4);
    m["width_body_cv_truth"] = r(widthCvGt.mean(), 4);
    m["width_err_mean_px"] = r(widthErr.mean(), 4);
    m["width_err_rel_mean_pct"] = r(widthErrRel.mean() * 100.0, 2);
    m["width_touch_ratio"] = r(widthTouch.mean());
    m["width_lift_ratio"] = r(widthLift.mean());
    m["width_end_jump_max_pct"] = r(widthEndJump.max() * 100.0, 2);
    m["width_end_jump_truth_max_pct"] = r(widthEndJumpGt.max() * 100.0, 2);
    m["pressure_lag_ms"] = r(pressureLag.mean(), 2);
    m["pressure_err_mean"] = r(pressureErr.mean(), 4);
```
  Затем переименовать два накопителя в теле блока «6б» и убедиться, что старых имён не осталось:
```sh
sed -i 's/^                widthStart\.add(centerWidth\.front()/                widthTouch.add(centerWidth.front()/; s/^                widthEnd\.add(centerWidth\.back()/                widthLift.add(centerWidth.back()/' tools/stroke-lab/metrics.cpp
grep -nE '\bwidth(Start|End)\b' tools/stroke-lab/metrics.cpp   # ожидание: пусто
git add tools/stroke-lab
```

- [x] 1.14 **`exp/antialias`: метрики растеризации и `lattice64`.**
```sh
labpatch antialias
```
  Ожидание: созданы `aaraster.{h,cpp}`, `rastermetrics.{h,cpp}`, `raster.{h,cpp}` наложены чисто; в `main.cpp` три конфликта.
  Блок 1 (конец `kColumns`) заменить на (повтор `{"width_mean_ratio", ...}` со стороны ours выброшен - он уже есть выше):
```cpp
    {"raster_vs_ideal_rms", Column::Mean, "rast rms"},
    {"raster_vs_ideal_p99", Column::Mean, "rast p99"},
    {"raster_vs_ideal_ink_ratio", Column::Mean, "ink/ideal"},
    {"raster_vs_truth_rms", Column::Mean, "rast-truth rms"},
    {"raster_vs_truth_ink_ratio", Column::Mean, "ink/truth"},
    {"width_cv", Column::Mean, "w cv"},
    {"width_p5_physpx", Column::Mean, "w p5 px"},
    // Растеризация (rastermetrics.h): ошибка покрытия ×255.
    {"aa_rmse_shape", Column::Mean, "AA rmse"},
    {"aa_p99_shape", Column::Mean, "AA p99"},
    {"aa_rmse_ideal", Column::Mean, "AA vs ideal"},
    {"aa_rmse_truth", Column::Mean, "px vs truth"},
    {"aa_flicker", Column::Mean, "flicker"},
    {"aa_mass_breath_pct", Column::Mean, "mass breath%"},
    {"aa_mass_ratio", Column::Mean, "mass/ideal"},
    {"aa_mass_cv", Column::Mean, "mass CV"},
    {"marker_dark_frac", Column::Mean, "marker dark"},
```
  Блок 2 (ключи `endcheck` против `no-raster`/`shifts`) заменить на:
```cpp
    QCommandLineOption endOpt(QStringLiteral("endcheck"),
                              QStringLiteral("Синтетика концов штриха (касание, отрыв, тап) и выход"));
    QCommandLineOption noRasterOpt(QStringLiteral("no-raster"),
                                   QStringLiteral("Без метрик растеризации (rastermetrics.h)"));
    QCommandLineOption shiftsOpt(QStringLiteral("shifts"),
                                 QStringLiteral("Субпиксельных сдвигов для aa_flicker"),
                                 QStringLiteral("n"), QStringLiteral("8"));
    parser.addOptions({variantsOpt, outOpt, samplesOpt, scalesOpt, widthOpt, dprOpt, repsOpt,
                       minWidthOpt, noiseOpt, rateOpt, libinputOpt, noPngOpt, listOpt, endOpt,
                       noRasterOpt, shiftsOpt});
```
  Блок 3 (после `render.metrics.width = width;`) заменить на:
```cpp
    render.metrics.minWidthFactor = parser.value(minWidthOpt).toDouble();
    const bool raster = !parser.isSet(noRasterOpt);
    RasterMetricsOptions rasterOptions;
    rasterOptions.shifts = std::max(1, parser.value(shiftsOpt).toInt());
```
  Затем:
```sh
git add tools/stroke-lab
grep -rnE '^(<<<<<<<|=======|>>>>>>>)' tools/stroke-lab   # ожидание: пусто
git diff --cached --check                                 # ожидание: пусто
```

- [x] 1.15 **Записать `tools/stroke-lab/CMakeLists.txt` целиком.** Новые единицы перевода в `stroke-lab`, отдельная цель `stroke-lab-aabench`, никакого `install()`:
```cmake
# stroke-lab - офлайн-стенд сравнения конвейеров рукописных чернил.
#
# Собирается отдельно от приложения (быстро, без QML/PDF):
#   cmake -S tools/stroke-lab -B build-lab -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-lab
# или вместе с приложением: -DUQ_BUILD_STROKE_LAB=ON в корневом CMake. В пакет
# не входит: install() здесь нет.
cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(stroke-lab LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui)

set(UQ_SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
if(NOT TARGET ink_stroke_modeler)
    add_subdirectory(${UQ_SOURCE_ROOT}/third_party ${CMAKE_CURRENT_BINARY_DIR}/third_party)
endif()

# Исходники чернил приложения - те же файлы, без копий.
add_library(stroke_lab_ink STATIC
    ${UQ_SOURCE_ROOT}/src/ink/oneeurofilter.cpp
    ${UQ_SOURCE_ROOT}/src/ink/strokesampler.cpp
    ${UQ_SOURCE_ROOT}/src/ink/stroketessellator.cpp
    ${UQ_SOURCE_ROOT}/src/ink/wetstroke.cpp
)
target_include_directories(stroke_lab_ink PUBLIC ${UQ_SOURCE_ROOT}/src ${UQ_SOURCE_ROOT}/src/ink)
target_link_libraries(stroke_lab_ink PUBLIC Qt6::Core Qt6::Gui ink_stroke_modeler)
set_target_properties(stroke_lab_ink PROPERTIES AUTOMOC OFF)

# Вариант - один .cpp в variants/: подхватывается сам.
file(GLOB STROKE_LAB_VARIANTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/variants/*.cpp)

add_executable(stroke-lab
    main.cpp
    corpus.cpp
    endcheck.cpp
    metrics.cpp
    raster.cpp
    aaraster.cpp
    rastermetrics.cpp
    render.cpp
    ${STROKE_LAB_VARIANTS}
)
target_include_directories(stroke-lab PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${UQ_SOURCE_ROOT}/tests        # support/referencestrokes.h
)
target_link_libraries(stroke-lab PRIVATE stroke_lab_ink Qt6::Core Qt6::Gui)
set_target_properties(stroke-lab PROPERTIES AUTOMOC OFF)

# Качество кромки тонкой линии: MSAA 4x/8x/16x против аналитического покрытия
# (exp/dry-refine). От вариантов не зависит.
add_executable(stroke-lab-aabench aabench.cpp raster.cpp)
target_include_directories(stroke-lab-aabench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(stroke-lab-aabench PRIVATE Qt6::Core Qt6::Gui)
set_target_properties(stroke-lab-aabench PROPERTIES AUTOMOC OFF)
```
  `git add tools/stroke-lab/CMakeLists.txt`.

- [x] 1.16 **Гейт: сборка, побайтная сверка, baseline не изменился.** (`SAMPLES` - из 1.4.)
```sh
sha256sum -c - <<'EOF'
07751d58b62d2b273bd48992f77629e238bfd00921a446c066b3d08cddb99c32  tools/stroke-lab/main.cpp
260e6e7ae10f44ca06d87fc9289ab291b6ed1d03ed3286f554379aacc3cb19fc  tools/stroke-lab/metrics.cpp
ac4bc59e04bb7a1cb3538504baea3cad8800c38737efecc91b74a2523531bbd7  tools/stroke-lab/metrics.h
be083799e8dc8fbb2dfba2ea252bea97cd30af2b1b80bf45be96f2b4a34c1a26  tools/stroke-lab/corpus.cpp
c0882294e749f6fc3fe556ede8fc1a6354a89830c057cbf2b4ed9038bb86f75e  tools/stroke-lab/corpus.h
d3be1591f7baca619c057f7169a63e6b0224c2d27c02ec7ab179f7d6b5c50f1e  tools/stroke-lab/render.cpp
96d9dc9d13d2dd26c385da0300956a9481f2ed3afd01d79ddd0671e1add34970  tools/stroke-lab/render.h
37ce66d2910b14ba8fc14ff9c75eb7756eb182d0a7b752c9aff4d01bffeaf359  tools/stroke-lab/raster.cpp
7f9944bd81397457aafeb614c09621d249acf472493056800847ffc3f0bcae86  tools/stroke-lab/raster.h
aa1e06eebf7410db7836731f771c0fbc06989cb2484e0fac9d17cdca93dd4365  tools/stroke-lab/lab.h
0da961959d15a87b621f5f85cbe38b71136aaf22698cbecf334baf16b0445f69  tools/stroke-lab/endcheck.cpp
3bd9c5f0952ab6dbcee190c028f6c2870e919e677822f2a2a16d29c8d23a9eeb  tools/stroke-lab/endcheck.h
816380de649fba0b4a61f55c36fd8a35e44fcea4b1fddae98faf1e1e55d1896a  tools/stroke-lab/aaraster.cpp
b9b9b5b92bdb7fb248a4ec07b19810c03bfdad7937e19be626b2508efb1cf968  tools/stroke-lab/aaraster.h
dde381e5072110f8df5deab4e1b0becd7be33f82b7d5fc66d87d580f430671f3  tools/stroke-lab/rastermetrics.cpp
3cc4e7050220023827f2fa41b8112c85a5545450582a1ee08b84848bf91ee1b4  tools/stroke-lab/rastermetrics.h
424acf18d036c04f631efd70c396d81c12af63a55c4547bbdf604fbcb5004ac6  tools/stroke-lab/aabench.cpp
c5696238346a603c1c74e85f449a12dda153eb6780bf23dd853aa24cbcab4d9d  tools/stroke-lab/variants/baseline.cpp
EOF
```
  После рецензии (коммит «Стенд: пустые средние - null, cap_dev по обоим торцам», следующий за 5a8b707) metrics.cpp, main.cpp и endcheck.cpp исправлены: пустые средние пишутся null, cap_dev проверяет оба торца; суммы выше - для состояния до этой правки.
  Ожидание: все `OK`. Если какой-то файл не совпал - разрешение конфликта ушло не туда; сравнить с эталонным диффом `git diff HEAD -- tools/stroke-lab/<файл>` против описания шагов 1.7-1.14 (рядом с планом лежит `openspec/changes/ink-beauty-pass/stroke-lab.patch` - тот же итог одним диффом от 720b497, включая `CMakeLists.txt` и `README.md` стенда; накладывается `git apply` поверх 1.5, если рецепт слияния разошёлся).
```sh
cmake --build build -j$(nproc) --target stroke-lab stroke-lab-aabench
rm -rf /tmp/lab-merged
./build/tools/stroke-lab/stroke-lab --variants baseline --no-png --samples $SAMPLES --out /tmp/lab-merged >/dev/null
python3 - /tmp/lab-720b497/summary.json /tmp/lab-merged/summary.json <<'EOF'
import json, sys
a = json.load(open(sys.argv[1]))['baseline']
b = json.load(open(sys.argv[2]))['baseline']
# 1) все ключи 720b497 на обоих масштабах - те же, кроме времени CPU
bad = [f'{sc}:{k} {v} -> {b[sc].get(k)}' for sc in a for k, v in a[sc].items()
       if not k.startswith('cpu_') and b[sc].get(k) != v]
# 2) перенесённые метрики дают числа design.md (baseline, масштаб 1)
want = {'width_track_rms_rel@mean': (0.1628, 5e-5), 'width_rise_px@mean': (36.361, 1e-3),
        'width_end_ratio@mean': (0.9496, 5e-5), 'width_mean_ratio@mean': (0.9257, 5e-5),
        'frame_gap_px_med_L22@mean': (9.494, 1e-3), 'aa_rmse_shape@mean': (27.626, 1e-3),
        'aa_flicker@mean': (18.679, 1e-3)}
s1 = b['s1']
bad += [f'{k}={s1.get(k)}' for k, (v, tol) in want.items() if s1.get(k) is None or abs(s1[k] - v) > tol]
print('FAIL\n' + '\n'.join(bad) if bad else 'OK')
EOF
```
  Ожидание: `OK`. Это и есть гейт D9 для шага 0: отслеживание толщины 0.163, заход 36.4 px, конец 0.95, средняя толщина 0.926 (D2), недогон 9.5 px (≈ 23 мс, Risks), ошибка кромки 27.6, мерцание 18.7 (D7). Проверка `settled_mutations` теперь ловит и изменение толщины окончательной точки - у baseline по-прежнему 0.
```sh
rm -rf /tmp/lab-full && time ./build/tools/stroke-lab/stroke-lab --variants baseline --out /tmp/lab-full >/dev/null
./build/tools/stroke-lab/stroke-lab --list | grep -c '^  '        # ожидание: 30 (1 вариант + 29 сэмплов)
./build/tools/stroke-lab/stroke-lab --variants baseline --endcheck | sed -n 3,8p
./build/tools/stroke-lab/stroke-lab-aabench --out /tmp/aabench | head -3 && ls /tmp/aabench
```
  Ожидание: полный прогон с PNG за несколько секунд (в песочнице 3.4 с), в `/tmp/lab-full` есть `baseline/marker/`, `baseline/aaerr/`, `baseline/wet/` (лента из четырёх кадров); `--endcheck` печатает таблицу с тремя строками baseline и `cap_dev` 1.000 (плоский торец); `aabench` печатает таблицу MSAA/аналитики и пишет `aabench.json`, `aabench.png`. Действие при провале: `FAIL` по ключам 720b497 значит, что разрешение конфликта задело общий код (`runVariant`, `computeMetrics`) - см. сверку контрольных сумм выше.

- [x] 1.17 **Записать `tools/stroke-lab/README.md` целиком.** Прежний README плюс то, что принесли ветки, без описаний вариантов, которых в дереве нет; раздел «Числа дизайна» с командой сверки; провенанс метрик:
````markdown
# stroke-lab

Офлайн-стенд для сравнения конвейеров рукописных чернил: один и тот же
синтетический почерк с известным эталоном проходит через несколько вариантов
«отсчёты пера → осевая линия → геометрия → пиксели», и для каждого получаются
PNG и метрики в JSON. Работает без экрана (`QT_QPA_PLATFORM=offscreen`
выставляется сам).

Стенд - часть основного дерева (`openspec/changes/ink-beauty-pass/design.md`,
D1): каждое число дизайна воспроизводится командой отсюда. В пакет он не
входит.

## Сборка и запуск

Вместе с приложением (так проверяются шаги ink-beauty-pass):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUQ_BUILD_STROKE_LAB=ON
cmake --build build -j$(nproc) --target stroke-lab stroke-lab-aabench
./build/tools/stroke-lab/stroke-lab --list                     # варианты и сэмплы
./build/tools/stroke-lab/stroke-lab --variants baseline,foo --out /tmp/lab
./build/tools/stroke-lab/stroke-lab --variants foo --samples 'small_*,word_hello' --scales 2.5 --out /tmp/lab
```

Отдельно, без QML и PDF (быстрее, не нужен qpdf):

```sh
cmake -S tools/stroke-lab -B build-lab -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-lab
./build-lab/stroke-lab --list
```

`build/` и `build-lab/` попадают под `.gitignore`. Опция `UQ_BUILD_STROKE_LAB`
по умолчанию выключена, поэтому обычная сборка и `packaging/PKGBUILD` стенд не
собирают, а `install()` у стенда нет.

Ключи:

| ключ | по умолчанию | смысл |
|---|---|---|
| `--variants a,b` | все | какие варианты гонять; `baseline` всегда первый столбец |
| `--samples x,y*` | все | сэмплы, `*` в конце - префикс |
| `--scales 1,2.5` | `1,2.5` | зум страницы (`m_zoom` приложения) |
| `--width 1.0` | `1.0` | толщина пера в точках страницы (`m_penWidth` по умолчанию 1.0; в меню 0.4…4.5) |
| `--dpr 1` | `1` | devicePixelRatio экрана; на HiDPI задавайте 1.5/2 |
| `--reps 3` | `3` | повторы для замера CPU, берётся минимум на событие |
| `--min-width-factor f` | `0.25` | нижний предел эталонного закона толщины `w·(f + (1−f)·p)`; для вариантов с законом стиля 1 (D3) - `0.08` |
| `--noise 0.05` | `0.05` | шум датчика σ, px |
| `--rate 481` | `481` | частота пера, Гц |
| `--libinput-smoothing` | | эмулировать среднее 4 последних отсчётов libinput (`tablet_smoothen_axes`) |
| `--no-png` | | только метрики (быстро); `raster_vs_*` без PNG не считаются |
| `--no-raster` | | без метрик растеризации `aa_*`, `marker_*` |
| `--shifts 8` | `8` | субпиксельных сдвигов для `aa_flicker` |
| `--endcheck` | | вместо корпуса - синтетика концов штриха и окружность с шумом, печатает таблицы и выходит |

Весь прогон baseline (29 сэмплов × 2 масштаба, PNG и растровые метрики) -
около 3.5 с.

## Числа дизайна: проверка baseline

Числа `design.md` посчитаны на корпусе из 27 сэмплов - до того, как в него
вошли `dots_tap` и `dots_taps`. Средние по корпусу от двух новых сэмплов
сдвигаются, поэтому сверка идёт по прежнему списку:

```sh
SAMPLES=ref_line,ref_loop,ref_zigzag,ref_spiral,ref_letter_e,ref_fast_circle,ref_corner_v,ref_corner_n,ref_cardiogram,word_minimum,word_hello,word_privet,digits_2025,word_minimum_small,small_o_8,small_o_12,small_o_20,small_a_12,small_e_10,corner_N,corner_M,corner_check,fast_eeee,fast_scribble,slow_line_tremor,slow_arc_tremor,slow_hello
./build/tools/stroke-lab/stroke-lab --variants baseline --no-png --samples $SAMPLES --out /tmp/lab-baseline
```

Ожидаемое в `/tmp/lab-baseline/summary.md`, масштаб 1:

| столбец | ключ `summary.json` | baseline |
|---|---|---|
| dev mean | `centerline_dev_mean_px@mean` | 0.1068 |
| corner mean / max | `corner_dist_mean_px@mean` / `corner_dist_max_px@max` | 0.5618 / 1.413 |
| facets>2° | `facet_count_2deg@sum` | 2818 |
| curvTV excess | `curvature_tv_excess_per100px@mean` | 3.252 |
| small H% | `shape_hausdorff_pct@mean` | 6.616 |
| w ratio | `width_mean_ratio@mean` | 0.9257 |
| w track | `width_track_rms_rel@mean` | 0.1628 |
| w rise px | `width_rise_px@mean` | 36.36 |
| w end | `width_end_ratio@mean` | 0.9496 |
| gap22 med | `frame_gap_px_med_L22@mean` | 9.494 |
| AA rmse | `aa_rmse_shape@mean` | 27.63 |
| flicker | `aa_flicker@mean` | 18.68 |
| mut | `settled_mutations@sum` | 0 |
| µs/ev | `cpu_us_per_event_mean@mean` | около 2.2 (зависит от машины) |

Всё, кроме `µs/ev`, детерминировано и должно совпасть до последнего знака.

## Что получается в `--out`

```
summary.md / summary.json      сводная таблица: вариант × масштаб, агрегаты ключевых метрик
contact_s1.png, contact_s2.5.png   все сэмплы: truth | <variant> (app) | <variant> (ideal raster) | ...
crops_s1.png, crops_s2.5.png   то же, фрагменты 32×24 px ×8 (nearest)
compare/<sample>_s<scale>.png  один сэмпл: сверху целиком (×2/×3), снизу фрагменты ×8 - смотреть сюда в первую очередь
truth/<sample>_s<scale>.png    эталон 1:1
<variant>/metrics.json         все метрики по каждому сэмплу и масштабу
<variant>/png/<tag>.png        как в приложении, 1:1 (физические пиксели)
<variant>/png/<tag>_ideal.png  осевая линия варианта, нарисованная идеально (см. ниже)
<variant>/wet/<tag>.png        лента мокрых кадров на 30/45/60/75% отсчётов, ×4
<variant>/crops/<tag>.png      фрагмент ×8
<variant>/marker/<tag>.png     тот же штрих полупрозрачным маркером
<variant>/aaerr/<tag>.png      карта ошибки сглаживания ×4: красное - темнее точного покрытия, синее - светлее
```

Столбцы картинок:

- **truth (ideal AA)** - эталонная траектория без тремора и шума, толщина по
  эталонному давлению по закону приложения
  (`width·(0.25 + 0.75·p)`, нижний предел меняет `--min-width-factor`),
  круглая кисть, покрытие по решётке 8×8.
- **app** - сухая геометрия варианта, растеризованная эмуляцией MSAA 4x (или
  своей растеризацией варианта).
- **ideal raster** - та же осевая линия и толщина варианта, но круглой кистью
  с покрытием 8×8. Разница app ↔ ideal показывает вклад тесселяции и
  растеризации; разница ideal ↔ truth - вклад сглаживания траектории и
  давления.
- **wet** - оранжевые точки - отсчёты пера, зелёная линия - эталон до этого
  момента, пурпурный пунктир - путь пера ещё на 22 мс вперёд (где перо будет в
  момент фотона), пурпурный косой крест - это положение, красные точки -
  провизорный хвост, красное кольцо - кончик мокрой линии, синий крест -
  положение пера в отсчёте.

## Растеризация «как в приложении»

Приложение рисует штрих `QSGFlatColorMaterial` (непрозрачный цвет, без
смешивания) треугольной полосой в окно с MSAA 4x (`main.cpp`,
`format.setSamples(4)`). Лаборатория повторяет это точно по выборке, а не
приближённо через QPainter (`raster.cpp`):

- 4 точки выборки на пиксель - стандартный шаблон 4x (D3D10+/GL, его же даёт
  Mesa на Intel): `(0.375,0.125) (0.875,0.375) (0.125,0.625) (0.625,0.875)`;
- точка покрыта, если лежит внутри хотя бы одного невырожденного треугольника
  (перекрытие треугольников полосы на непрозрачном цвете ничего не меняет -
  покрытие - объединение); правило «top-left» на рёбрах не эмулируется, точка
  на ребре считается покрытой - разница в долях процента пикселей на краю;
- resolve - среднее по точкам в значениях sRGB, без гаммы (буфер окна не sRGB):
  `pixel = paper + (ink − paper)·covered/4`. Отсюда только 5 уровней покрытия.

Бумага `#ffffff`, чернила `#1c2a5a` (`m_penColor`). Не эмулируются:
масштабирование текстуры страницы под чернилами, промежуточные кадры, дизеринг.
Для контура (`Geometry::Kind::Polygons`) используется тот же движок, заливка
по правилу nonzero построчно для каждой точки выборки.

`CoverageBuffer::compositeOntoLinear` - то же, но со смешиванием в линейном
свете, для вариантов, которые хотят проверить гамма-корректное сглаживание.
`SamplePattern::lattice64` - 64 точки решётки ранга 1, эталон для метрик
растеризации (у `grid(8)` почти горизонтальная кромка даёт только 8 уровней).

## Корпус (`corpus.cpp`)

Всё задаётся в экранных логических пикселях - так движется рука при любом
зуме; варианту отсчёты отдаются в точках страницы (`/ scale`).

- **Геометрия.** Эталоны из `tests/support/referencestrokes.h` (прямая, петля,
  зигзаг, спираль, «e», быстрая окружность, «V», «N», кардиограмма) и
  рукописные глифы: связное письмо (`minimum`, `hello`, `привет`), цифры
  `2025` (каждая отдельным штрихом), мелкие `o` 8/12/20 px, `a` 12, `e` 10,
  печатные `N`, `M`, галочка, быстрые `eeee` и каракули, медленные линия, дуга
  и `hello` с тремором. Глиф - контрольные точки в долях x-height, разбитые на
  части по остриям; часть - centripetal Catmull-Rom через свои точки, наклон
  12°, лёгкий разброс точек (σ = 0.025 x-height). Точки над `i` пишутся после
  слова отдельными штрихами.
- **Точки-касания.** `dots_tap` (тег `tap`) - пять касаний со сдвигом
  0.15…3 px на 15 px/s: точка над i, пунктуация; видно, пропадает ли касание и
  какой оно формы. `dots_taps` (тег `dots`) - шесть касаний со сдвигом
  0.3…3 px на 60 px/s. Касание совсем без сдвига корпус не умеет (эталону
  нужно 2 точки); ближайшее к нему - касание со сдвигом 0.3 px в `--endcheck`.
- **Скорость.** Закон 2/3: `v = cruise · clamp((κref/κ)^(1/3), 0.35, 1.6)`,
  `κref = 1/curvatureRadius`; в каждом острие и на касании/отрыве скорость
  гладко уходит в ноль (разгон smoothstep 35 мс, торможение `v ∝ √(остаток)`
  на 45 мс), в острие стоянка 6 мс. Интегрирование с шагом 0.1 мс - это и есть
  эталон `TruePath` (положение, время, скорость, давление).
- **Давление.** Нарастание за 30 мс от 0.05, спад за 40 мс к отрыву, база
  0.48…0.6 с медленной волной 0.9 и 2.3 Гц, на быстром чуть легче; шум σ 0.004,
  квантование 1/4096.
- **Отсчёты пера.** 481 Гц (`--rate`) со случайной фазой; метка времени
  округлена вниз до 1 мс (как `QEvent::timestamp()`); положение = эталон +
  физиологический тремор (по 3 синусоиды 8–12 Гц на ось, амплитуда 0.12–0.3 px,
  модуляция 1.3 Гц) + шум датчика σ 0.05 px (`--noise`), квантование 1/256 px
  (`wl_fixed`); `--libinput-smoothing` усредняет 4 последних отсчёта, как
  libinput для части планшетов.
- Острия с поворотом > 45° записываются как эталонные вершины углов.

Генератор детерминирован (свой splitmix64 и Box-Muller), у каждого сэмпла своё
зерно, корпус одинаков на любой машине.

## Метрики (`metrics.cpp`, все в экранных логических px)

| ключ | что меряет |
|---|---|
| `centerline_dev_mean/rms/max_px` | расстояние точек итоговой осевой до эталонной ломаной |
| `truth_coverage_mean/max_px` | обратное: точки эталона (шаг 0.5 px) до осевой - ловит срезанные углы и сжатые петли |
| `facet_count_2deg`, `facet_max_deg`, `facet_p99_deg` | грань: `|θ_i − (θ_{i−1}+θ_{i+1})/2|` по вершинам осевой (θ - угол поворота), без 2 px у концов и 4 px у углов; `STROKE_LAB_DEBUG_FACETS=1` печатает грани больше 60° |
| `turn_max_deg` | наибольший поворот в одной вершине там же |
| `curvature_tv_per100px`, `_truth_`, `_excess_`, `_ratio` | Σ|Δκ| на равномерной нарезке 1 px (κ - поворот на шаг), на 100 px длины; excess = вариант − эталон, ratio = (tv+1)/(tv_gt+1) |
| `corner_dist_mean/max_px` | от эталонной вершины угла до осевой; `STROKE_LAB_DEBUG_CORNERS=1` печатает каждую вершину |
| `shape_dev_mean_pct`, `shape_hausdorff_pct` | для `small`: отклонение / x-height, % |
| `closed_area_ratio` | для `closed` (буквы o): площадь осевой / площадь эталона |
| `width_tv_per100px` (+`_truth_`), `width_hf_rms_px`, `width_mean_ratio` | толщина вдоль дуги (1 px): Σ|Δw|, ВЧ-остаток после скользящего среднего 9 px, средняя толщина / эталонная |
| `width_track_rms_px`, `width_track_rms_rel` | RMS разности толщины варианта и эталона при сопоставлении по доле длины дуги (абсолютно и к средней эталонной) - ловит запаздывание фильтра давления; гейт шага 1: `width_track_rms_rel` ≤ 0.03 |
| `width_rise_px`, `width_fall_px` (+`_truth_`) | длина дуги от начала / до конца, пока толщина ниже 90% медианы штриха (заход и выход) |
| `width_start_ratio`, `width_end_ratio` (+`_truth`) | средняя толщина первых / последних 3 отсчётов нарезки (≈2 px) к медиане штриха: тупой конец ≈ 1, заострённый < 1 |
| `width_start_err_px`, `width_end_err_px`, `width_body_err_px` | ошибка толщины к эталону в той же точке эталона (проекция): первые и последние 10 px дуги эталона и тело |
| `width_step_max_px` (+`_truth_`), `width_body_cv` (+`_truth`) | наибольший скачок толщины на 1 px дуги; коэффициент вариации толщины в теле штриха |
| `width_err_mean_px`, `width_err_rel_mean_pct` | ошибка толщины к эталону в ближайшей точке эталона вдоль осевой (шаг 1 px) |
| `width_touch_ratio`, `width_lift_ratio` | толщина первой / последней точки осевой к эталонной толщине на касании / отрыве |
| `width_end_jump_max_pct` (+`_truth_`) | наибольший относительный скачок толщины на последних 3 точках («клюв» на отрыве) |
| `pressure_lag_ms`, `pressure_err_mean` | сдвиг давления осевой к эталону, при котором они лучше всего совпадают, и ошибка давления без сдвига |
| `wet_lag_px_*` | кончик мокрой линии ↔ положение пера в этом отсчёте |
| `wet_lag_truth_px_mean` | кончик ↔ эталон в настоящий момент отсчёта |
| `wet_lag_ms_mean/p95` | время ближайшей к кончику точки эталона в окне [−60, +10] мс, только где перо быстрее 50 px/s |
| `wet_vs_final_mean/max_px` | насколько провизорный хвост расходится с итоговой осевой |
| `frame_*` | эмуляция кадров, см. ниже |
| `settled_mutations` | сколько «окончательных» точек потом изменилось положением или толщиной (должно быть 0) |
| `cpu_us_per_event_mean/p50/p99/max`, `cpu_us_finish` | время `addEvent` (минимум по повторам) и `finish` |
| `final_vertices`, `centerline_points`, `wet_dirty_vertices_mean` | размер сухой геометрии и сколько вершин мокрой полосы перезаливается на событие |
| `raster_vs_ideal_rms/_p99/_ink_ratio` | только с PNG: покрытие картинки «app» против «ideal raster» той же осевой (вклад тесселяции и растеризации): RMS и p99 по пикселям с покрытием, масса чернил Σcov/Σcov эталона |
| `raster_vs_truth_rms/_p99/_ink_ratio` | то же против картинки truth (всё вместе: траектория, толщина, растеризация); на `dots_taps` - доля чернил точек |
| `width_cv`, `width_p5_physpx`, `width_mean_physpx` | ровность толщины: коэффициент вариации полной толщины по осевой (взвешен длиной), 5-й перцентиль и среднее в физических px |

### Эмуляция кадров (`frame_*`)

Кадр 120 Гц защёлкивает мокрое состояние в момент t_l: в него попадают
отсчёты, чьё настоящее время + 2 мс (доставка ввода) не позже t_l. Фотон
выходит через L − 2 мс после защёлки, то есть L - полная задержка «перо →
фотон»; L = 16, 22 и 28 мс (суффикс `_L16`, `_L22`, `_L28`), фаза кадров
перебирается четырьмя сдвигами.

| ключ | что меряет |
|---|---|
| `frame_gap_px_mean/med/p95_L*` | длина дуги эталона от проекции кончика до пера в момент фотона - «линия не успевает»; только где перо быстрее 50 px/s |
| `frame_gap_fast_px_med_L*` | то же на 150-600 px/s |
| `frame_gap_ms_med_L*` | тот же зазор во времени |
| `frame_ahead_px_p99/max_L*` | кончик впереди настоящего пера |
| `frame_lift_ahead_px_max` | то же после отрыва, пока отрыв не обработан («запятая») |
| `frame_offpath_px_p99/max` | дальняя точка провизорного хвоста от эталона в окне [−80, +60] мс - вылет мимо траектории |
| `frame_retract_px_p95/p99` | насколько хвост кадра не совпал с линией следующего кадра - «втягивание» |
| `frame_heading_err_deg_p95`, `frame_wag_deg_p95` | курс кончика против касательной эталона и его скачок между кадрами - «виляние» |
| `frame_tail_vs_final_px_p95/p99` | дальняя точка хвоста кадра от итоговой осевой |

Лаг и зазор меряются на модели доставки и кадра; настоящая задержка устройства
меряется в приложении (`UQ_FRAME_LOG`, `PERFORMANCE.md`).

## Метрики растеризации (`rastermetrics.cpp`)

Считаются для каждого варианта поверх траекторных (выключаются `--no-raster`,
число субпиксельных сдвигов - `--shifts 8`). Ошибки - доли покрытия ×255.
Эталонное покрытие - `SamplePattern::lattice64`.

| ключ | что меряет |
|---|---|
| `aa_rmse_shape`, `aa_p99_shape`, `aa_max_shape` | пиксели варианта против точного покрытия его же геометрии, только кромка (частичное покрытие хотя бы в одном) - чистая ошибка сглаживания |
| `aa_rmse_ideal` | против круглой кисти по осевой варианта (сглаживание + стыки/торцы) |
| `aa_rmse_truth` | против эталона (всё вместе) |
| `aa_flicker` | «дыхание» кромки при пане: std по субпиксельным сдвигам ошибки (вариант − форма), среднее по пикселям |
| `aa_mass_breath_pct` | std/mean суммарной массы чернил по сдвигам, % |
| `aa_mass_ratio` | масса чернил / масса идеальной кисти |
| `aa_mass_cv` | «бусины»: масса в отрезках осевой по 2 px относительно идеала, коэффициент вариации |
| `marker_dark_frac`, `marker_alpha_max` | тот же штрих маркером `#66f2d024`: доля пикселей с альфой > 0.41 (двойное наложение) |

## `--endcheck`: концы штриха (`endcheck.cpp`)

Прямая на 100 / 300 / 600 px/s, 481 Гц, метки по 1 мс, без шума; давление
нарастает 0.05 → 0.6 за 15 мс и спадает до нуля за 12 мс до отрыва. Плюс
касание со сдвигом 0.3 px за 30 мс и окружность r = 20 px с коррелированным
шумом AR(1) (ρ 0.6, σ 0.3 px).

```sh
./build/tools/stroke-lab/stroke-lab --variants baseline,foo --endcheck
```

| столбец | смысл |
|---|---|
| `entry px` / `exit px` (+`ref`) | длина дуги от начала / до конца, пока толщина ниже 90% плато; `ref` - закон приложения на настоящем давлении |
| `step_end` | наибольший относительный скачок толщины на последних 3 толщинах плато |
| `cap_dev` | отклонение вершин геометрии за концом штриха от окружности радиуса полутолщины, в долях радиуса: 0 - полудиск, 1 - плоский торец; гейт шага 2: ≤ 0.05 |
| `tap aspect` | отношение сторон охвата геометрии касания: 1 - диск |
| `residual px`, `rough deg`, `radius bias px` | окружность: СКО радиального отклонения, СКО второй разности угла касательной, смещение радиуса |

## `stroke-lab-aabench`: кромка тонкой линии

```sh
./build/tools/stroke-lab/stroke-lab-aabench --out /tmp/aabench
```

Прямые полосы 0.25-3 device px под углами 0-45°: СКО покрытия к точной площади,
«бусинистость» (CV суммы покрытия по поперечному срезу) и перепад пика для MSAA
4x/8x/16x и аналитических формул. Пишет `aabench.json` и `aabench.png`. От
вариантов не зависит.

## Откуда что

Стенд пришёл из `exp/stroke-lab` (720b497), метрики - из веток прототипов:

| ветка | что перенесено |
|---|---|
| `exp/rnote` | профиль толщины (`width_track_*`, `width_rise/fall_*`, `width_start/end_ratio`), `--min-width-factor` |
| `exp/outline` | `raster_vs_ideal/truth_*`, `width_cv`, `width_p5_physpx`, сэмпл `dots_tap` |
| `exp/google-ink` | сэмпл `dots_taps` |
| `exp/trajectory` | `--noise`, `--rate`, `--libinput-smoothing` |
| `exp/apps-survey` | `--endcheck`, `width_*_err_px`, `width_step_max_px`, `width_body_cv`, толщина в `settled_mutations` |
| `exp/latency` | эмуляция кадров `frame_*`, лента мокрых кадров |
| `exp/dry-refine` | `width_err_*`, `width_touch/lift_ratio` (в ветке - `width_start/end_ratio`), `width_end_jump_*`, `pressure_lag_ms`, `stroke-lab-aabench` |
| `exp/antialias` | `rastermetrics.cpp`, `aaraster.*`, `SamplePattern::lattice64` |

Варианты прототипов остаются в своих ветках; шаг изменения, который берёт
решение, приносит с собой и его вариант (`git show exp/<ветка>:tools/stroke-lab/variants/<файл>`).
GPU-замер `aa-gpu-bench` (`exp/antialias`, `gpu/`) рисует полосой варианта
`aa-fringe`, поэтому переносится вместе с ним в шаге 3.

## Как добавить вариант

1. Скопируйте `variants/baseline.cpp` в `variants/<имя>.cpp`.
2. Реализуйте `lab::Variant` (`lab.h`):
   - `begin(StrokeSetup)` - масштаб, толщина, цвет на касании;
   - `addEvent(InkPoint)` - отсчёт в точках страницы с давлением и временем;
     **вся** работа на отсчёт должна быть здесь (её время меряется);
   - `wet(from, WetState&)` - окончательные точки с индекса `from`, их
     общее число, провизорный хвост, размер мокрой геометрии;
   - `finish()`, затем `centerline()` (то, что сохранится в `.uqn`: точка +
     полная толщина в точках страницы) и `geometry()` (полоса, треугольники
     или контур в точках страницы);
   - необязательно: `wetGeometry()` для снимка мокрого кадра и
     `rasterize(Geometry, RasterTarget)` - своя растеризация (вершинная альфа,
     аналитическое сглаживание, шейдерная эмуляция); верните `true`, если
     нарисовали. Помощники - `CoverageBuffer` из `raster.h`, `aaraster.h`.
3. В конце файла: `STROKE_LAB_VARIANT(MyVariant, "my-variant", "описание");`
4. Пересоберите (новый файл подхватывается сам через `CONFIGURE_DEPENDS`) и
   `stroke-lab --variants baseline,my-variant --out ...`.

Можно наследоваться от классов приложения (`StrokeSampler`, `WetStroke`,
`StrokeTessellator` из `src/ink`) или скопировать их в вариант и менять
параметры. Ограничения приложения, которые вариант должен уважать: в `.uqn`
хранятся только `x, y, pressure` (без времени); окончательные точки не
меняются; работа на событие не растёт с длиной штриха.
````
  `git add tools/stroke-lab/README.md`.

- [x] 1.18 **Проверка готовности для шагов 1-3 (временная, без коммита): варианты из веток собираются со стендом как есть.**
```sh
V=tools/stroke-lab/variants
for spec in rnote/e2_pressure.cpp rnote/e3_follow.cpp rnote/rnote_kit.h outline/outline.cpp \
            outline/outline_common.h google-ink/google_ink.cpp dry-refine/dry_refine.cpp \
            antialias/aa_fringe.cpp antialias/aa_fringe.h antialias/app_pipeline.h; do
  git show "exp/${spec%%/*}:$V/${spec#*/}" > "$V/${spec#*/}"
done
cmake --build build -j$(nproc) --target stroke-lab
./build/tools/stroke-lab/stroke-lab --variants baseline,e2-pressure-caps,dr-resample-tol,outline-caps,aa-fringe \
  --no-png --samples $SAMPLES --out /tmp/lab-variants >/dev/null
python3 -c "
import json; b = json.load(open('/tmp/lab-variants/summary.json'))
for v in b:
    s = b[v]['s1']; print(v, {k: round(s[k], 4) for k in ['width_track_rms_rel@mean', 'width_rise_px@mean', 'width_end_ratio@mean', 'corner_dist_mean_px@mean', 'corner_dist_max_px@max', 'aa_rmse_shape@mean', 'settled_mutations@sum']})"
./build/tools/stroke-lab/stroke-lab --variants outline-caps --endcheck | sed -n 5,7p
rm $V/e2_pressure.cpp $V/e3_follow.cpp $V/rnote_kit.h $V/outline.cpp $V/outline_common.h \
   $V/google_ink.cpp $V/dry_refine.cpp $V/aa_fringe.cpp $V/aa_fringe.h $V/app_pipeline.h
git status --short tools/stroke-lab   # ожидание: только проиндексированные правки 1.7-1.17, лишних файлов нет
cmake --build build --target stroke-lab   # пересборка без вариантов
```
  Ожидание (песочница): `e2-pressure-caps` - отслеживание 0.0238, заход 11.42, конец 0.697 (D2: 0.024 / 11.4 / 0.70); `dr-resample-tol` - вершина острия 0.082 / 0.482 (D6: 0.08 / 0.48); `aa-fringe` - 11.32 на масштабе 1; `outline-caps` в `--endcheck` - `cap_dev` 0.000; `settled_mutations` 0 у всех. При провале: вариант зависит от инфраструктуры, которой в этом слиянии нет - найти по ошибке компиляции и дописать в шаг, который этот вариант приносит.

- [x] 1.19 **Промежуточный коммит.**
```sh
git commit -F - <<'EOF'
Метрики прототипов в стенде

Стенд получает метрики, которые прототипы добавили в своих ветках: профиль
толщины (exp/rnote), сравнение растров и сэмпл dots_tap (exp/outline),
dots_taps (exp/google-ink), шум и частота корпуса (exp/trajectory),
--endcheck и ошибки толщины по зонам (exp/apps-survey), эмуляцию кадров
(exp/latency), сдвиг давления и stroke-lab-aabench (exp/dry-refine), метрики
растеризации (exp/antialias). Метрики exp/dry-refine для первой и последней
точки переименованы в width_touch_ratio и width_lift_ratio: прежние имена
заняты профилем exp/rnote, по которому посчитаны числа design.md.

Варианты остаются в ветках, их приносят шаги, которые берут решения.
Baseline на прежних 27 сэмплах совпадает с 720b497 во всех метриках, кроме
времени, и даёт числа design.md для новых.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

- [x] 1.20 **Записать для шагов 1-3, откуда брать варианты** (справка, кода нет; файлы берутся `git show exp/<ветка>:tools/stroke-lab/variants/<файл> > tools/stroke-lab/variants/<файл>` и коммитятся в своём шаге; совместимость со стендом проверена в 1.18):

  | шаг | решение | ветка | файлы | варианты |
  |---|---|---|---|---|
  | 1 | D2 давление, D3 закон стиля 1 | `exp/rnote` | `variants/e2_pressure.cpp`, `variants/rnote_kit.h` | `e2-pressure-caps`, `e2-taper` (сравнивать с `--min-width-factor 0.08`), `e2-nodetime` |
  | 2 | D4 торцы | `exp/outline` | `variants/outline.cpp`, `variants/outline_common.h` | `outline-caps` (`--endcheck`: `cap_dev` 0) |
  | 2 | D5 точки | `exp/google-ink` | `variants/google_ink.cpp` | `gi-roundcaps` (точки-касания, `dots_taps`) |
  | 2 | D6 узел острия | `exp/dry-refine` | `variants/dry_refine.cpp` | `dr-resample-tol`, контроль `dr-copy` |
  | 2 | D6 окно [1,2,1]/4 | `exp/rnote` | `variants/e3_follow.cpp`, `variants/rnote_kit.h` (если ещё нет после шага 1) | `e3-raw121` |
  | 3 | D7 материал | `exp/antialias` | `variants/aa_fringe.cpp`, `variants/aa_fringe.h`, `variants/app_pipeline.h` | `aa-fringe`, `aa-fringe-naive`, `aa-fringe-nofloor` |
  | 3 (и гейт шага 4) | GPU-замер | `exp/antialias` | `gpu/bench.cpp`, `gpu/ink.vert`, `gpu/ink.frag`, `gpu/flat.vert`, `gpu/flat.frag`; в `tools/stroke-lab/CMakeLists.txt` - блок `find_package(Qt6 QUIET COMPONENTS ShaderTools GuiPrivate)` … `qt_add_shaders(aa-gpu-bench ...)` из `git show exp/antialias:tools/stroke-lab/CMakeLists.txt` (строки после `set_target_properties(stroke-lab ...)`) | цель `aa-gpu-bench` |
  | 3 | гейт складки D7 | `exp/outline` | те же `outline.cpp`, `outline_common.h` | `outline-aa-over` (снимок `e2_fold_small_e_s1_dpr2.png`) |

  Замечание для шага 3: в design.md «ошибка кромки 27.6 → 10.8» сравнивает baseline на масштабе 1 (27.63) со средним `aa-fringe` по масштабам 1 и 2.5 ((11.32 + 10.33) / 2); на одном масштабе 1 это 27.6 → 11.3, на 2.5 - 24.9 → 10.3. Гейты шага 3 формулировать на одном масштабе.

## 2. Честная латентность

**Files:**
- Create: `src/perf/presentationstats.h`, `src/perf/presentationstats.cpp`, `src/perf/presentationfeedback.h`, `src/perf/presentationfeedback.cpp`, `tests/tst_framestats.cpp`
- Modify: `src/perf/framestats.h`, `src/perf/framestats.cpp`, `src/canvas/documentcanvas.cpp` (`beginInkAt`, `continueInkAt`), `src/CMakeLists.txt`, `CMakeLists.txt` (язык C, опция `UQ_WP_PRESENTATION`), `tests/CMakeLists.txt`, `PERFORMANCE.md`
- Test: `tests/tst_framestats.cpp` (новый), `tests/tst_canvasink.cpp` (`inputLatencyStartsAtEventStamp`), `tests/tst_idlepower.cpp` (`presentationFeedbackIsNotCreatedWhenNotRequested`)

Что проверено (Qt 6.11.2 в системе, исходники qtbase v6.11.2,
`src/plugins/platforms/wayland/`; клиент Wayland в 6.10+ живёт в qtbase, а не в
qtwayland):

- **Шкала `QInputEvent::timestamp()` на Wayland.** Перо приходит через
  `zwp_tablet_tool_v2`: `QWaylandTabletToolV2::zwp_tablet_tool_v2_frame(uint32_t time)`
  кладёт `ulong timestamp = time` прямо в
  `QWindowSystemInterface::handleTabletEvent(window, timestamp, ...)`; у
  `wl_pointer` то же (`mParent->mTime = time`). Значит, это поле `time`
  протокола: миллисекунды, 32 бита. Протокол базу не задаёт; композиторы на
  libinput (KWin, Mutter, Weston, Sway), насколько известно, передают время
  события libinput, то есть `CLOCK_MONOTONIC`, усечённый до 32 бит
  (переполнение раз в 49.7 суток). По исходникам композиторов это не
  проверялось, поэтому код проверяет правдоподобие метки и считает чужие. Синтетическое мышиное событие,
  которое Qt Quick делает из планшетного (так перо доходит до холста, спайк
  1.1), несёт ту же метку. `FrameStats::nowNs()` - `std::chrono::steady_clock`,
  в libstdc++ это `CLOCK_MONOTONIC`, наносекунды. Сопоставление: младшие 32
  бита миллисекунд `nowNs()` против метки, разница по модулю 2^32 со знаком;
  метка правдоподобна при возрасте от −2 до 5000 мс. Иначе (другая база часов,
  метки `QTest` с нуля) латентность считается от момента обработки, а счётчик
  `foreign stamps` в сводке это показывает. Метка округлена вниз до мс, поэтому
  задержка завышается не больше чем на 1 мс.
- **Протокол.** `/usr/share/wayland-protocols/stable/presentation-time/presentation-time.xml`
  есть (wayland-protocols 1.49), `wayland-scanner` 1.26.0 (`pkg-config
  --variable=wayland_scanner wayland-scanner`), `wayland-client` 1.26.0.
  `qt6_generate_wayland_protocol_client_sources` (Qt6WaylandScannerTools)
  дополнительно генерирует C++-обёртки, которым нужны приватные заголовки
  QtWaylandClient, - не берём; хватает `wayland-scanner client-header` и
  `private-code`. Код протокола - C (в C++ `WL_PRIVATE const struct
  wl_interface ...` без предшествующего `extern` получил бы внутреннее
  связывание), поэтому проекту добавляется язык C.
- **Доступ к объектам окна.** `QNativeInterface::QWaylandApplication::display()`
  (публичный `qguiapplication_platform.h`, `QT_FEATURE_wayland 1`);
  `QGuiApplication::platformNativeInterface()->nativeResourceForWindow("surface", window)`
  (`qpa/qplatformnativeinterface.h`, нужен `Qt6::GuiPrivate`; в
  `QWaylandNativeInterface` ресурс `"surface"` возвращает
  `QWaylandWindow::wlSurface()`, у окна без платформенного окна - `nullptr`).
- **Когда заказывать feedback.** Запрос относится к ближайшему
  `wl_surface.commit`; его делает `eglSwapBuffers` в
  `QWaylandGLContext::swapBuffers` (перед ним `waitForFrameSync` и
  `handleUpdate`, которые не коммитят). Значит, заказ в `afterRendering` (поток
  рендера, до `endFrame`/swap) попадает ровно в этот кадр. События кадра -
  всё, что поток GUI отметил до `beforeSynchronizing`: в threaded render loop
  поток GUI на синхронизации стоит.
- **Живой прогон в песочнице** (Sway, окно 200×120, `UQ_FRAME_LOG=1`):
  привязка `wp_presentation`, отдельная очередь, заказ и разбор ответа
  работают - пришёл `discarded` за кадр, который композитор не показал (окно
  было на невидимом рабочем столе, поэтому `presented` там не проверен). Это
  остаётся ручным шагом 2.14.

- [x] 2.1 **Падающий тест: метка события в шкале `nowNs()`.** Создать `tests/tst_framestats.cpp`:
```cpp
// ink-beauty-pass, шаг 0 (design.md, D1): честная латентность. Метка события
// переводится в шкалу FrameStats::nowNs(), задержка до показа сводится по
// кадрам. Протокол wp_presentation здесь не участвует - его проверяет ручной
// шаг на устройстве (PERFORMANCE.md); тут только арифметика, без дисплея.

#include <QTest>

#include <cstdint>

#include "perf/framestats.h"

namespace {

constexpr qint64 kMs = 1'000'000;

// Метка события так, как её отдаёт QInputEvent::timestamp() на Wayland:
// миллисекунды CLOCK_MONOTONIC, усечённые до 32 бит, в секундах.
qreal waylandStampSeconds(qint64 monotonicMs)
{
    return qreal(uint32_t(monotonicMs)) / 1000.0;
}

} // namespace

class TestFrameStats : public QObject
{
    Q_OBJECT

private slots:
    void eventStampMapsToMonotonicClock()
    {
        const qint64 now = 5'000'123 * kMs + 456'789; // 5000 с аптайма
        const qint64 mapped =
            FrameStats::eventTimeToNs(waylandStampSeconds(5'000'123 - 7), now);
        QCOMPARE(mapped, (5'000'123 - 7) * kMs);
    }

    void eventStampSurvivesWrapAround()
    {
        // Младшие 32 бита часов уже перевалили через ноль, а метка - ещё нет.
        const qint64 nowMs = (qint64(1) << 32) + 10;
        const qint64 mapped =
            FrameStats::eventTimeToNs(waylandStampSeconds(nowMs - 15), nowMs * kMs);
        QCOMPARE(mapped, (nowMs - 15) * kMs);
    }

    void foreignStampsAreRejected_data()
    {
        QTest::addColumn<qint64>("ageMs");
        QTest::newRow("из будущего") << qint64(-50);
        QTest::newRow("старше 5 с") << qint64(60'000);
        QTest::newRow("часы QTest с нуля") << qint64(5'000'123 - 1'000);
    }

    void foreignStampsAreRejected()
    {
        QFETCH(qint64, ageMs);
        const qint64 nowMs = 5'000'123;
        QCOMPARE(FrameStats::eventTimeToNs(waylandStampSeconds(nowMs - ageMs), nowMs * kMs),
                 qint64(-1));
    }

    void missingStampIsNotForeign()
    {
        QCOMPARE(FrameStats::eventTimeToNs(-1.0, 5'000'123 * kMs), qint64(-1));

        FrameStats stats;
        stats.markInput();
        stats.markInput(-1.0);
        QCOMPARE(stats.foreignInputStamps(), 0);
    }

    void markInputCountsForeignStamps()
    {
        FrameStats stats;
        const qint64 nowMs = FrameStats::nowNs() / kMs;
        stats.markInput(waylandStampSeconds(nowMs - 3));
        QCOMPARE(stats.foreignInputStamps(), 0);
        stats.markInput(waylandStampSeconds(nowMs - 60'000));
        QCOMPARE(stats.foreignInputStamps(), 1);
        QVERIFY(stats.summary().contains(QStringLiteral("foreign stamps 1")));
    }

};

QTEST_MAIN(TestFrameStats)
#include "tst_framestats.moc"
```
  В `tests/CMakeLists.txt` после строки `uq_add_test(tst_idlepower tst_idlepower.cpp)` добавить:
```cmake
uq_add_test(tst_framestats tst_framestats.cpp)
```

- [x] 2.2 **Прогнать - ожидаемый провал.**
```sh
cmake --build build -j$(nproc) --target tst_framestats
```
  Ожидание: ошибка компиляции `tst_framestats.cpp`: у `FrameStats` нет членов `eventTimeToNs` и `foreignInputStamps`, `markInput` не принимает аргумент.

- [x] 2.3 **`FrameStats`: метка события, счётчик чужих меток.** В `src/perf/framestats.h`:
  - в шапочном комментарии класса после строки `//   * латентность от события ввода до кадра, в котором оно стало видимым.` добавить:
```cpp
//     Отсчёт идёт от метки времени события (QInputEvent::timestamp()), а не
//     от момента обработки: иначе в замер не попадает доставка события от
//     композитора (design.md, D1).
```
  - блок от `qreal maxLatencyMs() const` до `static qint64 nowNs();` заменить на:
```cpp
    qreal maxLatencyMs() const { return m_maxLatencyMs; }
    // Сколько меток событий оказалось не в шкале CLOCK_MONOTONIC: для них
    // латентность считалась от момента обработки. Не ноль на устройстве -
    // замер занижен, и это видно в сводке.
    int foreignInputStamps() const { return m_foreignInputStamps; }
    QString summary() const;

    // Помечает событие ввода. eventTimeSeconds - метка события в секундах той
    // же шкалы, что QInputEvent::timestamp() / 1000; отрицательная - метки нет,
    // и отсчёт идёт от момента вызова. Латентность закрывается в следующем
    // показанном кадре.
    Q_INVOKABLE void markInput(qreal eventTimeSeconds = -1.0);
    Q_INVOKABLE void reset();

    // Монотонные наносекунды от общего для всего приложения начала отсчёта.
    static qint64 nowNs();
    // Метка события -> наносекунды шкалы nowNs(); -1, если метка не из этой
    // шкалы. На Wayland timestamp() - поле time протокола: миллисекунды
    // CLOCK_MONOTONIC композитора, усечённые до 32 бит (qwaylandtabletv2.cpp,
    // zwp_tablet_tool_v2_frame). Поэтому сравниваются младшие 32 бита
    // миллисекунд nowNs(), а разница берётся со знаком - так переживается
    // переполнение раз в 49.7 суток. Метка правдоподобна, если событие не из
    // будущего (с запасом 2 мс на округление) и не старше 5 с.
    static qint64 eventTimeToNs(qreal eventTimeSeconds, qint64 nowNs);
```
  - последнюю строку членов `QTimer *m_logTimer = nullptr;` дополнить:
```cpp
    QTimer *m_logTimer = nullptr;

    int m_foreignInputStamps = 0;
```
  В `src/perf/framestats.cpp`:
  - блок включений и анонимного пространства заменить на:
```cpp
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace {
constexpr int kMaxSamples = 4096;
// Окно правдоподобия метки события относительно момента обработки, мс.
constexpr qint64 kStampFutureToleranceMs = 2;
constexpr qint64 kStampMaxAgeMs = 5000;
}
```
  - в `FrameStats::reset()` после `m_frameCount = m_longFrameCount = m_idleGapCount = 0;` добавить `m_foreignInputStamps = 0;`;
  - функцию `void FrameStats::markInput()` целиком заменить на:
```cpp
qint64 FrameStats::eventTimeToNs(qreal eventTimeSeconds, qint64 nowNs)
{
    if (eventTimeSeconds < 0.0 || nowNs <= 0)
        return -1;
    const qint64 nowMs = nowNs / 1'000'000;
    const auto stampMs32 = uint32_t(std::llround(eventTimeSeconds * 1000.0) & 0xffffffffLL);
    // Разница по модулю 2^32 со знаком: метка из прошлого даёт положительный
    // возраст и тогда, когда младшие биты часов уже перевалили через ноль.
    const qint64 ageMs = qint64(int32_t(uint32_t(nowMs) - stampMs32));
    if (ageMs < -kStampFutureToleranceMs || ageMs > kStampMaxAgeMs)
        return -1;
    return (nowMs - ageMs) * 1'000'000;
}

void FrameStats::markInput(qreal eventTimeSeconds)
{
    if (!m_active)
        return;
    const qint64 now = nowNs();
    qint64 input = eventTimeToNs(eventTimeSeconds, now);
    if (input < 0) {
        // Метка не из CLOCK_MONOTONIC (другой композитор, синтетика тестов) -
        // остаётся момент обработки, а счётчик показывает, что замер занижен.
        if (eventTimeSeconds >= 0.0)
            ++m_foreignInputStamps;
        input = now;
    }
    // Держим самое раннее ещё не показанное событие: латентность считается по
    // нему, иначе поток промежуточных событий занижал бы результат.
    qint64 expected = 0;
    m_pendingInputNs.compare_exchange_strong(expected, input, std::memory_order_relaxed);
}
```
  - в `FrameStats::summary()` строку `" | latency last %8 max %9 ms")` заменить на `" | latency (event stamp -> swap) last %8 max %9 ms | foreign stamps %11")`, а последний `.arg(m_idleGapCount);` - на:
```cpp
        .arg(m_idleGapCount)
        .arg(m_foreignInputStamps);
```

- [x] 2.4 **Прогнать - ожидаемый проход.**
```sh
cmake --build build -j$(nproc) --target tst_framestats && ctest --test-dir build -R tst_framestats --output-on-failure
```
  Ожидание: `tst_framestats` Passed, в выводе теста `Totals: 9 passed` (5 функций, у `foreignStampsAreRejected` три строки данных, плюс `initTestCase`/`cleanupTestCase`).

- [x] 2.5 **Падающий тест: холст передаёт метку события в `FrameStats`.** В `tests/tst_canvasink.cpp` добавить включения `#include <QScopeGuard>` и `#include <cstdint>` к блоку `#include <Q...>`, `#include "perf/framestats.h"` - к блоку проектных; после функции `inkPointsTakeTimeFromTheEvent()` добавить:
```cpp
    // ink-beauty-pass, D1: латентность пера отсчитывается от метки события, а
    // не от момента, когда холст его обработал. Метка не из CLOCK_MONOTONIC
    // не теряется молча, а видна в сводке кадров.
    void inputLatencyStartsAtEventStamp()
    {
        FrameStats stats;
        m_canvas.setFrameStats(&stats);
        const auto restore = qScopeGuard([this] {
            m_canvas.cancelInk();
            m_canvas.setFrameStats(nullptr);
        });
        m_canvas.goToPage(0);
        const QPointF from(420, 320);
        const auto send = [this](QEvent::Type type, const QPointF &at, Qt::MouseButton button,
                                 Qt::MouseButtons buttons, quint64 ms) {
            QMouseEvent event(type, at, at, button, buttons, Qt::NoModifier, stylus());
            event.setTimestamp(ms);
            QCoreApplication::sendEvent(&m_canvas, &event);
        };
        // Так метку ставит композитор: миллисекунды CLOCK_MONOTONIC, 32 бита.
        const auto monotonicMs = [] { return quint64(uint32_t(FrameStats::nowNs() / 1'000'000)); };

        send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, monotonicMs());
        send(QEvent::MouseMove, from + QPointF(10, 0), Qt::NoButton, Qt::LeftButton, monotonicMs());
        QCOMPARE(stats.foreignInputStamps(), 0);

        // Метка минутной давности - не та шкала часов.
        send(QEvent::MouseMove, from + QPointF(20, 0), Qt::NoButton, Qt::LeftButton,
             monotonicMs() - 60'000);
        QCOMPARE(stats.foreignInputStamps(), 1);
        send(QEvent::MouseButtonRelease, from + QPointF(20, 0), Qt::LeftButton, Qt::NoButton,
             monotonicMs());
    }
```

- [x] 2.6 **Прогнать - ожидаемый провал.**
```sh
cmake --build build -j$(nproc) --target tst_canvasink && ctest --test-dir build -R tst_canvasink --output-on-failure
```
  Ожидание: `FAIL! : TestCanvasInk::inputLatencyStartsAtEventStamp() Compared values are not the same Actual (stats.foreignInputStamps()): 0 Expected (1): 1` - холст зовёт `markInput()` без метки.

- [x] 2.7 **Холст передаёт метку.** В `src/canvas/documentcanvas.cpp`, в `DocumentCanvas::beginInkAt` (после `m_wet.addPoint(...)`) и в `DocumentCanvas::continueInkAt` (после `m_wet.addPoint(...)`) заменить
```cpp
    if (m_frameStats)
        m_frameStats->markInput();
```
  на
```cpp
    // Латентность - от метки события пера, а не от момента обработки
    // (ink-beauty-pass, D1); без метки (-1) - от момента обработки.
    if (m_frameStats)
        m_frameStats->markInput(timeSeconds);
```
  Вызов в `eraseAt` не трогать: времени события там нет, ластик замеру задержки пера не нужен.

- [x] 2.8 **Прогнать - ожидаемый проход, без регрессий.**
```sh
cmake --build build -j$(nproc) && ctest --test-dir build -R 'tst_(canvasink|framestats|idlepower|wetstroke)' --output-on-failure
```
  Ожидание: все четыре Passed.

- [x] 2.9 **Коммит.**
```sh
git add src/perf/framestats.h src/perf/framestats.cpp src/canvas/documentcanvas.cpp tests/tst_framestats.cpp tests/tst_canvasink.cpp tests/CMakeLists.txt
git commit -F - <<'EOF'
Латентность от метки события пера

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

- [x] 2.10 **Падающий тест: сводка «событие → показ».** В `tests/tst_framestats.cpp` добавить `#include "perf/presentationstats.h"` после `#include "perf/framestats.h"` и перед закрывающей `};` класса `TestFrameStats` вставить:
```cpp
    void frameKeepsOldestAndNewestInput()
    {
        FrameInputs inputs;
        QVERIFY(inputs.isEmpty());
        inputs.add(0); // «метки нет» не считается событием
        QVERIFY(inputs.isEmpty());
        inputs.add(30 * kMs);
        inputs.add(10 * kMs);
        inputs.add(20 * kMs);
        QCOMPARE(inputs.oldestNs, 10 * kMs);
        QCOMPARE(inputs.newestNs, 30 * kMs);
    }

    void summaryGivesMedianAndP95()
    {
        PresentationStats stats;
        // 20 кадров: самое раннее событие кадра на 10..29 мс раньше показа,
        // самое позднее - на 2 мс раньше.
        for (int i = 0; i < 20; ++i) {
            FrameInputs inputs;
            const qint64 presented = 1000 * kMs + i * 8 * kMs;
            inputs.add(presented - (10 + i) * kMs);
            inputs.add(presented - 2 * kMs);
            stats.addPresented(inputs, presented);
        }
        stats.addDiscarded();

        const PresentationSummary summary = stats.takeSummary();
        QCOMPARE(summary.presented, 20);
        QCOMPARE(summary.discarded, 1);
        QCOMPARE(summary.rejected, 0);
        QCOMPARE(summary.oldestMedianMs, 20.0); // отсортированная выборка, элемент 10
        QCOMPARE(summary.oldestP95Ms, 29.0);    // элемент 19
        QCOMPARE(summary.newestMedianMs, 2.0);
        QCOMPARE(summary.maxMs, 29.0);

        // Сводка за секунду: накопленное сброшено.
        QVERIFY(stats.takeSummary().isEmpty());
    }

    void presentationBeforeEventIsAClockMismatch()
    {
        PresentationStats stats;
        FrameInputs inputs;
        inputs.add(50 * kMs);
        stats.addPresented(inputs, 40 * kMs);
        stats.addPresented(FrameInputs(), 60 * kMs); // кадр без ввода не считается

        const PresentationSummary summary = stats.takeSummary();
        QCOMPARE(summary.presented, 0);
        QCOMPARE(summary.rejected, 1);
        QVERIFY(summary.toString().contains(QStringLiteral("clock mismatch 1")));
    }
```
```sh
cmake --build build -j$(nproc) --target tst_framestats
```
  Ожидание: ошибка компиляции - нет `perf/presentationstats.h`.

- [x] 2.11 **Чистая часть: `PresentationStats`.** Создать `src/perf/presentationstats.h`:
```cpp
#pragma once

#include <QString>
#include <QVector>
#include <QtGlobal>

// Задержка «метка события пера -> показ кадра с этим событием» (design.md, D1).
// Здесь только арифметика: какие события вошли в кадр и как свести задержки в
// сводку. Протокол wp_presentation живёт в presentationfeedback.cpp и
// проверяется руками на устройстве; эта часть проверяется tst_framestats без
// дисплея.
//
// Все метки - наносекунды шкалы FrameStats::nowNs() (CLOCK_MONOTONIC).

// События ввода, вошедшие в один кадр. Кадр забирает всё, что поток GUI успел
// обработать до синхронизации сцены, поэтому событий в нём обычно несколько
// (перо 481 Гц, кадр 120 Гц): самое раннее даёт худшую задержку кадра, самое
// позднее - лучшую.
struct FrameInputs
{
    qint64 oldestNs = 0;
    qint64 newestNs = 0;

    bool isEmpty() const { return oldestNs <= 0; }
    void add(qint64 eventNs);
};

struct PresentationSummary
{
    int presented = 0; // кадров с вводом, о показе которых пришёл ответ
    int discarded = 0; // кадров с вводом, которые композитор так и не показал
    int rejected = 0;  // показ раньше события: шкалы часов не совпали
    qreal oldestMedianMs = 0.0;
    qreal oldestP95Ms = 0.0;
    qreal newestMedianMs = 0.0;
    qreal maxMs = 0.0;

    bool isEmpty() const { return presented == 0 && discarded == 0 && rejected == 0; }
    QString toString() const;
};

class PresentationStats
{
public:
    // Кадр с событиями inputs показан на экране в presentedNs.
    void addPresented(const FrameInputs &inputs, qint64 presentedNs);
    void addDiscarded() { ++m_discarded; }
    // Сводка с прошлого вызова; накопленное сбрасывается, чтобы журнал раз в
    // секунду показывал именно эту секунду, а не среднее за весь запуск.
    PresentationSummary takeSummary();

private:
    QVector<qreal> m_oldestMs;
    QVector<qreal> m_newestMs;
    int m_discarded = 0;
    int m_rejected = 0;
};
```
  Создать `src/perf/presentationstats.cpp`:
```cpp
#include "presentationstats.h"

#include <algorithm>

namespace {

// Тот же порядок статистики, что у интервалов кадров в FrameStats: медиана -
// средний элемент отсортированной выборки, p95 - элемент с индексом n·0.95.
qreal sortedAt(const QVector<qreal> &sorted, qreal quantile)
{
    if (sorted.isEmpty())
        return 0.0;
    const qsizetype index = std::min(sorted.size() - 1, qsizetype(sorted.size() * quantile));
    return sorted[index];
}

} // namespace

void FrameInputs::add(qint64 eventNs)
{
    if (eventNs <= 0)
        return;
    if (oldestNs <= 0 || eventNs < oldestNs)
        oldestNs = eventNs;
    newestNs = std::max(newestNs, eventNs);
}

void PresentationStats::addPresented(const FrameInputs &inputs, qint64 presentedNs)
{
    if (inputs.isEmpty())
        return;
    // Показ раньше события физически невозможен: значит, метка события или
    // время показа пришли не в CLOCK_MONOTONIC. Такой кадр не смешивается со
    // сводкой, а считается отдельно - по счётчику видно, что замер врёт.
    if (presentedNs < inputs.newestNs) {
        ++m_rejected;
        return;
    }
    m_oldestMs.append((presentedNs - inputs.oldestNs) / 1e6);
    m_newestMs.append((presentedNs - inputs.newestNs) / 1e6);
}

PresentationSummary PresentationStats::takeSummary()
{
    PresentationSummary summary;
    summary.presented = int(m_oldestMs.size());
    summary.discarded = m_discarded;
    summary.rejected = m_rejected;
    std::sort(m_oldestMs.begin(), m_oldestMs.end());
    std::sort(m_newestMs.begin(), m_newestMs.end());
    summary.oldestMedianMs = sortedAt(m_oldestMs, 0.5);
    summary.oldestP95Ms = sortedAt(m_oldestMs, 0.95);
    summary.newestMedianMs = sortedAt(m_newestMs, 0.5);
    summary.maxMs = m_oldestMs.isEmpty() ? 0.0 : m_oldestMs.constLast();
    m_oldestMs.clear();
    m_newestMs.clear();
    m_discarded = 0;
    m_rejected = 0;
    return summary;
}

QString PresentationSummary::toString() const
{
    return QStringLiteral("event->present: frames %1 | oldest median %2 p95 %3 | newest median %4"
                          " | max %5 ms | discarded %6 | clock mismatch %7")
        .arg(presented)
        .arg(oldestMedianMs, 0, 'f', 1)
        .arg(oldestP95Ms, 0, 'f', 1)
        .arg(newestMedianMs, 0, 'f', 1)
        .arg(maxMs, 0, 'f', 1)
        .arg(discarded)
        .arg(rejected);
}
```
  В `src/CMakeLists.txt`, в `qt_add_qml_module(uqnotes_core ... SOURCES ...)` после `perf/framestats.h` добавить:
```cmake
        perf/presentationstats.cpp
        perf/presentationstats.h
```
```sh
cmake --build build -j$(nproc) --target tst_framestats && ctest --test-dir build -R tst_framestats --output-on-failure
```
  Ожидание: Passed, `Totals: 12 passed`.

- [x] 2.12 **Падающий тест: без `UQ_FRAME_LOG` подписки на показ нет.** В `tests/tst_idlepower.cpp` добавить `#include "perf/presentationfeedback.h"` после `#include "perf/framestats.h"` и после `frameStatsIsSilentWhenNotRequested()`:
```cpp
    // То же требование для задержки до показа (ink-beauty-pass, D1): без
    // UQ_FRAME_LOG подписки на wp_presentation нет вовсе, даже когда кадры
    // идут, - ни объекта протокола, ни своей очереди событий.
    void presentationFeedbackIsNotCreatedWhenNotRequested()
    {
        QVERIFY2(!m_stats->isLogging(),
                 "тест должен идти без UQ_FRAME_LOG, иначе он проверяет другой режим");
        m_canvas->update();
        QTest::qWait(300);
        QVERIFY(!m_stats->findChild<PresentationFeedback *>());
    }
```
```sh
cmake --build build -j$(nproc) --target tst_idlepower
```
  Ожидание: ошибка компиляции - нет `perf/presentationfeedback.h`.

- [x] 2.13 **`PresentationFeedback` и подключение к `FrameStats`.**
  Создать `src/perf/presentationfeedback.h`:
```cpp
#pragma once

#include <QMutex>
#include <QObject>
#include <QSet>

#include <atomic>
#include <cstdint>

#include "presentationstats.h"

class QQuickWindow;
struct wl_display;
struct wl_event_queue;
struct wl_output;
struct wl_registry;
struct wl_surface;
struct wp_presentation;
struct wp_presentation_feedback;

// Задержка «метка события пера -> показ кадра на экране» по протоколу
// wp_presentation (design.md, D1). FrameStats меряет только до frameSwapped,
// а кадр после этого ещё ждёт композитор и сканирование панели; именно этот
// хвост решает, нужен ли предсказатель кончика.
//
// Создаётся только при UQ_FRAME_LOG и только на Wayland (specs/idle-power:
// без явного запроса замер не стоит ничего). На любой другой платформе, при
// композиторе без wp_presentation и в сборке без wayland-client create()
// возвращает nullptr, и FrameStats работает как раньше.
//
// Потоки. noteInput() и dispatchPending() - поток GUI. Кадр собирается на
// потоке рендера: в beforeSynchronizing (поток GUI в этот момент стоит)
// забираются события, вошедшие в кадр, в afterRendering (до swap, то есть до
// wl_surface.commit этого кадра) на них заказывается feedback. Ответы
// композитора приходят на собственную wl_event_queue и разбираются на потоке
// GUI, очередь Qt не трогается.
class PresentationFeedback : public QObject
{
    Q_OBJECT

public:
    // Окно должно быть уже показано: wl_surface появляется вместе с
    // платформенным окном, поэтому FrameStats зовёт create() на первом
    // показанном кадре, а не при setWindow().
    static PresentationFeedback *create(QQuickWindow *window, QObject *parent);
    ~PresentationFeedback() override;

    // Событие с меткой eventNs (FrameStats::nowNs()) обработано и войдёт в
    // ближайший синхронизированный кадр.
    void noteInput(qint64 eventNs);
    // Разбирает ответы, уже прочитанные из сокета потоком событий Qt. Своего
    // нотификатора нет: зовётся из frameSwapped и раз в секунду из журнала.
    void dispatchPending();
    PresentationSummary takeSummary() { return m_stats.takeSummary(); }

private:
    struct Request
    {
        PresentationFeedback *owner = nullptr;
        wp_presentation_feedback *feedback = nullptr;
        FrameInputs inputs;
    };

    PresentationFeedback(QQuickWindow *window, QObject *parent);
    bool init();
    void onBeforeSynchronizing();
    void onAfterRendering();
    void finish(Request *request);

    static void registryGlobal(void *data, wl_registry *registry, uint32_t name,
                               const char *interface, uint32_t version);
    static void registryGlobalRemove(void *data, wl_registry *registry, uint32_t name);
    static void presentationClockId(void *data, wp_presentation *presentation, uint32_t clockId);
    static void feedbackSyncOutput(void *data, wp_presentation_feedback *feedback,
                                   wl_output *output);
    static void feedbackPresented(void *data, wp_presentation_feedback *feedback,
                                  uint32_t secondsHigh, uint32_t secondsLow, uint32_t nanoseconds,
                                  uint32_t refresh, uint32_t sequenceHigh, uint32_t sequenceLow,
                                  uint32_t flags);
    static void feedbackDiscarded(void *data, wp_presentation_feedback *feedback);

    QQuickWindow *m_window = nullptr;
    wl_display *m_display = nullptr;
    wl_display *m_displayWrapper = nullptr;
    wl_event_queue *m_queue = nullptr;
    wl_registry *m_registry = nullptr;
    wp_presentation *m_presentation = nullptr;
    wl_surface *m_surface = nullptr;
    uint32_t m_clockId = 1; // CLOCK_MONOTONIC, пока композитор не сказал иного

    // Поток GUI пишет, поток рендера забирает на синхронизации.
    std::atomic<qint64> m_pendingOldestNs{0};
    std::atomic<qint64> m_pendingNewestNs{0};
    // Только поток рендера: от синхронизации до afterRendering того же кадра.
    FrameInputs m_frameInputs;

    QMutex m_mutex; // m_requests: создаются на потоке рендера, гаснут на GUI
    QSet<Request *> m_requests;
    PresentationStats m_stats; // только поток GUI
};
```
  Создать `src/perf/presentationfeedback.cpp` (в определениях колбэков `struct wp_presentation_feedback *` пишется с `struct`: в сгенерированном заголовке есть одноимённая функция-запрос, и без `struct` имя внутри класса означает её):
```cpp
#include "presentationfeedback.h"

#include <QGuiApplication>
#include <QMutexLocker>
#include <QQuickWindow>

#ifdef UQ_HAVE_WP_PRESENTATION
#include <qpa/qplatformnativeinterface.h>

#include <wayland-client.h>

#include "presentation-time-client-protocol.h"

#include <cstring>
#include <ctime>
#endif

namespace {

#ifdef UQ_HAVE_WP_PRESENTATION
qint64 clockNs(clockid_t clock)
{
    timespec ts{};
    clock_gettime(clock, &ts);
    return qint64(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

// Время показа приходит в часах, которые назвал композитор (clock_id). KWin,
// Mutter и Weston называют CLOCK_MONOTONIC - ту же шкалу, что у
// FrameStats::nowNs() (std::chrono::steady_clock в libstdc++). Для любых других
// часов разница шкал берётся в момент ответа: ошибка - доли микросекунды.
qint64 toMonotonicNs(qint64 ns, uint32_t clockId)
{
    if (clockId == uint32_t(CLOCK_MONOTONIC))
        return ns;
    return ns - clockNs(clockid_t(clockId)) + clockNs(CLOCK_MONOTONIC);
}
#endif

} // namespace

PresentationFeedback *PresentationFeedback::create(QQuickWindow *window, QObject *parent)
{
#ifdef UQ_HAVE_WP_PRESENTATION
    if (!window || !QGuiApplication::platformName().startsWith(QLatin1String("wayland")))
        return nullptr;
    auto *feedback = new PresentationFeedback(window, parent);
    if (!feedback->init()) {
        delete feedback;
        return nullptr;
    }
    return feedback;
#else
    Q_UNUSED(window);
    Q_UNUSED(parent);
    return nullptr;
#endif
}

PresentationFeedback::PresentationFeedback(QQuickWindow *window, QObject *parent)
    : QObject(parent)
    , m_window(window)
{
}

PresentationFeedback::~PresentationFeedback()
{
    // Поток рендера к этому моменту уже не зовёт нас: FrameStats уничтожается
    // вместе с окном после остановки рендера или удаляет нас сам при смене окна,
    // а отключение ниже снимает прямые соединения.
    if (m_window)
        m_window->disconnect(this);
#ifdef UQ_HAVE_WP_PRESENTATION
    {
        QMutexLocker lock(&m_mutex);
        for (Request *request : std::as_const(m_requests)) {
            wp_presentation_feedback_destroy(request->feedback);
            delete request;
        }
        m_requests.clear();
    }
    if (m_presentation)
        wp_presentation_destroy(m_presentation);
    if (m_registry)
        wl_registry_destroy(m_registry);
    if (m_displayWrapper)
        wl_proxy_wrapper_destroy(m_displayWrapper);
    if (m_queue)
        wl_event_queue_destroy(m_queue);
#endif
}

bool PresentationFeedback::init()
{
#ifdef UQ_HAVE_WP_PRESENTATION
    auto *wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    if (!wayland || !wayland->display())
        return false;
    m_display = wayland->display();
    QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
    m_surface = native ? static_cast<wl_surface *>(
                    native->nativeResourceForWindow(QByteArrayLiteral("surface"), m_window))
                       : nullptr;
    if (!m_surface)
        return false;

    // Своя очередь: ответы композитора не попадают в очередь Qt и разбираются
    // тогда, когда нам удобно, а не на потоке событий Qt.
    m_queue = wl_display_create_queue(m_display);
    m_displayWrapper = static_cast<wl_display *>(wl_proxy_create_wrapper(m_display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(m_displayWrapper), m_queue);
    m_registry = wl_display_get_registry(m_displayWrapper);
    static const wl_registry_listener registryListener = {
        &PresentationFeedback::registryGlobal,
        &PresentationFeedback::registryGlobalRemove,
    };
    wl_registry_add_listener(m_registry, &registryListener, this);
    if (wl_display_roundtrip_queue(m_display, m_queue) < 0 || !m_presentation)
        return false;

    static const wp_presentation_listener presentationListener = {
        &PresentationFeedback::presentationClockId,
    };
    wp_presentation_add_listener(m_presentation, &presentationListener, this);
    // Второй обмен - за clock_id, который композитор шлёт сразу после bind.
    if (wl_display_roundtrip_queue(m_display, m_queue) < 0)
        return false;

    connect(m_window, &QQuickWindow::beforeSynchronizing, this,
            &PresentationFeedback::onBeforeSynchronizing, Qt::DirectConnection);
    connect(m_window, &QQuickWindow::afterRendering, this,
            &PresentationFeedback::onAfterRendering, Qt::DirectConnection);
    return true;
#else
    return false;
#endif
}

void PresentationFeedback::noteInput(qint64 eventNs)
{
    if (eventNs <= 0)
        return;
    qint64 expected = 0;
    m_pendingOldestNs.compare_exchange_strong(expected, eventNs, std::memory_order_relaxed);
    m_pendingNewestNs.store(eventNs, std::memory_order_relaxed);
}

void PresentationFeedback::onBeforeSynchronizing()
{
    // Поток GUI стоит на синхронизации, поэтому всё, что он успел отметить,
    // войдёт ровно в этот кадр, а отмеченное позже - в следующий.
    m_frameInputs = FrameInputs();
    m_frameInputs.add(m_pendingOldestNs.exchange(0, std::memory_order_relaxed));
    m_frameInputs.add(m_pendingNewestNs.exchange(0, std::memory_order_relaxed));
}

void PresentationFeedback::onAfterRendering()
{
#ifdef UQ_HAVE_WP_PRESENTATION
    // Кадры без ввода не заказываются: их задержка ничего не говорит о пере, а
    // лишний объект протокола на каждый кадр - лишняя работа композитора.
    if (m_frameInputs.isEmpty())
        return;
    auto *request = new Request;
    request->owner = this;
    request->inputs = m_frameInputs;
    m_frameInputs = FrameInputs();
    // feedback относится к ближайшему wl_surface.commit, а его делает
    // eglSwapBuffers сразу после этого сигнала. Ответ придёт не раньше показа
    // кадра, то есть заведомо после add_listener ниже.
    request->feedback = wp_presentation_feedback(m_presentation, m_surface);
    static const wp_presentation_feedback_listener feedbackListener = {
        &PresentationFeedback::feedbackSyncOutput,
        &PresentationFeedback::feedbackPresented,
        &PresentationFeedback::feedbackDiscarded,
    };
    wp_presentation_feedback_add_listener(request->feedback, &feedbackListener, request);
    QMutexLocker lock(&m_mutex);
    m_requests.insert(request);
#endif
}

void PresentationFeedback::dispatchPending()
{
#ifdef UQ_HAVE_WP_PRESENTATION
    if (m_display && m_queue)
        wl_display_dispatch_queue_pending(m_display, m_queue);
#endif
}

void PresentationFeedback::finish(Request *request)
{
#ifdef UQ_HAVE_WP_PRESENTATION
    wp_presentation_feedback_destroy(request->feedback);
    {
        QMutexLocker lock(&m_mutex);
        m_requests.remove(request);
    }
    delete request;
#else
    Q_UNUSED(request);
#endif
}

void PresentationFeedback::registryGlobal(void *data, wl_registry *registry, uint32_t name,
                                          const char *interface, uint32_t version)
{
#ifdef UQ_HAVE_WP_PRESENTATION
    auto *self = static_cast<PresentationFeedback *>(data);
    if (!self->m_presentation && std::strcmp(interface, wp_presentation_interface.name) == 0) {
        // Версии 1 достаточно: presented и discarded в ней уже есть.
        Q_UNUSED(version);
        self->m_presentation = static_cast<wp_presentation *>(
            wl_registry_bind(registry, name, &wp_presentation_interface, 1));
    }
#else
    Q_UNUSED(data);
    Q_UNUSED(registry);
    Q_UNUSED(name);
    Q_UNUSED(interface);
    Q_UNUSED(version);
#endif
}

void PresentationFeedback::registryGlobalRemove(void *, wl_registry *, uint32_t)
{
}

void PresentationFeedback::presentationClockId(void *data, wp_presentation *, uint32_t clockId)
{
    static_cast<PresentationFeedback *>(data)->m_clockId = clockId;
}

void PresentationFeedback::feedbackSyncOutput(void *, struct wp_presentation_feedback *, wl_output *)
{
}

void PresentationFeedback::feedbackPresented(void *data, struct wp_presentation_feedback *,
                                             uint32_t secondsHigh, uint32_t secondsLow,
                                             uint32_t nanoseconds, uint32_t, uint32_t, uint32_t,
                                             uint32_t)
{
#ifdef UQ_HAVE_WP_PRESENTATION
    auto *request = static_cast<Request *>(data);
    PresentationFeedback *self = request->owner;
    const qint64 seconds = qint64((quint64(secondsHigh) << 32) | secondsLow);
    const qint64 presentedNs =
        toMonotonicNs(seconds * 1'000'000'000 + qint64(nanoseconds), self->m_clockId);
    self->m_stats.addPresented(request->inputs, presentedNs);
    self->finish(request);
#else
    Q_UNUSED(data);
    Q_UNUSED(secondsHigh);
    Q_UNUSED(secondsLow);
    Q_UNUSED(nanoseconds);
#endif
}

void PresentationFeedback::feedbackDiscarded(void *data, struct wp_presentation_feedback *)
{
    auto *request = static_cast<Request *>(data);
    request->owner->m_stats.addDiscarded();
    request->owner->finish(request);
}
```
  Корневой `CMakeLists.txt`: строку `project(uq-notes VERSION 0.7.0 LANGUAGES CXX)` заменить на
```cmake
# C - только для кода протокола wp_presentation, который генерирует
# wayland-scanner (src/CMakeLists.txt).
project(uq-notes VERSION 0.7.0 LANGUAGES C CXX)
```
  и после `option(UQ_DIAGNOSTICS ...)` добавить:
```cmake
# Задержка до показа кадра (wp_presentation, ink-beauty-pass D1). Работает
# только при UQ_FRAME_LOG на Wayland; OFF собирает заглушку - так проверяется
# сборка без wayland-client.
option(UQ_WP_PRESENTATION "Build the wp_presentation latency probe if wayland-client is available" ON)
```
  `src/CMakeLists.txt`: в `SOURCES` модуля после строк из 2.11 добавить
```cmake
        perf/presentationfeedback.cpp
        perf/presentationfeedback.h
```
  а после `target_link_libraries(uqnotes_core PUBLIC ... ink_stroke_modeler)` добавить:
```cmake
# Задержка «событие -> показ» по wp_presentation (ink-beauty-pass, D1). Только
# для замера и только на Wayland: без wayland-client, протокола или
# Qt6::GuiPrivate приложение собирается как раньше, а PresentationFeedback
# тихо возвращает nullptr.
pkg_check_modules(WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
pkg_get_variable(UQ_WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
pkg_get_variable(UQ_WAYLAND_SCANNER wayland-scanner wayland_scanner)
find_package(Qt6 QUIET COMPONENTS GuiPrivate)
set(UQ_PRESENTATION_XML
    "${UQ_WAYLAND_PROTOCOLS_DIR}/stable/presentation-time/presentation-time.xml")
if(UQ_WP_PRESENTATION AND WAYLAND_CLIENT_FOUND AND UQ_WAYLAND_SCANNER
   AND EXISTS "${UQ_PRESENTATION_XML}" AND TARGET Qt6::GuiPrivate)
    set(UQ_WAYLAND_GEN ${CMAKE_CURRENT_BINARY_DIR}/wayland)
    file(MAKE_DIRECTORY ${UQ_WAYLAND_GEN})
    add_custom_command(
        OUTPUT ${UQ_WAYLAND_GEN}/presentation-time-client-protocol.h
               ${UQ_WAYLAND_GEN}/presentation-time-protocol.c
        COMMAND ${UQ_WAYLAND_SCANNER} client-header ${UQ_PRESENTATION_XML}
                ${UQ_WAYLAND_GEN}/presentation-time-client-protocol.h
        COMMAND ${UQ_WAYLAND_SCANNER} private-code ${UQ_PRESENTATION_XML}
                ${UQ_WAYLAND_GEN}/presentation-time-protocol.c
        DEPENDS ${UQ_PRESENTATION_XML}
        COMMENT "Generating wp_presentation client code"
        VERBATIM)
    target_sources(uqnotes_core PRIVATE
        ${UQ_WAYLAND_GEN}/presentation-time-client-protocol.h
        ${UQ_WAYLAND_GEN}/presentation-time-protocol.c)
    target_include_directories(uqnotes_core PRIVATE ${UQ_WAYLAND_GEN})
    target_link_libraries(uqnotes_core PRIVATE PkgConfig::WAYLAND_CLIENT Qt6::GuiPrivate)
    target_compile_definitions(uqnotes_core PRIVATE UQ_HAVE_WP_PRESENTATION)
    message(STATUS "uq-notes: замер задержки до показа (wp_presentation) включён")
endif()
```
  `src/perf/framestats.h`:
  - после `#include <atomic>` добавить пустую строку и `class PresentationFeedback;`;
  - в шапочном комментарии после строк из 2.3 добавить:
```cpp
//
// При UQ_FRAME_LOG на Wayland к этому добавляется задержка до показа кадра на
// экране (PresentationFeedback): frameSwapped - ещё не фотон.
```
  - после `int m_foreignInputStamps = 0;` добавить:
```cpp
    // Задержка до показа: только при UQ_FRAME_LOG, создаётся на первом
    // показанном кадре, когда у окна уже есть wl_surface.
    PresentationFeedback *m_presentation = nullptr;
    bool m_presentationTried = false;
```
  `src/perf/framestats.cpp`:
  - после `#include <cstdint>` добавить пустую строку и `#include "presentationfeedback.h"`;
  - в конструкторе соединение таймера журнала заменить на:
```cpp
        connect(m_logTimer, &QTimer::timeout, this, [this] {
            QTextStream(stdout) << "[frames] " << summary() << Qt::endl;
            if (m_presentation) {
                m_presentation->dispatchPending();
                const PresentationSummary present = m_presentation->takeSummary();
                if (!present.isEmpty())
                    QTextStream(stdout) << "[present] " << present.toString() << Qt::endl;
            }
        });
```
  - в `setWindow` после `if (m_window) m_window->disconnect(this);` добавить:
```cpp
    // Подписка на показ привязана к wl_surface прежнего окна.
    delete m_presentation;
    m_presentation = nullptr;
    m_presentationTried = false;
```
  - в `markInput` после `m_pendingInputNs.compare_exchange_strong(...)` добавить:
```cpp
    if (m_presentation)
        m_presentation->noteInput(input);
```
  - в `onFrameSwapped` после `const qint64 now = nowNs();` добавить:
```cpp
    if (m_logTimer && !m_presentationTried && m_window) {
        // Первый показанный кадр: wl_surface у окна уже есть.
        m_presentationTried = true;
        m_presentation = PresentationFeedback::create(m_window, this);
    }
    if (m_presentation)
        m_presentation->dispatchPending();
```

- [x] 2.14 **Прогнать - сборка с протоколом и без, тесты зелёные.**
```sh
cmake -S . -B build 2>&1 | grep 'wp_presentation'   # ожидание: "uq-notes: замер задержки до показа (wp_presentation) включён"
cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure
```
  Ожидание: всё зелёное (`tst_idleframes` без сессии - SKIP, как раньше), в том числе `tst_framestats`, `tst_idlepower` (новая функция), `tst_canvasink`.
```sh
cmake -S . -B build-nowp -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUQ_WP_PRESENTATION=OFF -DUQ_BUILD_TESTS=OFF
cmake --build build-nowp -j$(nproc) --target uq-notes
```
  Ожидание: без строки про wp_presentation, сборка без ошибок (заглушка: `create()` возвращает `nullptr`). `build-nowp/` попадает под `build-*/` в `.gitignore`.

- [ ] 2.15 **Ручной шаг на устройстве (Galaxy Book, KDE/kwin_wayland): задержка до показа.**
```sh
UQ_FRAME_LOG=1 ./build/src/uq-notes большой.pdf
```
  Писать пером 30-60 секунд, затем закрыть. Ожидание в выводе раз в секунду во время письма: строка `[frames] ... | latency (event stamp -> swap) last X max Y ms | foreign stamps 0` и следом `[present] event->present: frames N | oldest median A p95 B | newest median C | max D ms | discarded E | clock mismatch 0` с N около 100-120 (кадры с вводом за секунду при 120 Гц). Ориентир по стенду: недогон 9.5 px ≈ 23 мс до фотона, то есть `oldest median` порядка 15-35 мс; `latency (event stamp -> swap)` выше прежних 0.8-1.8 мс на доставку от композитора. В простое строк `[present]` нет (кадров с вводом нет).
  Если `[present]` не появляется вовсе: `WAYLAND_DEBUG=client UQ_FRAME_LOG=1 ./build/src/uq-notes большой.pdf 2>&1 | grep -m5 -E 'wp_presentation(_feedback)?[@#]'` - должны быть `wp_presentation@N.feedback(...)` и `wp_presentation_feedback@M.presented(...)` (libwayland 1.26 пишет `#` вместо `@`); нет `wp_presentation` в реестре - композитор протокол не даёт (тогда замер невозможен, записать в PERFORMANCE.md). `foreign stamps` больше нуля или `clock mismatch` больше нуля - метки или время показа не в `CLOCK_MONOTONIC`; числа этого прогона не годятся, записать как наблюдение. `UQ_DEMO_INK` для этого замера не годится: сценарный ввод идёт без меток, и задержка считается от обработки.
  Записать медиану, p95 и максимум за 3 прогона в таблицу PERFORMANCE.md (2.16).

- [x] 2.16 **PERFORMANCE.md: как теперь меряется.**
  - В «Как воспроизвести» после абзаца про `UQ_FRAME_LOG` добавить:
```markdown
На Wayland к сводке раз в секунду добавляется строка `[present]` - задержка
от метки события пера до показа кадра с этим событием на экране (раздел «Как
меряется латентность» ниже). Сценарный ввод `UQ_DEMO_INK` идёт без меток
времени, поэтому для неё задержка считается от момента обработки; настоящие
числа - только при письме пером.
```
  - Сразу после таблицы «Латентность — бюджет держится с запасом» (перед абзацем про автосохранение) добавить:
```markdown
Эти числа сняты до ink-beauty-pass: отсчёт шёл от момента, когда холст
обработал событие, до `frameSwapped`. Доставка события от композитора и
путь кадра после swap в них не входят, поэтому для решения о предсказателе
кончика они не годятся. Как меряется теперь - в разделе «Как меряется
латентность».
```
  - Перед разделом «## Что оказалось узким местом» добавить раздел:
```markdown
## Как меряется латентность

Два числа, оба только при `UQ_FRAME_LOG`.

**`[frames] ... latency (event stamp -> swap)`** - от метки времени события
(`QInputEvent::timestamp()`) до `frameSwapped` кадра, в котором событие
отрисовано. На Wayland метка - поле `time` протокола (`zwp_tablet_tool_v2.frame`):
миллисекунды `CLOCK_MONOTONIC` композитора, усечённые до 32 бит.
`FrameStats::eventTimeToNs` переводит её в шкалу `FrameStats::nowNs()`
(`steady_clock`, тот же `CLOCK_MONOTONIC`) по младшим 32 битам, с учётом
переполнения. Метка, которая не укладывается в окно от −2 мс до 5 с
относительно момента обработки, считается чужой: задержка тогда идёт от
обработки, а счётчик `foreign stamps` растёт. На устройстве он обязан быть
0, иначе число занижено. Метка округлена вниз до миллисекунды - задержка
завышена не больше чем на 1 мс.

**`[present] event->present`** - от метки события до момента, когда
композитор показал кадр с этим событием (`wp_presentation_feedback.presented`,
`src/perf/presentationfeedback.cpp`). Кадр забирает все события, обработанные
до синхронизации сцены; `oldest` - самое раннее из них (худшая задержка
кадра), `newest` - самое позднее. Медиана и p95 - за последнюю секунду.
`discarded` - кадры с вводом, которые композитор так и не показал;
`clock mismatch` - ответы, где показ раньше события (часы не совпали), они в
медиану не входят и на устройстве должны быть 0. Подписка идёт на своей
`wl_event_queue`, очередь Qt не трогается; без `UQ_FRAME_LOG` и не на
Wayland её нет вовсе (`specs/idle-power`).

Не входит ни в одно из чисел: задержка дигитайзера пера до libinput и
развёртка панели после момента показа.

| Показатель (письмо пером, 200 страниц) | Значение |
| --- | --- |
| событие → swap, типичная / максимум | не замерено |
| событие → показ, медиана `oldest` | не замерено |
| событие → показ, p95 `oldest` | не замерено |
| событие → показ, медиана `newest` | не замерено |
```
    «Не замерено» заменяется числами из 2.15. Если замера на устройстве в этом шаге нет, таблица остаётся с «не замерено», и это явно сказано в сообщении коммита.

- [x] 2.17 **Финальный коммит шага 0.**
```sh
git add CMakeLists.txt src/CMakeLists.txt src/perf/ tests/tst_framestats.cpp tests/tst_idlepower.cpp PERFORMANCE.md
git status --short   # ожидание: ничего неподготовленного, кроме .claude/worktrees/
git commit -F - <<'EOF'
Стенд stroke-lab и замер задержки до показа

Решения ink-beauty-pass принимаются по числам, и этот коммит даёт оба
источника чисел. Офлайн-стенд stroke-lab теперь в основном дереве вместе с
метриками всех прототипов, и baseline на нём воспроизводит design.md:
отслеживание толщины 0.163, заход 36 px, тупой конец 0.95, недогон кончика
9.5 px, ошибка кромки 27.6 из 255. Каждый следующий шаг проверяется теми же
командами из tools/stroke-lab/README.md.

Прежняя латентность 0.8-1.8 мс мерила только путь от обработки события до
swap. Теперь отсчёт идёт от метки события пера, то есть с доставкой от
композитора, а при UQ_FRAME_LOG на Wayland журнал раз в секунду печатает
задержку до настоящего показа кадра по wp_presentation: медиану и p95 от
самого раннего события кадра. Подписка живёт на своей очереди событий и без
UQ_FRAME_LOG не создаётся, поэтому покой в простое не задет; метки не из
CLOCK_MONOTONIC и ответы с несовпавшими часами считаются отдельно, а не
портят медиану.

Чистая часть - перевод метки и сводка по кадрам - покрыта tst_framestats,
протокол проверяется руками на устройстве (PERFORMANCE.md).

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```
  Если в 2.15 замера не было, перед строкой Co-Authored-By добавить абзац: «Чисел с устройства в этом коммите нет: таблица задержки до показа в PERFORMANCE.md ждёт прогона на Galaxy Book».

## Чего раздел не покрывает

- **Сборка приложения и ctest в песочнице не выполнялись**: на машине нет
  `libqpdf` (`pkg-config libqpdf` не находит, `pacman -Q qpdf` - нет пакета),
  поэтому `uqnotes_core` не конфигурируется. Проверено отдельно: стенд из
  корня собран с подставным `libqpdf.pc` (цели стенда qpdf не нужны), код
  группы 2 собран в отдельном проекте с той же генерацией протокола и в
  варианте без неё, `tst_framestats` прошёл (12/12). Изменения
  `tst_canvasink`, `tst_idlepower`, `documentcanvas.cpp` и `src/CMakeLists.txt`
  в составе `uqnotes_core` не компилировались.
- `presented` от композитора живьём не получен (в песочнице окно не было
  видно, пришёл только `discarded`); проверка - ручной шаг 2.15.
- `aa-gpu-bench` и варианты прототипов переносятся в шагах 1-3 (таблица 1.20).
- Запись трасс с устройства (`input_probe --dump` из итогового отчёта) и
  проверка сглаживания libinput у пера 2D1F:0230 не входят в D1 и в этот
  раздел; эмуляция `--libinput-smoothing` есть.
- Задержка ластика (`eraseAt`) по-прежнему считается от обработки.


# Шаг 1: давление и закон толщины

## 3. Давление без запаздывания (D2)

Цель: толщина следует за нажимом. Сценарии ink-input «Уверенное начало
штриха», «Конец штриха по спаду нажима», требование «Штрих пера с учётом
нажатия». Три правки в `StrokeSampler` (design.md, D2): давление узла по
времени узла, One Euro давления 12 Гц / beta 0.5, на отрыве отфильтрованное
давление. Траектория не меняется ни в одном бите.

Проверено до плана: правки и новые тесты собраны отдельным проектом во
scratchpad против продуктовых `src/ink/*.cpp` (полный проект в среде плана не
собирается: нет `libqpdf`). Новые эталоны падают на текущем коде с указанными
ниже сообщениями и проходят после правок; все 36 прежних проверок
`tst_strokesampler` и все прежние `tst_wetstroke` проходят и после правок.

**Files:**
- Modify: `src/ink/strokesampler.h`, `src/ink/strokesampler.cpp`
- Test: `tests/tst_strokesampler.cpp`

- [x] 3.1 **Открыть фабрику фильтра давления, поведение не меняется.** Тест
  «давление узла по времени узла» сверяет давление точек с отфильтрованным
  входом и должен строить фильтр с теми же параметрами, что и сэмплер, не
  повторяя констант.

  `src/ink/strokesampler.h`, публичная часть класса, сразу после строки
  `bool isEmpty() const { return m_points.isEmpty(); }`, вставить:

  ```cpp

      // Фильтр давления с параметрами сэмплера. Открыт для эталонов
      // tst_strokesampler: давление точки сверяется с отфильтрованным давлением
      // пера в момент точки (ink-beauty-pass, design.md, D2).
      static OneEuroFilter makePressureFilter();
  ```

  `src/ink/strokesampler.cpp`, конструктор: строку
  `    , m_pressureFilter(kPressureMinCutoff, kPressureBeta, kPressureDerivativeCutoff)`
  заменить на

  ```cpp
      , m_pressureFilter(makePressureFilter())
  ```

  и сразу после `StrokeSampler::~StrokeSampler() = default;` вставить:

  ```cpp

  OneEuroFilter StrokeSampler::makePressureFilter()
  {
      return OneEuroFilter(kPressureMinCutoff, kPressureBeta, kPressureDerivativeCutoff);
  }
  ```

- [x] 3.2 **Прогон: поведение не изменилось.**
  `cmake --build build -j$(nproc) && ctest --test-dir build -R '^tst_strokesampler$' --output-on-failure`
  Ожидается: `100% tests passed`.

- [x] 3.3 **Помощники для эталонов давления в `tests/tst_strokesampler.cpp`.**
  После `#include <QTest>` добавить:

  ```cpp

  #include <limits>
  ```

  В секцию `private:` класса `TestStrokeSampler`, сразу после функции
  `run(...)` (перед `private slots:`), вставить:

  ```cpp

      // Прямая слева направо со скоростью speed (px/с) на частоте пера целевого
      // устройства; давление - функция времени от касания. Время событий - не от
      // нуля, как у настоящего пера.
      template <typename Pressure>
      static QList<InkPoint> lineWithPressure(qreal speed, qreal duration, Pressure pressure)
      {
          QList<InkPoint> input;
          const int count = int(std::lround(duration * kDeviceHz));
          for (int k = 0; k <= count; ++k) {
              const qreal t = k / kDeviceHz;
              input.append({QPointF(40.0 + speed * t, 100.0), pressure(t), 3.0 + t});
          }
          return input;
      }

      static void feed(StrokeSampler &sampler, const QList<InkPoint> &input)
      {
          for (const InkPoint &point : input)
              sampler.append(point);
          sampler.finish();
      }
  ```

- [x] 3.4 **Эталон «на нарастающем нажиме давление осевой отстаёт не больше
  чем на 15 мс».** Вставить в `private slots:` перед `void resetClearsEverything()`:

  ```cpp
      // --- ink-beauty-pass, D2: давление без запаздывания ---------------------

      // Сценарий «Уверенное начало штриха»: перо ставится и сразу ведёт линию,
      // нажим за 80 мс растёт от 0.05 до 0.65. Запаздывание - сдвиг по времени,
      // при котором входное давление лучше всего совпадает с давлением точек
      // осевой: толщина - функция давления, так что это и запаздывание толщины.
      void pressureRampLagsAtMost15ms()
      {
          const auto pressure = [](qreal t) { return 0.05 + 0.6 * qBound(0.0, t / 0.08, 1.0); };
          const QList<InkPoint> input = lineWithPressure(300.0, 0.3, pressure);
          StrokeSampler sampler;
          feed(sampler, input);

          const qreal origin = input.constFirst().timeSeconds;
          qreal bestShift = 0.0;
          qreal bestError = std::numeric_limits<qreal>::max();
          for (int step = 0; step <= 200; ++step) {
              const qreal shift = step * 0.0005;
              qreal error = 0.0;
              for (const InkPoint &point : sampler.points()) {
                  const qreal t = point.timeSeconds - origin;
                  if (t > 0.15)
                      break;
                  const qreal expected = pressure(qMax(0.0, t - shift));
                  error += (point.pressure - expected) * (point.pressure - expected);
              }
              if (error < bestError) {
                  bestError = error;
                  bestShift = shift;
              }
          }
          QVERIFY2(bestShift <= 0.015,
                   qPrintable(QStringLiteral("давление отстаёт на %1 мс").arg(bestShift * 1000.0)));
      }

  ```

- [x] 3.5 **Эталон «конец без скачка».** Вставить следом:

  ```cpp
      // Сценарий «Конец штриха по спаду нажима»: нажим 0.6 плавно спадает до 0.2
      // за 100 мс перед отрывом. Скачок на последней точке - не больше обычного
      // шага этого спада с запасом в четверть: 4/с · 4 мс на шаг нарезки (1.2 px
      // при 300 px/с) = 0.016, порог 0.02. Сырое давление на отрыве при
      // отфильтрованном у остальных точек даёт скачок 0.25 при прежнем фильтре
      // (на стенде - 56% толщины) и 0.044 даже при новом: он отстаёт от пера на
      // свои 11 мс, а сырое - нет.
      void strokeEndFollowsFallingPressureWithoutJump()
      {
          const auto pressure = [](qreal t) {
              return t < 0.2 ? 0.6 : 0.6 - 0.4 * qMin(1.0, (t - 0.2) / 0.1);
          };
          StrokeSampler sampler;
          feed(sampler, lineWithPressure(300.0, 0.3, pressure));

          const QList<InkPoint> &points = sampler.points();
          QVERIFY(points.size() > 10);
          const qreal last = points.constLast().pressure;
          const qreal previous = points[points.size() - 2].pressure;
          QVERIFY2(qAbs(last - previous) <= 0.02,
                   qPrintable(QStringLiteral("скачок давления на последней точке: %1 -> %2")
                                  .arg(previous)
                                  .arg(last)));
          // И линия сошла вслед за нажимом, а не осталась толстой: на отрыве
          // давление 0.2, отставание на 15 мс при спаде 4/с - ещё 0.06.
          QVERIFY2(previous <= 0.2 + 0.06 + 0.01,
                   qPrintable(QStringLiteral("к отрыву давление осталось %1").arg(previous)));
      }

  ```

  Почему X = 0.02, а не относительный порог: у конца давление мало, и
  относительный скачок там растёт сам собой. Порог привязан к честному шагу
  этого спада (0.016) с запасом 25%. На новом коде скачок 0.0135, на текущем -
  0.249 (в 12 раз больше порога); при быстром фильтре, но сыром давлении на
  отрыве - 0.044 (в 2 раза больше), так что эталон ловит каждую из правок D2
  отдельно.

- [x] 3.6 **Эталон «давление узла по времени узла».** Вставить следом:

  ```cpp
      // Давление точки - давление пера в момент этой точки, а не проекция
      // отстающей пружины модели. На быстрой прямой положение узла - перо в момент
      // узла, и давление обязано быть оттуда же: это отфильтрованное давление
      // входа, линейно по времени между отсчётами. Последняя точка - тоже.
      void pointPressureIsPenPressureAtPointTime()
      {
          const QList<InkPoint> input =
              lineWithPressure(600.0, 0.3, [](qreal t) { return 0.2 + 2.0 * t; });
          StrokeSampler sampler;
          feed(sampler, input);

          OneEuroFilter filter = StrokeSampler::makePressureFilter();
          const qreal origin = input.constFirst().timeSeconds;
          QList<qreal> filtered;
          for (const InkPoint &point : input)
              filtered.append(qBound(0.0, filter.filter(point.pressure, point.timeSeconds - origin), 1.0));
          const auto filteredAt = [&](qreal time) {
              if (time <= input.constFirst().timeSeconds)
                  return filtered.constFirst();
              for (int i = 1; i < input.size(); ++i) {
                  if (time <= input[i].timeSeconds) {
                      const qreal u = (time - input[i - 1].timeSeconds)
                          / (input[i].timeSeconds - input[i - 1].timeSeconds);
                      return filtered[i - 1] + (filtered[i] - filtered[i - 1]) * u;
                  }
              }
              return filtered.constLast();
          };

          for (int i = 0; i < sampler.points().size(); ++i) {
              const InkPoint &point = sampler.points()[i];
              const qreal expected = filteredAt(point.timeSeconds);
              QVERIFY2(qAbs(point.pressure - expected) < 2e-3,
                       qPrintable(QStringLiteral("точка %1: давление %2, у пера в её момент %3")
                                      .arg(i)
                                      .arg(point.pressure)
                                      .arg(expected)));
          }
      }

  ```

  Допуск 2e-3: на новом коде наибольшее расхождение 0.0009 (нарезка
  интерполирует давление по хорде между точками сплайна), на текущем - 0.105.

- [x] 3.7 **Прогон: три новых эталона падают.**
  `cmake --build build -j$(nproc) && ctest --test-dir build -R '^tst_strokesampler$' --output-on-failure`
  Ожидается `3 failed`:
  - `pressureRampLagsAtMost15ms` - «давление отстаёт на 57.5 мс»;
  - `strokeEndFollowsFallingPressureWithoutJump` - «скачок давления на
    последней точке: 0.450992 -> 0.202495»;
  - `pointPressureIsPenPressureAtPointTime` - «точка 2: давление 0.200111, у
    пера в её момент 0.20243».

- [x] 3.8 **Давление узла по времени узла.** `src/ink/strokesampler.h`, после
  объявления `QPointF rawAt(qreal time) const;` вставить:

  ```cpp
      // Отфильтрованное давление пера в момент time, линейно между соседними
      // отсчётами. Давление узла берётся отсюда, по времени узла, а не с
      // проекции отстающей пружины на ломаную отсчётов: при следовании за пером
      // положение узла - перо в момент t, а давление пружины примерно на 20 мс
      // старше, и спад нажима на отрыве терялся (ink-beauty-pass, design.md, D2).
      qreal pressureAt(qreal time) const;
  ```

  Там же комментарий над `QList<InkPoint> m_rawHistory;` заменить целиком:

  ```cpp
      // Последние отсчёты пера для rawAt и pressureAt: экранные единицы,
      // отфильтрованное давление, время от начала штриха. Узлы модели никогда не
      // бывают старше нескольких отсчётов.
  ```

  `src/ink/strokesampler.cpp`, перед `InkPoint StrokeSampler::followedNode(`
  (после конца `StrokeSampler::rawAt`) вставить:

  ```cpp
  qreal StrokeSampler::pressureAt(qreal time) const
  {
      if (m_rawHistory.isEmpty())
          return -1.0;
      if (time >= m_rawHistory.constLast().timeSeconds)
          return m_rawHistory.constLast().pressure;
      for (qsizetype i = m_rawHistory.size() - 1; i > 0; --i) {
          const InkPoint &a = m_rawHistory[i - 1];
          const InkPoint &b = m_rawHistory[i];
          if (time >= a.timeSeconds) {
              const qreal span = b.timeSeconds - a.timeSeconds;
              const qreal u = span > 0.0 ? (time - a.timeSeconds) / span : 1.0;
              return a.pressure + (b.pressure - a.pressure) * u;
          }
      }
      return m_rawHistory.constFirst().pressure;
  }

  ```

  В `StrokeSampler::followedNode` строку
  `    node.pressure = m_lastInputPressure < 0.0 ? -1.0 : qBound(0.0, qreal(result.pressure), 1.0);`
  заменить на:

  ```cpp
      // Давление - пера в момент узла, а не проекция пружины (см. pressureAt).
      // Догон на отрыве и предсказание уходят за последний отсчёт и получают
      // его давление.
      node.pressure = m_lastInputPressure < 0.0 ? -1.0 : pressureAt(time);
  ```

- [x] 3.9 **Прогон: эталон времени узла держится везде, кроме последней точки.**
  `cmake --build build -j$(nproc) && ctest --test-dir build -R '^tst_strokesampler$' --output-on-failure`
  Ожидается `3 failed`: «давление отстаёт на 38.5 мс»; «скачок давления на
  последней точке: 0.387317 -> 0.202495»; «точка 150: давление 0.798753, у пера
  в её момент 0.693671». Номер 150 - последняя точка: её давление ещё сырое.

- [x] 3.10 **Фильтр давления 12 Гц, beta 0.5.** `src/ink/strokesampler.cpp`,
  блок констант давления в анонимном пространстве имён: строки

  ```cpp
  // Давление сглаживается отдельно: модель лишь проецирует его на траекторию, а
  // скачок давления виден ступенькой толщины.
  constexpr qreal kPressureMinCutoff = 2.5;
  constexpr qreal kPressureBeta = 0.01;
  ```

  заменить на (строка `kPressureDerivativeCutoff = 10.0` остаётся):

  ```cpp
  // Давление сглаживается отдельно от траектории: скачок давления виден
  // ступенькой толщины. Срез 12 Гц (τ ≈ 13 мс) вместо прежних 2.5 Гц (τ ≈ 64 мс):
  // с прежним начало штриха оставалось бледным на 36 px, а конец не успевал
  // сойти по спаду нажима (ink-beauty-pass, design.md, D2). beta 0.5 поднимает
  // срез на быстром нажиме: на заходе 0.05 -> 0.65 за 80 мс он около 15 Гц. Шум
  // давления при этом проходит честно; если на устройстве при ровном нажиме
  // видна рябь толщины, minCutoff снижается до 8 Гц - это одна константа.
  constexpr qreal kPressureMinCutoff = 12.0;
  constexpr qreal kPressureBeta = 0.5;
  ```

- [x] 3.11 **Прогон: заход в норме, конец ещё со скачком.**
  `cmake --build build -j$(nproc) && ctest --test-dir build -R '^tst_strokesampler$' --output-on-failure`
  Ожидается `2 failed`: «скачок давления на последней точке: 0.246002 ->
  0.202495»; «точка 150: давление 0.798753, у пера в её момент 0.780459».
  `pressureRampLagsAtMost15ms` проходит (запаздывание 6.5 мс).

- [x] 3.12 **На отрыве - отфильтрованное давление.** `src/ink/strokesampler.cpp`,
  `StrokeSampler::finish()`: после комментария, который кончается строкой
  `    // штрих из двух точек в одном месте.`, строки

  ```cpp
      InkPoint last = m_points.constLast();
      QList<InkPoint> tail;
      walkTowards(last, m_lastRaw, tail);
      if (distance(last.position, m_lastRaw.position) > kEndSnapDistance)
          tail.append(m_lastRaw);
  ```

  заменить на:

  ```cpp
      //
      // Положение конца - сырое, а давление - отфильтрованное, как у всех точек
      // штриха: сырое давление последнего отсчёта на спаде нажима меньше
      // отфильтрованного, и толщина на последней точке скакала (на стенде - на
      // 56%), оставляя «клюв» (ink-beauty-pass, design.md, D2).
      InkPoint end = m_lastRaw;
      if (end.hasPressure())
          end.pressure = m_lastInputPressure;
      InkPoint last = m_points.constLast();
      QList<InkPoint> tail;
      walkTowards(last, end, tail);
      if (distance(last.position, end.position) > kEndSnapDistance)
          tail.append(end);
  ```

  `m_lastInputPressure` - отфильтрованное и прижатое к 0..1 давление
  последнего принятого отсчёта (или -1 без давления), его уже кладёт `append()`.

- [x] 3.13 **Прогон: всё зелёное.**
  `cmake --build build -j$(nproc) && ctest --test-dir build -R '^tst_(strokesampler|wetstroke|canvasink)$' --output-on-failure`
  Ожидается `100% tests passed, 0 tests failed out of 3`. В выводе
  `tst_strokesampler` - `Totals: 39 passed, 0 failed` (было 36).

- [x] 3.14 **Сверить, что прежние эталоны не завязаны на старое давление.**
  Правок не требуется; проверено прогоном прежних файлов против новых
  исходников:
  - `tst_strokesampler`: позиционные эталоны (`smoothsJitterOnSlowStroke`,
    `fastCircleHasNoFacets`, `smallLoopsKeepTheirSize` и т. д.) давления не
    касаются, траектория не изменилась. `interpolatesPressureAlongResampledPoints`
    требует рост давления больше 0.5 от первой точки к последней - при 12 Гц
    рост только ближе к входному. `keepsMissingPressureAsMissing` - `pressureAt`
    отдаёт -1 из `m_rawHistory`, и узел без давления остаётся без давления.
    `finishEndsExactlyAtLastContactPoint` сравнивает положение, а не давление.
  - `tst_wetstroke`: давление влияет только на толщину, а
    `incrementalResultMatchesFullTessellation` сравнивает мокрую полосу с
    тесселяцией тех же точек - инвариант от фильтра не зависит.
  - `tst_canvasink::burstDeliveryGivesTheSameStroke` сравнивает давление
    точек при равномерной и пакетной доставке на равенство: сэмплер
    детерминирован по последовательности событий, `pressureAt` от способа
    доставки не зависит. `finishedStrokeIsTheSameEverywhere` сравнивает
    нанесённый штрих с ним же после записи и отмены.

  Если какой-то из них всё же упадёт - это расхождение с прототипом
  `exp/rnote` `e2-pressure-caps`; сравнить правки 3.8-3.12 с
  `git show exp/rnote:tools/stroke-lab/variants/rnote_kit.h`
  (`SamplerParams::pressureAtNodeTime`, `endPressureFiltered`), порог эталона не
  трогать.

- [x] 3.15 **Коммит.**

  ```sh
  git add src/ink/strokesampler.h src/ink/strokesampler.cpp tests/tst_strokesampler.cpp
  git commit -F - <<'EOF'
  Давление штриха без запаздывания

  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  EOF
  ```

## 4. Закон толщины хранится со штрихом (D3)

Цель: новые штрихи пера - `W·(0.08 + 0.92p)`, прежние - `W·(0.25 + 0.75p)`,
без давления оба - `W·0.7375`. Закон - поле `Stroke::style`, переносится через
мокрый штрих, сухую тесселяцию, экспорт, файл заметки (схема 3), отмену и
повтор. Сценарии ink-input «Заострение новых штрихов», «Перо без данных о
давлении»; note-storage «Закон толщины хранится со штрихом» (все четыре
сценария).

Проверено до плана тем же отдельным проектом: `tst_stroketessellator`,
`tst_wetstroke`, `tst_inkdocument`, `tst_notestore` с новыми тестами
собираются и проходят на правках, новые тесты падают без них. Тесты
`tst_canvasink` и `tst_export` и правки `DocumentCanvas`/`NoteExporter` в среде
плана не собирались (нужен `libqpdf`); замер массы чернил для теста экспорта
снят через `QPdfWriter` + `pdftoppm` без наложения qpdf: Legacy 3.005,
Tapered 0.710.

**Files:**
- Modify: `src/ink/inktypes.h`, `src/ink/stroketessellator.h`,
  `src/ink/stroketessellator.cpp`, `src/ink/wetstroke.cpp`,
  `src/storage/notestore.h`, `src/storage/notestore.cpp`,
  `src/canvas/documentcanvas.h`, `src/canvas/documentcanvas.cpp`,
  `src/export/noteexporter.cpp`
- Test: `tests/tst_stroketessellator.cpp`, `tests/tst_inkdocument.cpp`,
  `tests/tst_wetstroke.cpp`, `tests/tst_notestore.cpp`,
  `tests/tst_canvasink.cpp`, `tests/tst_export.cpp`

### 4a. Тип, тесселятор, мокрый штрих, история

- [x] 4.1 **Тест закона толщины в `tests/tst_stroketessellator.cpp`.** Вставить
  перед `void loopHasNoArtifacts()`:

  ```cpp
      // ink-beauty-pass, D3, сценарии «Заострение новых штрихов» и «Перо без
      // данных о давлении»: Legacy - W·(0.25 + 0.75p), Tapered - W·(0.08 +
      // 0.92p), без давления оба - W·0.7375 при давлении по умолчанию 0.65.
      void styleSelectsThicknessLaw()
      {
          const auto widthAt = [](const StrokeTessellator &tessellator, qreal pressure) {
              return 2.0 * tessellator.halfWidthAt({QPointF(0, 0), pressure, 0.0});
          };
          const auto near = [](qreal a, qreal b) { return qAbs(a - b) < 1e-9; };

          StrokeTessellator tessellator;
          tessellator.setWidth(4.0);
          // По умолчанию - прежний закон: им рисуются штрихи старых заметок.
          QVERIFY(near(widthAt(tessellator, 0.0), 4.0 * 0.25));

          tessellator.setStyle(StrokeStyle::Tapered);
          QVERIFY(near(widthAt(tessellator, 0.0), 4.0 * 0.08));
          QVERIFY(near(widthAt(tessellator, 0.5), 4.0 * (0.08 + 0.92 * 0.5)));
          QVERIFY(near(widthAt(tessellator, 1.0), 4.0));
          QVERIFY2(near(widthAt(tessellator, -1.0), 4.0 * 0.7375),
                   qPrintable(QStringLiteral("без давления %1").arg(widthAt(tessellator, -1.0))));

          tessellator.setStyle(StrokeStyle::Legacy);
          QVERIFY(near(widthAt(tessellator, 0.0), 4.0 * 0.25));
          QVERIFY(near(widthAt(tessellator, 0.5), 4.0 * (0.25 + 0.75 * 0.5)));
          QVERIFY(near(widthAt(tessellator, 1.0), 4.0));
          QVERIFY(near(widthAt(tessellator, -1.0), 4.0 * 0.7375));

          // И полоса идёт по тому же закону: прямая с нулевым нажимом.
          tessellator.setStyle(StrokeStyle::Tapered);
          QList<InkPoint> points;
          for (int i = 0; i < 10; ++i)
              points.append({QPointF(i * 5.0, 50.0), 0.0, i / 200.0});
          const QVector<Vertex> strip = tessellator.tessellate(points);
          QCOMPARE(strip.size(), points.size() * 2);
          for (int i = 0; i + 1 < strip.size(); i += 2) {
              const qreal width = std::hypot(strip[i].x - strip[i + 1].x, strip[i].y - strip[i + 1].y);
              QVERIFY2(qAbs(width - 4.0 * 0.08) < 1e-4,
                       qPrintable(QStringLiteral("толщина %1 в паре %2").arg(width).arg(i / 2)));
          }
      }

  ```

  Строка «без давления» ловит наивную реализацию (`setStyle` только меняет
  долю): она дала бы `4·(0.08 + 0.92·0.65) = 2.712` вместо 2.95. Прежний тест
  `missingPressureGivesConstantWidth` (`setMinWidthFactor(0.0)`,
  `setConstantPressure(1.0)`) после правки 4.5 даёт `3·(0.25 + 0.75·1) = 3.0` -
  то же, что и сейчас, правки не требует.

- [x] 4.2 **Тест отмены и повтора в `tests/tst_inkdocument.cpp`.** Вставить
  перед комментарием `// Гранулярность: одно движение ластика - одна отмена, сколько бы штрихов`:

  ```cpp
      // ink-beauty-pass, сценарий «Отмена и повтор» требования «Закон толщины
      // хранится со штрихом»: отмена, повтор и отмена стирания возвращают штрих
      // с тем же законом толщины.
      void undoRedoKeepStrokeStyle()
      {
          InkDocument ink;
          Stroke stroke = lineOnPage(1, QPointF(5, 5), QPointF(80, 40));
          stroke.style = StrokeStyle::Tapered;
          const quint64 tapered = ink.addStroke(stroke);
          const quint64 legacy = ink.addStroke(lineOnPage(1, QPointF(5, 60), QPointF(80, 90)));
          const auto styleOf = [&ink](quint64 id) {
              for (const InkDocument::Record &record : ink.strokesOnPage(1)) {
                  if (record.id == id)
                      return record.stroke.style;
              }
              return StrokeStyle(-1);
          };

          ink.undo();
          ink.undo();
          ink.redo();
          ink.redo();
          QCOMPARE(styleOf(tapered), StrokeStyle::Tapered);
          QCOMPARE(styleOf(legacy), StrokeStyle::Legacy);

          ink.eraseStrokes({tapered, legacy});
          QCOMPARE(ink.strokeCount(), 0);
          ink.undo();
          QCOMPARE(styleOf(tapered), StrokeStyle::Tapered);
          QCOMPARE(styleOf(legacy), StrokeStyle::Legacy);
      }

  ```

  Честно: после 4.3 этот тест зелёный сразу - `InkDocument::Action` хранит
  `Record` целиком (`QList<Record> records`), и поле едет вместе со штрихом.
  Тест закрепляет сценарий спеки против будущей «оптимизации» истории.

- [x] 4.3 **Прогон: сборка падает.**
  `cmake --build build -j$(nproc) 2>&1 | grep -m3 error`
  Ожидается ошибка компиляции `tst_stroketessellator.cpp` и
  `tst_inkdocument.cpp`: `'StrokeStyle' has not been declared` /
  `'class StrokeTessellator' has no member named 'setStyle'`.

- [x] 4.4 **`StrokeStyle` и `Stroke::style` в `src/ink/inktypes.h`.** Перед
  комментарием `// Точка траектории. Координаты - в системе координат страницы (точки PDF),`
  вставить:

  ```cpp
  // Закон толщины штриха от давления (ink-beauty-pass, design.md, D3). Хранится
  // со штрихом: смена закона для новых штрихов не меняет толщину нанесённых.
  // Значения пишутся в файл заметки - не перенумеровывать.
  enum class StrokeStyle : int {
      Legacy = 0,  // W·(0.25 + 0.75p) - все штрихи, нанесённые до заострения
      Tapered = 1, // W·(0.08 + 0.92p) - тонкие заходы и выходы, как у пера rnote
  };

  ```

  В `struct Stroke` между `qreal width = 2.4;` и `QList<InkPoint> points;`
  вставить:

  ```cpp
      // Закон толщины. По умолчанию прежний: так читаются строки заметок, где
      // закона ещё не было. Новые штрихи пера получают Tapered в DocumentCanvas.
      StrokeStyle style = StrokeStyle::Legacy;
  ```

- [x] 4.5 **`StrokeTessellator::setStyle` и закон без давления.**
  `src/ink/stroketessellator.h`: блок

  ```cpp
      // Доля толщины, остающаяся при нулевом нажиме: линия не должна
      // схлопываться в ничто на слабом касании.
      void setMinWidthFactor(qreal factor);

      // Толщина, которой рисуется штрих, если устройство не сообщает давление.
      void setConstantPressure(qreal pressure);
  ```

  заменить на:

  ```cpp
      // Доля толщины, остающаяся при нулевом нажиме: линия не должна
      // схлопываться в ничто на слабом касании. Обычно задаётся через setStyle.
      void setMinWidthFactor(qreal factor);

      // Закон толщины штриха: доля при нулевом нажиме 0.25 у Legacy и 0.08 у
      // Tapered (ink-beauty-pass, design.md, D3). Задаётся перед каждым штрихом -
      // и мокрым, и сухим, и для экспорта: один тесселятор рисует штрихи разных
      // законов.
      void setStyle(StrokeStyle style);

      // Давление, которым рисуется штрих, если устройство давления не сообщает.
      // Такой штрих идёт по закону Legacy при любом стиле: толщина пера без
      // давления задаётся настройками и от заострения не меняется (D3).
      void setConstantPressure(qreal pressure);
  ```

  `src/ink/stroketessellator.cpp`, анонимное пространство имён, после
  `constexpr qreal kBisectorEpsilon = 0.2;` вставить:

  ```cpp

  // Доля толщины при нулевом нажиме по законам штриха (ink-beauty-pass,
  // design.md, D3). 0.08, а не 0, как у rnote (W·p): на стенде конец 0.57 и
  // заход 15 px против 0.544 и 13.7 px у эталона, а касание почти без нажима
  // не пропадает совсем.
  constexpr qreal kLegacyMinWidthFactor = 0.25;
  constexpr qreal kTaperedMinWidthFactor = 0.08;
  ```

  Функцию `StrokeTessellator::halfWidthAt` заменить целиком (вместе с новой
  `setStyle` перед ней):

  ```cpp
  void StrokeTessellator::setStyle(StrokeStyle style)
  {
      setMinWidthFactor(style == StrokeStyle::Tapered ? kTaperedMinWidthFactor
                                                      : kLegacyMinWidthFactor);
  }

  qreal StrokeTessellator::halfWidthAt(const InkPoint &point) const
  {
      // Устройство не сообщает давление - штрих идёт постоянной толщиной, и
      // всегда по прежнему закону: W·0.7375 при давлении по умолчанию 0.65,
      // какой бы ни был стиль штриха (D3).
      if (!point.hasPressure()) {
          const qreal factor =
              kLegacyMinWidthFactor + (1.0 - kLegacyMinWidthFactor) * m_constantPressure;
          return m_width * factor * 0.5;
      }
      const qreal pressure = qBound(0.0, point.pressure, 1.0);
      const qreal factor = m_minWidthFactor + (1.0 - m_minWidthFactor) * pressure;
      return m_width * factor * 0.5;
  }
  ```

  Инициализатор `qreal m_minWidthFactor = 0.25;` в заголовке не меняется: это
  и есть Legacy по умолчанию.

- [x] 4.6 **Прогон: тесселятор и история зелёные.**
  `cmake --build build -j$(nproc) && ctest --test-dir build -R '^tst_(stroketessellator|inkdocument)$' --output-on-failure`
  Ожидается `100% tests passed, 0 tests failed out of 2`.

- [x] 4.7 **Тест мокрого штриха в `tests/tst_wetstroke.cpp`.** Вставить перед
  `void geometryStaysFiniteThroughSharpTurns()`:

  ```cpp
      // ink-beauty-pass, D3: мокрый штрих рисуется законом толщины своего
      // прототипа, и завершённый штрих уносит этот закон с собой.
      void wetStrokeUsesPrototypeStyle()
      {
          Stroke proto = prototype(4.0);
          proto.style = StrokeStyle::Tapered;
          WetStroke wet;
          wet.begin(0, proto);
          // Прямая с нулевым нажимом: каждая пара вершин полосы - поперечник
          // штриха, и он обязан быть 0.08 толщины пера, а не 0.25.
          for (int i = 0; i < 40; ++i)
              wet.addPoint({QPointF(i * 3.0, 20.0), 0.0, i / 200.0});
          const Stroke stroke = wet.finish();
          QCOMPARE(stroke.style, StrokeStyle::Tapered);

          const QVector<WetStroke::Vertex> &strip = wet.strip();
          QVERIFY(strip.size() >= 4);
          for (int i = 0; i + 1 < strip.size(); i += 2) {
              const qreal width = std::hypot(strip[i].x - strip[i + 1].x, strip[i].y - strip[i + 1].y);
              QVERIFY2(qAbs(width - 4.0 * 0.08) < 1e-3,
                       qPrintable(QStringLiteral("толщина %1 в паре %2").arg(width).arg(i / 2)));
          }
      }

  ```

- [x] 4.8 **Прогон: мокрый штрих рисует прежним законом.**
  `cmake --build build -j$(nproc) && ctest --test-dir build -R '^tst_wetstroke$' --output-on-failure`
  Ожидается `1 failed`: `wetStrokeUsesPrototypeStyle` - «толщина 1 в паре 0»
  (4·0.25 вместо 4·0.08; стиль в `Stroke` уже доходит, в тесселятор - нет).

- [x] 4.9 **`WetStroke::begin` задаёт закон.** `src/ink/wetstroke.cpp`, в конце
  `WetStroke::begin` после `    m_tessellator.setWidth(prototype.width);`
  вставить:

  ```cpp
      // Мокрый штрих рисуется тем же законом толщины, с каким штрих потом
      // ляжет в страницу и в файл (ink-beauty-pass, design.md, D3).
      m_tessellator.setStyle(prototype.style);
  ```

- [x] 4.10 **Прогон.**
  `cmake --build build -j$(nproc) && ctest --test-dir build -R '^tst_(wetstroke|strokesampler|stroketessellator|inkdocument)$' --output-on-failure`
  Ожидается `100% tests passed, 0 tests failed out of 4`
  (`incrementalResultMatchesFullTessellation` сравнивает с тесселятором по
  умолчанию, а прототип там Legacy - совпадает).

- [x] 4.11 **Коммит.**

  ```sh
  git add src/ink/inktypes.h src/ink/stroketessellator.h src/ink/stroketessellator.cpp \
          src/ink/wetstroke.cpp tests/tst_stroketessellator.cpp tests/tst_inkdocument.cpp \
          tests/tst_wetstroke.cpp
  git commit -F - <<'EOF'
  Закон толщины у штриха

  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  EOF
  ```

### 4b. Хранение: схема 3

- [x] 4.12 **Тесты хранения и миграции в `tests/tst_notestore.cpp`.**
  Файлы старой схемы тесты уже строят напрямую через `RawNote` (см.
  `opensSchemaVersionOneNote`); тест миграции 2 → 3 делает так же, со схемой
  ровно как в нынешнем `NoteStore::initializeSchema()`.

  1. В `opensSchemaVersionOneNote` ожидаемую версию схемы поднять: строку
     `                     QVariantList({QStringLiteral("2")}));` заменить на
     `                     QVariantList({QStringLiteral("3")}));`.
     Тест проверяет, что открытый файл помечен текущей схемой; по D3 текущая -
     3 (note-storage, сценарий «Заметка прежней версии»).

  2. Перед комментарием `    // Задача 2.6, сценарий «Вкладыш на исчезнувшей странице».`
     вставить:

  ```cpp
      // --- ink-beauty-pass, D3: закон толщины хранится со штрихом -----------

      // Сценарий «Штрих после повторного открытия»: закон толщины переживает
      // запись - и у штриха страницы, и у штриха вкладыша.
      void roundTripsStrokeStyle()
      {
          const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("ac.pdf"));
          const QString note = NoteStore::notePathFor(pdf);

          quint64 legacy = 0;
          quint64 tapered = 0;
          quint64 insertStroke = 0;
          {
              InkDocument ink;
              NoteStore store;
              QVERIFY(store.create(note, pdf));
              store.attach(&ink);
              legacy = ink.addStroke(lineOnPage(0));
              Stroke stroke = lineOnPage(0, 30.0);
              stroke.style = StrokeStyle::Tapered;
              tapered = ink.addStroke(stroke);
              const quint64 insert = ink.addInsert(0, QRectF(0, 0, 200, 140));
              stroke.insert = insert;
              insertStroke = ink.addStroke(stroke);
              QVERIFY(store.flush());
          }

          InkDocument ink;
          NoteStore store;
          QVERIFY(store.openNote(note));
          QVERIFY(store.load(&ink));
          QHash<quint64, StrokeStyle> styles;
          for (const InkDocument::Record &record : ink.allStrokes())
              styles.insert(record.id, record.stroke.style);
          for (const InkDocument::Record &record : ink.allInsertStrokes())
              styles.insert(record.id, record.stroke.style);
          QCOMPARE(styles.size(), 3);
          QCOMPARE(styles.value(legacy), StrokeStyle::Legacy);
          QCOMPARE(styles.value(tapered), StrokeStyle::Tapered);
          QCOMPARE(styles.value(insertStroke), StrokeStyle::Tapered);
      }

      // Сценарий «Заметка прежней версии»: файл схемы 2 получает колонку style
      // в обеих таблицах штрихов, его штрихи читаются законом Legacy, а новый
      // штрих сохраняет свой закон.
      void migratesSchemaVersionTwoNote()
      {
          const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("ad.pdf"));
          const QString note = NoteStore::notePathFor(pdf);

          {
              RawNote raw(note);
              QVERIFY(raw.isOpen());
              // Схема ровно такая, какой её создавала версия 2.
              const QStringList schema = {
                  QStringLiteral("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)"),
                  QStringLiteral("CREATE TABLE stroke (id INTEGER PRIMARY KEY, page INTEGER NOT NULL, "
                                 "tool INTEGER NOT NULL, color INTEGER NOT NULL, width REAL NOT NULL, "
                                 "points BLOB NOT NULL)"),
                  QStringLiteral("CREATE INDEX stroke_page ON stroke(page)"),
                  QStringLiteral("CREATE TABLE view_state (id INTEGER PRIMARY KEY CHECK (id = 1), "
                                 "page INTEGER NOT NULL, zoom REAL NOT NULL, page_mode INTEGER NOT NULL, "
                                 "origin_x REAL NOT NULL, origin_y REAL NOT NULL)"),
                  QStringLiteral("CREATE TABLE ink_insert (id INTEGER PRIMARY KEY, page INTEGER NOT NULL, "
                                 "x REAL NOT NULL, y REAL NOT NULL, width REAL NOT NULL, "
                                 "height REAL NOT NULL, open INTEGER NOT NULL)"),
                  QStringLiteral("CREATE TABLE insert_stroke (id INTEGER PRIMARY KEY, "
                                 "insert_id INTEGER NOT NULL, tool INTEGER NOT NULL, "
                                 "color INTEGER NOT NULL, width REAL NOT NULL, points BLOB NOT NULL)"),
                  QStringLiteral("CREATE INDEX insert_stroke_insert ON insert_stroke(insert_id)"),
              };
              for (const QString &statement : schema)
                  QVERIFY(raw.exec(statement));
              const QList<QPair<QString, QString>> metas = {
                  {QStringLiteral("schema"), QStringLiteral("2")},
                  {QStringLiteral("source_path"), QFileInfo(pdf).absoluteFilePath()},
                  {QStringLiteral("source_size"), QString::number(QFileInfo(pdf).size())},
                  {QStringLiteral("source_hash"), NoteStore::hashOfFile(pdf)},
              };
              for (const auto &[key, value] : metas) {
                  QVERIFY(raw.exec(QStringLiteral("INSERT INTO meta(key, value) VALUES(?, ?)"),
                                   {key, value}));
              }
              QVERIFY(raw.exec(QStringLiteral("INSERT INTO stroke(id, page, tool, color, width, "
                                              "points) VALUES(1, 0, 0, ?, 2.0, ?)"),
                               {QColor(Qt::black).rgba(), packedLine(12)}));
              QVERIFY(raw.exec(QStringLiteral("INSERT INTO ink_insert(id, page, x, y, width, height, "
                                              "open) VALUES(1, 0, 30.0, 40.0, 200.0, 140.0, 1)")));
              QVERIFY(raw.exec(QStringLiteral("INSERT INTO insert_stroke(id, insert_id, tool, color, "
                                              "width, points) VALUES(2, 1, 0, ?, 2.0, ?)"),
                               {QColor(Qt::black).rgba(), packedLine(12)}));
          }

          quint64 added = 0;
          {
              InkDocument ink;
              NoteStore store;
              QVERIFY2(store.openNote(note), qPrintable(store.errorString()));
              QVERIFY2(store.load(&ink), qPrintable(store.errorString()));
              QCOMPARE(ink.allStrokes().size(), 1);
              QCOMPARE(ink.allStrokes().first().stroke.style, StrokeStyle::Legacy);
              QCOMPARE(ink.strokesInInsert(1).size(), 1);
              QCOMPARE(ink.strokesInInsert(1).first().stroke.style, StrokeStyle::Legacy);

              store.attach(&ink);
              Stroke stroke = lineOnPage(1);
              stroke.style = StrokeStyle::Tapered;
              added = ink.addStroke(stroke);
              QVERIFY(store.flush());
          }

          {
              RawNote raw(note);
              QCOMPARE(raw.column(QStringLiteral("SELECT value FROM meta WHERE key = 'schema'")),
                       QVariantList({QStringLiteral("3")}));
              for (const QString &table : {QStringLiteral("stroke"), QStringLiteral("insert_stroke")}) {
                  QVERIFY2(raw.column(QStringLiteral("SELECT name FROM pragma_table_info('%1')").arg(table))
                               .contains(QStringLiteral("style")),
                           qPrintable(table));
              }
              QCOMPARE(raw.column(QStringLiteral("SELECT style FROM stroke ORDER BY id")),
                       QVariantList({0, 1}));
          }

          InkDocument ink;
          NoteStore store;
          QVERIFY(store.openNote(note));
          QVERIFY(store.load(&ink));
          for (const InkDocument::Record &record : ink.allStrokes())
              QCOMPARE(record.stroke.style, record.id == added ? StrokeStyle::Tapered : StrokeStyle::Legacy);
      }

      // Сценарий «Открытие прежней версией приложения»: версия 0.7 читает stroke
      // своим запросом и пишет строки без колонки style. Файл схемы 3 принимает
      // и то и другое, а строка без style читается законом Legacy.
      void previousVersionQueriesWorkOnSchemaThree()
      {
          const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("ae.pdf"));
          const QString note = NoteStore::notePathFor(pdf);
          {
              InkDocument ink;
              NoteStore store;
              QVERIFY(store.create(note, pdf));
              store.attach(&ink);
              Stroke stroke = lineOnPage(0);
              stroke.style = StrokeStyle::Tapered;
              ink.addStroke(stroke);
              QVERIFY(store.flush());
          }

          {
              RawNote raw(note);
              QCOMPARE(raw.column(QStringLiteral("SELECT page FROM (SELECT id, page, tool, color, "
                                                 "width, points FROM stroke ORDER BY id)")),
                       QVariantList({0}));
              QVERIFY(raw.exec(QStringLiteral("INSERT INTO stroke(id, page, tool, color, width, points) "
                                              "VALUES(?, ?, ?, ?, ?, ?)"),
                               {7, 2, 0, QColor(Qt::black).rgba(), 2.0, packedLine(12)}));
          }

          InkDocument ink;
          NoteStore store;
          QVERIFY(store.openNote(note));
          QVERIFY(store.load(&ink));
          QCOMPARE(ink.allStrokes().size(), 2);
          QCOMPARE(ink.allStrokes()[0].stroke.style, StrokeStyle::Tapered);
          QCOMPARE(ink.allStrokes()[1].id, quint64(7));
          QCOMPARE(ink.allStrokes()[1].stroke.style, StrokeStyle::Legacy);
      }

  ```

- [x] 4.13 **Прогон: четыре теста падают.**
  `cmake --build build -j$(nproc) && ctest --test-dir build -R '^tst_notestore$' --output-on-failure`
  Ожидается `4 failed`:
  - `opensSchemaVersionOneNote` - «Compared lists differ at index 0» (схема "2");
  - `roundTripsStrokeStyle` - «Compared values are not the same» (Tapered
    прочитан как Legacy: поле не пишется);
  - `migratesSchemaVersionTwoNote` - «Compared lists differ at index 0» (схема "2");
  - `previousVersionQueriesWorkOnSchemaThree` - «Compared values are not the same».

- [x] 4.14 **Объявление миграции колонки.** `src/storage/notestore.h`, секция
  `private:`, после `    bool initializeSchema();` вставить:

  ```cpp
      // Добавляет колонку, если её в таблице нет: миграция файла прежней схемы.
      bool ensureColumn(const QString &table, const QString &column, const QString &definition);
  ```

- [x] 4.15 **Схема 3 в `src/storage/notestore.cpp`.** Шесть правок:

  1. Комментарий и константу версии (строки 14-17) заменить на:

  ```cpp
  // Версия 2 добавила вкладыши. Их таблицы создаются через CREATE TABLE IF NOT
  // EXISTS, как и прежние, поэтому файл первой версии получает их при открытии
  // без отдельной миграции (design.md, D8).
  //
  // Версия 3 добавила закон толщины штриха - колонку style в stroke и
  // insert_stroke (ink-beauty-pass, design.md, D3). Колонку в существующую
  // таблицу CREATE TABLE IF NOT EXISTS не добавит, поэтому для неё миграция
  // явная: ensureColumn при каждом открытии.
  constexpr int kSchemaVersion = 3;
  ```

  2. В конце анонимного пространства имён, после функции `unpackPoints` и
     перед `} // namespace`, вставить:

  ```cpp

  // Закон толщины из файла. Неизвестное значение - от будущей версии - читается
  // как Legacy: так же прежние версии читают файл, о колонке не зная.
  StrokeStyle styleFromFile(int value)
  {
      return value == int(StrokeStyle::Tapered) ? StrokeStyle::Tapered : StrokeStyle::Legacy;
  }
  ```

  3. В `initializeSchema()` в обоих `CREATE TABLE IF NOT EXISTS stroke (` и
     `CREATE TABLE IF NOT EXISTS insert_stroke (` последнюю строку
     `"  points BLOB NOT NULL)"),` заменить на две:

  ```cpp
                         "  points BLOB NOT NULL,"
                         "  style INTEGER NOT NULL DEFAULT 0)"),
  ```

  4. Конец `initializeSchema()` - две строки `    return true;` и `}` сразу
     после цикла `for (const QString &statement : statements) { ... }` -
     заменить на текст ниже: миграция в конце `initializeSchema()` и следом
     новая функция `ensureColumn`:

  ```cpp

      // Схема 3. DEFAULT 0 - закон Legacy: им читаются все штрихи файла прежней
      // схемы, и такие же строки пишет прежняя версия приложения, которая о
      // колонке не знает (сценарий «Открытие прежней версией приложения»).
      for (const QString &table : {QStringLiteral("stroke"), QStringLiteral("insert_stroke")}) {
          if (!ensureColumn(table, QStringLiteral("style"),
                            QStringLiteral("INTEGER NOT NULL DEFAULT 0")))
              return false;
      }
      return true;
  }

  bool NoteStore::ensureColumn(const QString &table, const QString &column,
                               const QString &definition)
  {
      QSqlQuery query(m_database);
      if (!query.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
          m_errorString = query.lastError().text();
          return false;
      }
      while (query.next()) {
          if (query.value(1).toString() == column) // колонки: cid, name, type, ...
              return true;
      }
      if (!query.exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3").arg(table, column, definition))) {
          m_errorString = query.lastError().text();
          return false;
      }
      return true;
  }
  ```

  5. `NoteStore::load`: в лямбде `readStroke` после строки
     `        record->stroke.points = unpackPoints(query.value(5).toByteArray());`
     вставить:

  ```cpp
          record->stroke.style = styleFromFile(query.value(6).toInt());
  ```

     Запрос штрихов страницы заменить на
     `"SELECT id, page, tool, color, width, points, style FROM stroke ORDER BY id"`,
     запрос штрихов вкладышей - на

  ```cpp
              "SELECT id, insert_id, tool, color, width, points, style FROM insert_stroke "
              "ORDER BY id"))) {
  ```

  6. `writeStroke` и `writeInsertStroke` заменить целиком:

  ```cpp
  bool NoteStore::writeStroke(const InkDocument::Record &record)
  {
      return exec(
          QStringLiteral(
              "INSERT INTO stroke(id, page, tool, color, width, points, style) "
              "VALUES(?, ?, ?, ?, ?, ?, ?) "
              "ON CONFLICT(id) DO UPDATE SET page=excluded.page, tool=excluded.tool, "
              "color=excluded.color, width=excluded.width, points=excluded.points, "
              "style=excluded.style"),
          {qulonglong(record.id), record.stroke.page, int(record.stroke.tool),
           record.stroke.color.rgba(), record.stroke.width, packPoints(record.stroke.points),
           int(record.stroke.style)});
  }

  bool NoteStore::writeInsertStroke(const InkDocument::Record &record)
  {
      return exec(
          QStringLiteral("INSERT INTO insert_stroke(id, insert_id, tool, color, width, points, "
                         "style) VALUES(?, ?, ?, ?, ?, ?, ?) "
                         "ON CONFLICT(id) DO UPDATE SET insert_id=excluded.insert_id, "
                         "tool=excluded.tool, color=excluded.color, width=excluded.width, "
                         "points=excluded.points, style=excluded.style"),
          {qulonglong(record.id), qulonglong(record.stroke.insert), int(record.stroke.tool),
           record.stroke.color.rgba(), record.stroke.width, packPoints(record.stroke.points),
           int(record.stroke.style)});
  }
  ```

  `openNote` менять не нужно: он уже поднимает `meta.schema` до
  `kSchemaVersion`, если она ниже, и не опускает версию файла из будущего.

- [x] 4.16 **Прогон.**
  `cmake --build build -j$(nproc) && ctest --test-dir build -R '^tst_(notestore|crashrecovery)$' --output-on-failure`
  Ожидается `100% tests passed, 0 tests failed out of 2`; в выводе
  `tst_notestore` - `Totals: 28 passed, 0 failed`. `tst_crashrecovery` пишет
  заметку из дочернего процесса и открывает её - схема 3 там создаётся с нуля.

- [x] 4.17 **Коммит.**

  ```sh
  git add src/storage/notestore.h src/storage/notestore.cpp tests/tst_notestore.cpp
  git commit -F - <<'EOF'
  Схема заметки 3: закон толщины штриха

  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  EOF
  ```

### 4c. Холст и экспорт

- [x] 4.18 **Тестовый доступ к сухой геометрии штриха.** Без него не видно,
  каким законом `DocumentCanvas::tessellate` нарисовал штрих.
  `src/canvas/documentcanvas.h`, после
  `    Q_INVOKABLE int inkGeometryStrokeCount(int page) const;` вставить:

  ```cpp
      // Готовая полоса штриха страницы из кэша геометрии; пустая - штриха там
      // нет. По ней тесты видят, каким законом толщины штрих нарисован.
      QVector<StrokeTessellator::Vertex> inkGeometryStrip(int page, quint64 id) const;
  ```

  `src/canvas/documentcanvas.cpp`, сразу после определения
  `DocumentCanvas::inkGeometryStrokeCount` вставить:

  ```cpp

  QVector<StrokeTessellator::Vertex> DocumentCanvas::inkGeometryStrip(int page, quint64 id) const
  {
      for (const StrokeGeometry &entry : m_inkGeometry.value(page)) {
          if (entry.id == id)
              return entry.strip;
      }
      return {};
  }
  ```

  (`StrokeTessellator` в заголовке уже виден через `ink/wetstroke.h`.)

- [x] 4.19 **Тесты холста в `tests/tst_canvasink.cpp`.**

  1. Перед `    void inkInGapBetweenPagesIsIgnored()` вставить:

  ```cpp
      // ink-beauty-pass, D3, сценарий «Заострение новых штрихов»: штрих пера
      // получает заострённый закон толщины.
      void penStrokesAreTapered()
      {
          m_canvas.goToPage(0);
          drawLine(&m_canvas, QPointF(400, 300), QPointF(600, 300));
          QCOMPARE(m_ink.strokeCount(), 1);
          QCOMPARE(m_ink.allStrokes().first().stroke.style, StrokeStyle::Tapered);
      }

      // Сухая геометрия рисует каждый штрих его собственным законом: штрихи,
      // нанесённые раньше (Legacy), толщину не меняют, а перо без давления
      // рисует прежнюю толщину и в заострённом штрихе.
      void dryGeometryFollowsStrokeStyle()
      {
          m_canvas.goToPage(0);
          const int page = m_canvas.pageAtView(QPointF(450, 350));
          QVERIFY(page >= 0);

          struct Spec
          {
              StrokeStyle style;
              qreal pressure;
              qreal expectedFactor;
          };
          const QList<Spec> specs = {
              {StrokeStyle::Legacy, 0.0, 0.25},
              {StrokeStyle::Tapered, 0.0, 0.08},
              {StrokeStyle::Tapered, -1.0, 0.7375},
          };
          QList<InkDocument::Record> records;
          for (int i = 0; i < specs.size(); ++i) {
              Stroke stroke;
              stroke.page = page;
              stroke.width = 4.0;
              stroke.style = specs[i].style;
              for (int k = 0; k <= 20; ++k)
                  stroke.points.append({QPointF(40 + k * 8.0, 120 + i * 40), specs[i].pressure, k / 200.0});
              records.append({quint64(i + 1), stroke});
          }
          m_ink.loadStrokes(records, specs.size() + 1);
          QCOMPARE(m_canvas.inkGeometryStrokeCount(page), int(specs.size()));

          // Прямая вдоль x с постоянным нажимом: вертикальный размах полосы - полная
          // толщина, как бы ни были устроены торцы и стыки (шаги 2 и 3 их меняют).
          for (int i = 0; i < specs.size(); ++i) {
              const QVector<StrokeTessellator::Vertex> strip =
                  m_canvas.inkGeometryStrip(page, quint64(i + 1));
              QVERIFY2(strip.size() >= 4, qPrintable(QString::number(i)));
              qreal top = 1e9;
              qreal bottom = -1e9;
              for (const StrokeTessellator::Vertex &vertex : strip) {
                  top = qMin(top, qreal(vertex.y));
                  bottom = qMax(bottom, qreal(vertex.y));
              }
              const qreal width = bottom - top;
              QVERIFY2(qAbs(width - 4.0 * specs[i].expectedFactor) < 1e-3,
                       qPrintable(QStringLiteral("штрих %1: толщина %2, ожидалась %3")
                                      .arg(i + 1)
                                      .arg(width)
                                      .arg(4.0 * specs[i].expectedFactor)));
          }
      }

  ```

  2. В `finishedStrokeIsTheSameEverywhere` (сценарии «Штрих после повторного
     открытия» и «Отмена и повтор» note-storage):
     - после `        const Stroke drawn = m_ink.allStrokes().first().stroke;` вставить
       `        QCOMPARE(drawn.style, StrokeStyle::Tapered);`
     - после `QVERIFY2(samePoints(restored.allStrokes().first().stroke.points, drawn.points, 1e-3),`
       и его строки сообщения `"штрих из файла не совпал с нанесённым");` вставить
       `            QCOMPARE(restored.allStrokes().first().stroke.style, drawn.style);`
     - после `QVERIFY2(samePoints(m_ink.allStrokes().first().stroke.points, drawn.points, 0.0),`
       и строки `"штрих после отмены и повтора не совпал с нанесённым");` вставить
       `        QCOMPARE(m_ink.allStrokes().first().stroke.style, drawn.style);`
     - после `        tessellator.setWidth(drawn.width);` вставить
       `        tessellator.setStyle(drawn.style);` - контур экспорта строится тем
       же законом, что и в `NoteExporter`.

- [x] 4.20 **Прогон: штрих пера ещё Legacy.**
  `cmake --build build -j$(nproc) && ctest --test-dir build -R '^tst_canvasink$' --output-on-failure`
  Ожидается `3 failed`: `penStrokesAreTapered` и
  `finishedStrokeIsTheSameEverywhere` - «Compared values are not the same»
  (Legacy вместо Tapered); `dryGeometryFollowsStrokeStyle` - «штрих 2:
  толщина 1, ожидалась 0.32».

- [x] 4.21 **Холст: заострение новых штрихов и закон в сухой тесселяции.**
  `src/canvas/documentcanvas.cpp`, `DocumentCanvas::beginInkAt`: после
  `    prototype.insert = insert;` вставить:

  ```cpp
      // Новые штрихи пера - с заострёнными концами; закон уходит вместе со
      // штрихом в страницу, файл и историю (ink-beauty-pass, design.md, D3).
      prototype.style = StrokeStyle::Tapered;
  ```

  `DocumentCanvas::tessellate`: после `        tessellator.setWidth(record.stroke.width);`
  вставить:

  ```cpp
          // Каждый штрих - своим законом толщины: нанесённые до заострения
          // остаются прежней толщины (D3).
          tessellator.setStyle(record.stroke.style);
  ```

  Геометрия вкладышей (`m_insertGeometry`) строится той же `tessellate()` -
  отдельной правки не нужно.

- [x] 4.22 **Прогон.**
  `cmake --build build -j$(nproc) && ctest --test-dir build -R '^tst_(canvasink|pagescenegraph|insertoverlay)$' --output-on-failure`
  Ожидается `100% tests passed, 0 tests failed out of 3`.

- [x] 4.23 **Тест экспорта в `tests/tst_export.cpp`.** Замена дешевле не нужна:
  тест в духе `insertInkIsNotExported` (растр `pdftoppm`) стоит один экспорт
  одной страницы. Вставить перед `private:` в конце класса (после
  `insertInkIsNotExported`):

  ```cpp
      // ink-beauty-pass, D3: экспорт рисует каждый штрих его законом толщины.
      // Две прямые с нулевым нажимом, перо 12 pt: Legacy - 3 pt, Tapered - 0.96 pt.
      void exportFollowsStrokeStyle()
      {
          PdfDocumentModel document;
          QVERIFY(document.open(fixtures::noOutline()));
          const Prepared prepared = prepare(document);

          InkDocument ink;
          for (int i = 0; i < 2; ++i) {
              Stroke stroke;
              stroke.page = 0;
              stroke.width = 12.0;
              stroke.color = QColor(220, 20, 30);
              stroke.style = i == 0 ? StrokeStyle::Legacy : StrokeStyle::Tapered;
              for (int k = 0; k <= 20; ++k)
                  stroke.points.append({QPointF(60 + k * 23.0, 200 + i * 200), 0.0, k / 200.0});
              QVERIFY(ink.addStroke(stroke) != 0);
          }

          const QString output = m_dir.filePath(QStringLiteral("styles.pdf"));
          QString error;
          QVERIFY2(NoteExporter::exportNow(fixtures::noOutline(), prepared.sizes,
                                           prepared.rotations, ink.allStrokes(), output, &error),
                   qPrintable(error));

          // 72 точки на дюйм: пиксель - точка страницы.
          const QString png = m_dir.filePath(QStringLiteral("styles"));
          QProcess render;
          render.start(QStringLiteral("pdftoppm"),
                       {QStringLiteral("-f"), QStringLiteral("1"), QStringLiteral("-l"),
                        QStringLiteral("1"), QStringLiteral("-r"), QStringLiteral("72"),
                        QStringLiteral("-png"), output, png});
          QVERIFY(render.waitForFinished(60000));
          const QFileInfo root(png);
          const QStringList produced =
              QDir(root.absolutePath())
                  .entryList({root.fileName() + QStringLiteral("-*.png")}, QDir::Files);
          QVERIFY(!produced.isEmpty());
          const QImage image(QDir(root.absolutePath()).filePath(produced.first()));
          QVERIFY(!image.isNull());

          // Масса чернил поперёк линии в столбце x = 300: доля покрытия по
          // разности красного и зелёного (у чернил 200, у бумаги и чёрного
          // текста 0), так что текст страницы в счёт не идёт.
          const auto inkAcross = [&image](int centerY) {
              qreal mass = 0.0;
              for (int y = centerY - 10; y <= centerY + 10; ++y) {
                  if (!image.valid(300, y))
                      continue;
                  const QColor c = image.pixelColor(300, y);
                  mass += qBound(0.0, (c.red() - c.green()) / 200.0, 1.0);
              }
              return mass;
          };
          const qreal legacy = inkAcross(200);
          const qreal tapered = inkAcross(400);
          QVERIFY2(legacy > 2.0 && legacy < 4.0,
                   qPrintable(QStringLiteral("Legacy: масса %1 вместо около 3").arg(legacy)));
          QVERIFY2(tapered < legacy * 0.5,
                   qPrintable(QStringLiteral("Tapered: масса %1 при Legacy %2").arg(tapered).arg(legacy)));
      }

  ```

  Ожидаемые массы (замер `QPdfWriter` + `pdftoppm`): Legacy 3.0, Tapered 0.71.

- [x] 4.24 **Прогон: экспорт рисует прежним законом.**
  `cmake --build build -j$(nproc) && ctest --test-dir build -R '^tst_export$' --output-on-failure`
  Ожидается `1 failed`: `exportFollowsStrokeStyle` - «Tapered: масса 3.0… при
  Legacy 3.0…».

- [x] 4.25 **Экспорт задаёт закон.** `src/export/noteexporter.cpp`, в цикле по
  штрихам страницы после `                    tessellator.setWidth(record->stroke.width);`
  вставить:

  ```cpp
                      // Тот же закон толщины, что на экране (ink-beauty-pass, D3).
                      tessellator.setStyle(record->stroke.style);
  ```

- [x] 4.26 **Прогон: весь набор.**
  `cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure`
  Ожидается `100% tests passed`. `tst_idleframes` без сессии Wayland сообщает
  SKIP - это не провал.

- [x] 4.27 **Коммит.**

  ```sh
  git add src/canvas/documentcanvas.h src/canvas/documentcanvas.cpp \
          src/export/noteexporter.cpp tests/tst_canvasink.cpp tests/tst_export.cpp
  git commit -F - <<'EOF'
  Новые штрихи пера заострены

  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  EOF
  ```

## 5. Стенд и гейт шага 1, версия 0.8.0

Отдельный вариант с копией давления не нужен: стенд линкует продуктовые
`src/ink/*.cpp`, так что после групп 3-4 `baseline` - это уже D2+D3. Но
одного перезапуска `baseline` мало по двум причинам:

- `baseline` повторяет `DocumentCanvas`, а тот теперь даёт штрихам Tapered.
  Если `baseline` оставить без `prototype.style`, он молча останется Legacy и
  перестанет быть «приложением как есть». Поэтому `baseline` ставит Tapered и
  зовёт `setStyle` в сухой геометрии.
- Гейт D9 «отслеживание толщины ≤ 0.03» и числа D2 (0.163 → 0.024) сняты на
  законе 0.25. На законе 0.08 та же правка даёт 0.0343 (прототип `e2-taper`),
  и гейт на нём не проходит по построению. Нужен прогон того же конвейера с
  Legacy - вариант `app-legacy`.

Чтобы не копировать класс `Baseline` (он в анонимном пространстве имён
`baseline.cpp`), конвейер выносится в `variants/app_pipeline.h` с законом в
параметре, а `baseline.cpp` и `step1_pressure.cpp` только регистрируют его. CMake
стенда собирает `variants/*.cpp` глобом, заголовок рядом подхватывается
include-ом.

Проверено до плана: стенд `exp/rnote` (метрики профиля толщины,
`--min-width-factor`) собран во scratchpad против правленых `src/ink` групп 3-4
с этими тремя файлами. `app-legacy` совпал с `e2-pressure-caps` во всех
метриках толщины, `baseline` при `--min-width-factor 0.08` - с `e2-taper`;
траектория совпала с baseline шага 0 до бита. Числа ниже - из этого прогона.

**Files:**
- Create: `tools/stroke-lab/variants/app_pipeline.h`,
  `tools/stroke-lab/variants/step1_pressure.cpp`
- Modify: `tools/stroke-lab/variants/baseline.cpp`, `tools/stroke-lab/README.md`,
  `CMakeLists.txt`, `packaging/PKGBUILD`

- [x] 5.1 **Конвейер приложения с законом в параметре.** Создать
  `tools/stroke-lab/variants/app_pipeline.h`:

  ```cpp
  #pragma once

  // Конвейер приложения ровно как в DocumentCanvas: WetStroke::begin(page,
  // prototype, m_zoom) на касании, addPoint на каждый отсчёт, finish() на отрыве;
  // сухая геометрия - новый StrokeTessellator с setWidth(stroke.width) и
  // setStyle(stroke.style), затем tessellate(stroke.points)
  // (DocumentCanvas::tessellate). Растеризация - лабораторная эмуляция MSAA 4x.
  //
  // Закон толщины - параметр: приложение даёт новым штрихам Tapered (baseline),
  // а Legacy нужен, чтобы мерить давление на прежнем законе W·(0.25 + 0.75p),
  // на котором выбраны числа D2 (app-legacy, ink-beauty-pass, design.md).

  #include "lab.h"

  #include "ink/stroketessellator.h"
  #include "ink/wetstroke.h"

  namespace lab {

  class AppPipeline : public Variant
  {
  public:
      explicit AppPipeline(StrokeStyle style)
          : m_style(style)
      {
      }

      void begin(const StrokeSetup &setup) override
      {
          Stroke prototype;
          prototype.tool = InkTool::Pen;
          prototype.color = setup.color;
          prototype.width = setup.width;
          prototype.style = m_style;
          m_wet.begin(0, prototype, setup.scale);
          m_final = Stroke();
      }

      void addEvent(const InkPoint &raw) override { m_wet.addPoint(raw); }

      void wet(int from, WetState &out) const override
      {
          const QList<InkPoint> &points = m_wet.points();
          out.settledCount = int(points.size());
          for (int i = qMax(0, from); i < points.size(); ++i)
              out.settledFrom.push_back(toCenter(points[i]));
          for (const InkPoint &p : m_wet.provisionalPoints())
              out.provisional.push_back(toCenter(p));
          out.vertexCount = int(m_wet.strip().size());
          out.settledVertexCount = m_wet.settledVertexCount();
      }

      void finish() override { m_final = m_wet.finish(); }

      std::vector<CenterPoint> centerline() const override
      {
          std::vector<CenterPoint> out;
          out.reserve(m_final.points.size());
          for (const InkPoint &p : m_final.points)
              out.push_back(toCenter(p));
          return out;
      }

      Geometry geometry() const override
      {
          StrokeTessellator tessellator;
          tessellator.setWidth(m_final.width);
          tessellator.setStyle(m_final.style);
          const QVector<StrokeTessellator::Vertex> strip = tessellator.tessellate(m_final.points);
          Geometry g;
          if (StrokeTessellator::isDegenerate(strip))
              return g;
          g.kind = Geometry::Kind::TriangleStrip;
          g.vertices.reserve(strip.size());
          for (const auto &v : strip)
              g.vertices.emplace_back(v.x, v.y);
          return g;
      }

      Geometry wetGeometry() const override
      {
          Geometry g;
          g.kind = Geometry::Kind::TriangleStrip;
          for (const auto &v : m_wet.strip())
              g.vertices.emplace_back(v.x, v.y);
          return g;
      }

  private:
      CenterPoint toCenter(const InkPoint &p) const
      {
          // halfWidthAt зависит только от толщины и закона, а их задал begin():
          // одна формула и для мокрого, и для сухого штриха.
          auto &tessellator = const_cast<WetStroke &>(m_wet).tessellator();
          return {p.position, 2.0 * tessellator.halfWidthAt(p), p.pressure, p.timeSeconds};
      }

      StrokeStyle m_style;
      WetStroke m_wet;
      Stroke m_final;
  };

  } // namespace lab
  ```

- [x] 5.2 **`baseline` - приложение как есть, с Tapered.** Заменить
  `tools/stroke-lab/variants/baseline.cpp` целиком:

  ```cpp
  // baseline - конвейер приложения как есть (app_pipeline.h). Новые штрихи пера
  // приложение рисует законом Tapered, поэтому и baseline - тоже: его толщину
  // сравнивать с эталоном при --min-width-factor 0.08.

  #include "app_pipeline.h"

  namespace {

  struct Baseline : lab::AppPipeline
  {
      Baseline()
          : AppPipeline(StrokeStyle::Tapered)
      {
      }
  };

  } // namespace

  STROKE_LAB_VARIANT(Baseline, "baseline",
                     "Конвейер приложения: StrokeSampler (ink-stroke-modeler + follow + "
                     "Catmull-Rom 1.2 px, давление по времени узла) -> WetStroke -> "
                     "StrokeTessellator, закон Tapered 0.08+0.92p, MSAA 4x");
  ```

  Если шаг 0 вносил в `baseline.cpp` свои правки (сверить:
  `git diff exp/stroke-lab -- tools/stroke-lab/variants/baseline.cpp`), перенести
  их в `AppPipeline`.

- [x] 5.3 **Вариант `app-legacy`.** Создать
  `tools/stroke-lab/variants/step1_pressure.cpp`:

  ```cpp
  // app-legacy - конвейер приложения с законом толщины Legacy W·(0.25 + 0.75p).
  // Числа D2 (ink-beauty-pass, design.md) выбраны на этом законе, и гейт шага 1
  // «отслеживание толщины ≤ 0.03» меряется здесь, при --min-width-factor 0.25
  // (по умолчанию). Траектория - та же, что у baseline, до бита.

  #include "app_pipeline.h"

  namespace {

  struct AppLegacy : lab::AppPipeline
  {
      AppLegacy()
          : AppPipeline(StrokeStyle::Legacy)
      {
      }
  };

  } // namespace

  STROKE_LAB_VARIANT(AppLegacy, "app-legacy",
                     "Конвейер приложения с законом Legacy 0.25+0.75p: давление D2 на прежнем "
                     "законе, сравнивать при --min-width-factor 0.25");
  ```

- [x] 5.4 **Сборка стенда.**
  `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUQ_BUILD_STROKE_LAB=ON && cmake --build build -j$(nproc) && ./build/tools/stroke-lab/stroke-lab --list | grep -E '^ *(baseline|app-legacy)'`
  Ожидается: сборка без ошибок, в списке оба варианта.

- [x] 5.5 **Гейт A: давление на законе 0.25.**

  ```sh
  OUT=$(mktemp -d)
  ./build/tools/stroke-lab/stroke-lab --samples ref_line,ref_loop,ref_zigzag,ref_spiral,ref_letter_e,ref_fast_circle,ref_corner_v,ref_corner_n,ref_cardiogram,word_minimum,word_hello,word_privet,digits_2025,word_minimum_small,small_o_8,small_o_12,small_o_20,small_a_12,small_e_10,corner_N,corner_M,corner_check,fast_eeee,fast_scribble,slow_line_tremor,slow_arc_tremor,slow_hello --variants baseline,app-legacy --no-png --out $OUT/law025
  jq -r '.["app-legacy"].s1 | "track \(.["width_track_rms_rel@mean"]) rise \(.["width_rise_px@mean"]) end \(.["width_end_ratio@mean"]) mean \(.["width_mean_ratio@mean"]) dev \(.["centerline_dev_mean_px@mean"]) facets \(.["facet_count_2deg@sum"]) cpu \(.["cpu_us_per_event_mean@mean"]) mut \(.["settled_mutations@sum"])"' $OUT/law025/summary.json
  jq '[.samples[] | select(.scale == 1) | .width_end_ratio_truth | select(. != null)] | add / length' $OUT/law025/app-legacy/metrics.json
  ```

  Ожидается (прогон плана): `track 0.0238 rise 11.42 end 0.697 mean 1.0015
  dev 0.1068 facets 2818 cpu ≈2.2 mut 0`, эталон конца 0.681.
  Критерий (все сразу):
  - `width_track_rms_rel` ≤ 0.03 (гейт D9; baseline шага 0 - 0.163);
  - `width_mean_ratio` в 0.95…1.05;
  - `width_rise_px` ≤ 15 (D2: 36.4 → 11.4);
  - `|width_end_ratio − эталон|` ≤ 0.05 (D2: 0.70 при 0.68);
  - `centerline_dev_mean_px` и `facet_count_2deg` равны числам baseline шага 0
    (0.1068 и 2818): траектория не изменилась;
  - `settled_mutations` = 0; `cpu_us_per_event_mean` не больше чем на 0.3 µs
    выше baseline шага 0 на той же машине.

  Столбцы толщины `baseline` в этом прогоне не смотреть: он рисует законом
  0.08, а эталон здесь 0.25.

  При провале: сравнить с прототипом, собрав стенд `exp/rnote` из
  `git archive exp/rnote tools/stroke-lab` в отдельную папку с теми же
  `src/ink`, и прогнать `--variants app-legacy,e2-pressure-caps`. Расхождение
  метрик - ошибка переноса в группе 3 (обычно `pressureAt` не на всех путях
  `followedNode` или сырое давление в `finish`); константы фильтра не
  подгонять без записи в design.md, D2.

- [x] 5.6 **Гейт B: заострение на законе 0.08.**

  ```sh
  ./build/tools/stroke-lab/stroke-lab --samples ref_line,ref_loop,ref_zigzag,ref_spiral,ref_letter_e,ref_fast_circle,ref_corner_v,ref_corner_n,ref_cardiogram,word_minimum,word_hello,word_privet,digits_2025,word_minimum_small,small_o_8,small_o_12,small_o_20,small_a_12,small_e_10,corner_N,corner_M,corner_check,fast_eeee,fast_scribble,slow_line_tremor,slow_arc_tremor,slow_hello --variants baseline --min-width-factor 0.08 --no-png --out $OUT/law008
  jq -r '.baseline.s1 | "rise \(.["width_rise_px@mean"]) end \(.["width_end_ratio@mean"]) track \(.["width_track_rms_rel@mean"]) mean \(.["width_mean_ratio@mean"])"' $OUT/law008/summary.json
  jq '[.samples[] | select(.scale == 1) | .width_end_ratio_truth | select(. != null)] | add / length' $OUT/law008/baseline/metrics.json
  jq '[.samples[] | select(.scale == 1) | .width_rise_truth_px | select(. != null)] | add / length' $OUT/law008/baseline/metrics.json
  ```

  Ожидается: `rise 15.02 end 0.570 track 0.0343 mean 1.0023`, эталон конца
  0.544, эталон захода 13.68.
  Критерий:
  - `width_rise_px` ≤ 15.5. В design.md это «15 px при 13.7» - округление
    15.02; порог «≤ 15» буквально не прошёл бы и прототип `e2-taper`, с которым
    перенос совпал до сотых;
  - `|width_end_ratio − эталон|` ≤ 0.05 (0.570 против 0.544 - конец «≈
    эталону»);
  - `width_mean_ratio` в 0.95…1.05.
  `width_track_rms_rel` 0.034 здесь не гейт (см. введение к группе).

  При провале: как в 5.5, стендом `exp/rnote` с теми же `src/ink` и
  `--variants baseline,e2-taper --min-width-factor 0.08`. Расхождение по
  заходу/концу при совпавшем гейте A - ошибка в группе 4 (`setStyle` не
  доходит до `WetStroke`: `CenterPoint.width` стенда берётся из мокрого
  тесселятора).

- [x] 5.7 **Команды шага 1 в README стенда.** Дописать в конец
  `tools/stroke-lab/README.md`:

  ````markdown

  ## Шаг 1 ink-beauty-pass: давление и закон толщины

  `baseline` с шага 1 рисует законом Tapered `W·(0.08 + 0.92p)`, как
  приложение рисует новые штрихи; его толщину сравнивать с эталоном при
  `--min-width-factor 0.08`. `app-legacy` - тот же конвейер с прежним законом
  `W·(0.25 + 0.75p)`: на нём сняты числа D2.

  ```sh
  ./build/tools/stroke-lab/stroke-lab --samples ref_line,ref_loop,ref_zigzag,ref_spiral,ref_letter_e,ref_fast_circle,ref_corner_v,ref_corner_n,ref_cardiogram,word_minimum,word_hello,word_privet,digits_2025,word_minimum_small,small_o_8,small_o_12,small_o_20,small_a_12,small_e_10,corner_N,corner_M,corner_check,fast_eeee,fast_scribble,slow_line_tremor,slow_arc_tremor,slow_hello --variants baseline,app-legacy --no-png --out /tmp/lab/law025
  ./build/tools/stroke-lab/stroke-lab --samples ref_line,ref_loop,ref_zigzag,ref_spiral,ref_letter_e,ref_fast_circle,ref_corner_v,ref_corner_n,ref_cardiogram,word_minimum,word_hello,word_privet,digits_2025,word_minimum_small,small_o_8,small_o_12,small_o_20,small_a_12,small_e_10,corner_N,corner_M,corner_check,fast_eeee,fast_scribble,slow_line_tremor,slow_arc_tremor,slow_hello --variants baseline --min-width-factor 0.08 --no-png --out /tmp/lab/law008
  ```

  | метрика (масштаб 1) | baseline шага 0 | app-legacy, закон 0.25 | baseline, закон 0.08 | эталон 0.25 / 0.08 |
  |---|---|---|---|---|
  | отслеживание толщины (`w track`) | 0.163 | 0.024 | 0.034 | - |
  | средняя толщина к эталону | 0.926 | 1.002 | 1.002 | 1 |
  | заход до 90% плато, px | 36.4 | 11.4 | 15.0 | 5.9 / 13.7 |
  | толщина у конца к медиане | 0.95 | 0.70 | 0.57 | 0.68 / 0.544 |

  Гейт шага 1: `w track` у `app-legacy` ≤ 0.03; заход `baseline` при 0.08 ≤ 15.5
  px; конец - в пределах 0.05 от эталона.
  ````

- [x] 5.8 **Коммит стенда.**

  ```sh
  git add tools/stroke-lab/variants/app_pipeline.h tools/stroke-lab/variants/baseline.cpp \
          tools/stroke-lab/variants/step1_pressure.cpp tools/stroke-lab/README.md
  git commit -F - <<'EOF'
  Стенд: baseline с заострением, вариант app-legacy

  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  EOF
  ```

- [x] 5.9 **Тесты шага целиком (гейт D9 по тестам).**
  `cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure`
  Ожидается `100% tests passed` (`tst_idleframes` без сессии - SKIP). Отдельно
  убедиться, что `tst_strokesampler`, `tst_wetstroke`, `tst_notestore` в
  списке пройденных. При провале - не поднимать версию, вернуться к группе,
  чей тест упал.

- [x] 5.10 **Версия 0.8.0.** `CMakeLists.txt`, строка 2:
  `project(uq-notes VERSION 0.7.0 LANGUAGES CXX)` →
  `project(uq-notes VERSION 0.8.0 LANGUAGES CXX)`.
  `packaging/PKGBUILD`, строка 9: `pkgver=0.7.0` → `pkgver=0.8.0` (`pkgrel=1`
  остаётся). Проверка: `git grep -n '0\.7\.0' -- CMakeLists.txt packaging/PKGBUILD`
  ничего не находит; `cmake -S . -B build >/dev/null && grep -m1 CMAKE_PROJECT_VERSION:STATIC build/CMakeCache.txt`
  показывает `0.8.0`.

- [x] 5.11 **Финальный коммит шага прозой.** Числа в тексте сверить с выводом
  5.5, 5.6 и 3.7; если на машине исполнителя CPU отличается, поправить только
  его.

  ```sh
  git add CMakeLists.txt packaging/PKGBUILD
  git commit -F - <<'EOF'
  Толщина штриха следует за нажимом, версия 0.8.0

  Начало штриха оставалось бледным на протяжении 36 px, конец выходил тупым,
  а на отрыве толщина скакала на 56%, оставляя «клюв». Все три дефекта сидели в
  том, как StrokeSampler обращался с давлением. Фильтр One Euro со срезом 2.5 Гц
  запаздывал на 64 мс. Давление узла бралось с проекции пружины модели на
  ломаную отсчётов, хотя положение узла при следовании за пером - перо в момент
  узла, так что давление было ещё на 20 мс старше положения. А последняя точка
  получала сырое давление при отфильтрованном у всех остальных.

  Теперь давление узла - отфильтрованное давление пера, линейно
  интерполированное в момент узла; фильтр - 12 Гц и beta 0.5; на отрыве
  последняя точка получает отфильтрованное давление. На стенде (закон 0.25,
  перо 1 pt, 27 сэмплов) ошибка толщины вдоль штриха 0.163 -> 0.024, средняя
  толщина 0.926 -> 1.00 эталона, заход до 90% плато 36.4 -> 11.4 px при
  эталонных 5.9, толщина у конца 0.95 -> 0.70 при эталонных 0.68. Траектория не
  изменилась ни в одном бите: отклонение осевой, грани и острия те же. Работа
  на событие не выросла, около 2.2 мкс.

  Новые штрихи пера рисуются заострёнными: толщина W·(0.08 + 0.92p) вместо
  W·(0.25 + 0.75p), с тонкими заходами и выходами, как у пера в rnote. На
  стенде с этим законом конец 0.57 при эталоне 0.544, заход 15 px при 13.7.
  Закон хранится со штрихом - Stroke::style, Legacy или Tapered, - и каждый
  штрих тесселируется своим законом: мокрый, сухой и в экспорте. Поэтому
  штрихи, нанесённые раньше, толщину не меняют, а отмена и повтор возвращают
  штрих вместе с законом. Перо без давления рисует прежнюю толщину W·0.7375 при
  любом законе.

  Заметка получила схему 3: колонку style в stroke и insert_stroke, по
  умолчанию 0. CREATE TABLE IF NOT EXISTS колонку в существующую таблицу не
  добавляет, поэтому файл прежней схемы получает её через ALTER TABLE при
  открытии. Прежняя версия приложения такой файл открывает: колонку не читает,
  пишет строки без неё, и новые штрихи у неё рисуются прежним законом.

  В tst_strokesampler три новых эталона: запаздывание давления на заходе не
  больше 15 мс (было 57.5, стало 6.5), конец без скачка на спаде нажима и
  давление точки, равное давлению пера в её момент; каждый падал на прежнем
  коде и ловит свою правку. Закон толщины проверяют tst_stroketessellator,
  tst_wetstroke, tst_canvasink и tst_export, хранение и миграцию 2 -> 3 -
  tst_notestore, отмену и повтор - tst_inkdocument и tst_canvasink. Прежние
  эталоны давления от смены фильтра не сломались; в
  tst_notestore::opensSchemaVersionOneNote ожидаемая схема теперь 3.

  На стенде baseline рисует законом Tapered, как приложение, а вариант
  app-legacy - тот же конвейер с прежним законом, на котором выбраны числа
  D2. Версия поднята до 0.8.0 в CMakeLists.txt и PKGBUILD.

  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  EOF
  ```

### Что раздел не покрывает

- Проверку на устройстве ряби толщины при ровном нажиме (design.md, D2:
  при ряби `minCutoff` 12 → 8 Гц). Шум давления в корпусе синтетический;
  проверка - руками на пере после 0.8.0, правка - одна константа и запись в
  design.md.
- Круглые торцы, точки от касания, узел острия (D4-D6) - шаг 2. Тесты
  `tapWithoutMovementLeavesNoStroke` и `singleTapProducesNoStroke` в шаге 1 не
  трогаются.
- Пересчёт давления и заострения у ранее нанесённых штрихов (non-goal: сырых
  отсчётов нет).
- Выбор закона толщины в интерфейсе: новые штрихи всегда Tapered, настройки
  нет и спека её не требует.
- Перенос требований в `openspec/specs` и архивирование изменения - после
  последнего шага.


# Шаг 2: круглые торцы и точки

Группы 6-9 плана ink-beauty-pass. Исходное состояние: шаги 0 и 1 выполнены
(стенд в `tools/stroke-lab` с `--endcheck`, `StrokeStyle`, `StrokeTessellator::setStyle`,
`Stroke::style`, давление по шагу 1, версия 0.8.0).

Как проверялся этот раздел. Код групп 6-8 для `src/ink` (тесселятор, мокрый штрих,
сэмплер) и тесты `tst_stroketessellator`, `tst_wetstroke`, `tst_strokesampler`
собраны и прогнаны на копии `src/ink` текущего дерева в scratchpad (с заглушкой
`StrokeStyle`/`setStyle` вместо шага 1): все ожидаемые провалы и проходы ниже взяты
из этих прогонов. Вариант стенда `d6-raw121-cusp` и проверка гейта собраны и
прогнаны на стенде из `exp/stroke-lab` (с правками стенда из группы 8) поверх
того же кода групп 6-7 - числа гейта ниже взяты оттуда; гейт не пройден. Тесты `tst_canvasink`, `tst_penonlymode`,
`tst_insertoverlay`, `tst_export` в этой среде не собирались (нет `libqpdf`) -
их код написан по существующим тестам тех же файлов и проверяется только на
шагах прогона ниже.

Решения раздела, которые уточняют design.md:
- **Точка (D5).** Не `StrokeSampler::finish()` дописывает вторую точку к касанию из
  одной точки, а `WetStroke` по правилу прототипа `outline-caps`: если
  окончательная осевая короче полной толщины пера, к штриху дописывается одна новая
  последняя точка в положении последней с давлением `peakPressure()`; касание из
  одной точки - частный случай. Правило живёт в `WetStroke::withDotTwin()` и
  применяется и к мокрой полосе во время касания, и к штриху на `finish()`, поэтому
  мокрый вид совпадает с сухим. `StrokeSampler` по-прежнему даёт
  `contactDuration()` и `peakPressure()`, но точек не дописывает.
  `StrokeTessellator::isDot()` - публичный: решение «точка или нет» у тесселятора и
  у `WetStroke` одно.
- **Публичные константы** `StrokeTessellator::kCapPairs = 8` и `kCapLookback = 48` -
  их используют `WetStroke` и тесты.
- **Гейт D6** строже, чем в design.md: ложные острия по всему корпусу, острие без
  стоянки пера, запас по зубцу короче кадра (группа 8). Гейт на стенде **не
  пройден** - группа 8 описывает путь провала как основной.

Сборка и прогоны везде: `cmake --build build -j$(nproc)` в корне репозитория,
тесты - `ctest --test-dir build -R <имя> --output-on-failure`. Стенд собран с
`-DUQ_BUILD_STROKE_LAB=ON` и лежит в `./build/tools/stroke-lab/stroke-lab`.

---

## 6. Круглые торцы (D4)

Торец - полудиск радиуса полуширины в точке, из `K = 8` симметричных пар вершин на
четверть оборота: `c + r·(∓cos αk·t ± sin αk·n)`, `αk = (π/2)·k/K`, `k = 0..K`.
Начальный торец идёт от кончика `c - r·t` к боковой паре `c ± r·n`, конечный -
от боковой пары к кончику `c + r·t`. Пары сохраняют чётность полосы: `outline()`
(чётные вершины вперёд, нечётные назад), экспорт и куски мокрой полосы работают
без изменений. Направление торца - хорда до первой точки на расстоянии не меньше
радиуса, не дальше 48 точек (`capDirection`).

Мокрый штрих. Начальный торец ложится в окончательную часть только когда хорда для
него известна (точка на расстоянии ≥ r0 среди первых 48 или уже 48 точек) - тогда
`capDirection` на мокрых точках даёт то же, что на сохранённых: точки только
добавляются в конец. До этого вся мокрая полоса провизорная и строится
`tessellate(points + provisional)` - тем же правилом, что сухая. Конечный торец
провизорной части берёт хорду из окна последних 48 точек (окончательные + хвост),
поэтому работа на событие не растёт с длиной штриха, а на отрыве окно совпадает с
последними 48 точками штриха и мокрая полоса совпадает с сухой вершина в вершину.

Центр дуги конечного торца (для тестов «конец линии у пера» и «торец экспорта в
точке отрыва»): торец - последние `2·(K + 1)` вершин полосы, его первая пара -
боковая `c + r·n`, `c - r·n`, и её середина - ровно центр `c`. Прежняя формула
«середина последней пары» теперь даёт кончик `c + r·t`, на полуширину дальше.

**Files:**
- Modify: `src/ink/stroketessellator.h`, `src/ink/stroketessellator.cpp`
- Modify: `src/ink/wetstroke.h`, `src/ink/wetstroke.cpp`
- Test: `tests/tst_stroketessellator.cpp`, `tests/tst_wetstroke.cpp`,
  `tests/tst_canvasink.cpp` (`finishedStrokeIsTheSameEverywhere`), `tests/tst_export.cpp`

- [x] 6.1 **Тесты тесселятора: помощники для торцов.** `tests/tst_stroketessellator.cpp`:
  после `#include <QTest>` добавить `#include <QLineF>` (строка include становится
  такой):

  ```cpp
  #include <QLineF>
  #include <QTest>
  ```

  В анонимном пространстве имён, сразу после функции `distanceToPolyline` (перед
  закрывающим `} // namespace`), вставить:

  ```cpp
  // --- ink-beauty-pass, D4 и D5 ------------------------------------------------

  // Вершин в одном торце: K + 1 пар.
  constexpr int kCapVertices = 2 * (StrokeTessellator::kCapPairs + 1);

  // Середина боковой пары торца - центр его дуги. У начального торца боковая
  // пара последняя, у конечного - первая.
  QPointF capCenter(const QVector<Vertex> &strip, StrokeTessellator::CapEnd end)
  {
      const int i = end == StrokeTessellator::CapEnd::Start ? kCapVertices - 2
                                                            : int(strip.size()) - kCapVertices;
      return (toPoint(strip[i]) + toPoint(strip[i + 1])) / 2.0;
  }

  // Полные толщины пар от боковой пары начального торца до боковой пары
  // конечного. У прямого штриха это ровно по одной паре на точку.
  QList<qreal> sideWidths(const QVector<Vertex> &strip)
  {
      QList<qreal> widths;
      for (int i = kCapVertices - 2; i <= int(strip.size()) - kCapVertices; i += 2)
          widths.append(QLineF(toPoint(strip[i]), toPoint(strip[i + 1])).length());
      return widths;
  }

  // Площадь замкнутого многоугольника по формуле шнурования.
  qreal polygonArea(const QPolygonF &polygon)
  {
      qreal twice = 0.0;
      for (int i = 0; i + 1 < polygon.size(); ++i)
          twice += polygon[i].x() * polygon[i + 1].y() - polygon[i + 1].x() * polygon[i].y();
      return std::abs(twice) / 2.0;
  }

  QRectF boundsOf(const QVector<Vertex> &strip)
  {
      QPolygonF polygon;
      for (const Vertex &vertex : strip)
          polygon.append(toPoint(vertex));
      return polygon.boundingRect();
  }
  ```

- [x] 6.2 **Тесты тесселятора: переписать проверки, считавшие торец одной парой.**
  В том же файле заменить целиком функцию `straightStrokeHasRequestedWidth()`:

  ```cpp
      void straightStrokeHasRequestedWidth()
      {
          StrokeTessellator tessellator;
          tessellator.setWidth(4.0);
          tessellator.setMinWidthFactor(0.0);

          QList<InkPoint> points;
          for (int i = 0; i < 20; ++i)
              points.append({QPointF(i * 5.0, 50.0), 1.0, i / 200.0});

          const QVector<Vertex> strip = tessellator.tessellate(points);
          // По паре на каждую внутреннюю точку и по полудиску на каждом конце.
          QCOMPARE(strip.size(), (points.size() - 2) * 2 + 2 * kCapVertices);

          const QList<qreal> widths = sideWidths(strip);
          QCOMPARE(widths.size(), points.size());
          for (int i = 0; i < widths.size(); ++i) {
              QVERIFY2(qAbs(widths[i] - 4.0) < 1e-4,
                       qPrintable(QStringLiteral("width %1 at %2").arg(widths[i]).arg(i)));
          }
      }
  ```

  В `widthFollowsPressureSmoothly()` заменить блок

  ```cpp
          const QVector<Vertex> strip = tessellator.tessellate(points);

          QList<qreal> widths;
          for (int i = 0; i + 1 < strip.size(); i += 2)
              widths.append(std::hypot(strip[i].x - strip[i + 1].x, strip[i].y - strip[i + 1].y));

          QCOMPARE(widths.size(), count);
  ```

  на

  ```cpp
          const QVector<Vertex> strip = tessellator.tessellate(points);
          const QList<qreal> widths = sideWidths(strip);

          QCOMPARE(widths.size(), count);
  ```

  В `missingPressureGivesConstantWidth()` заменить блок

  ```cpp
          const QVector<Vertex> strip = tessellator.tessellate(points);
          QVERIFY(!StrokeTessellator::isDegenerate(strip));
          for (int i = 0; i + 1 < strip.size(); i += 2) {
              const qreal width = std::hypot(strip[i].x - strip[i + 1].x,
                                             strip[i].y - strip[i + 1].y);
              QVERIFY(qAbs(width - 3.0) < 1e-4);
          }
  ```

  на

  ```cpp
          const QVector<Vertex> strip = tessellator.tessellate(points);
          QVERIFY(!StrokeTessellator::isDegenerate(strip));
          for (const qreal width : sideWidths(strip))
              QVERIFY(qAbs(width - 3.0) < 1e-4);
  ```

  В `sharpTurnProducesRoundJoinVertices()` заменить две последние строки

  ```cpp
          QCOMPARE(tessellator.tessellate(gentle).size(), 6); // три пары, без дуги
          QVERIFY2(tessellator.tessellate(sharp).size() > 6, "стык не скруглён");
  ```

  на

  ```cpp
          // Два торца и одна пара на стыке, без дуги.
          QCOMPARE(tessellator.tessellate(gentle).size(), 2 * kCapVertices + 2);
          QVERIFY2(tessellator.tessellate(sharp).size() > 2 * kCapVertices + 2, "стык не скруглён");
  ```

- [x] 6.3 **Тесты тесселятора: торцы, направление по хорде, контур экспорта, касание в
  одной точке.** В том же файле вставить перед `void degenerateInputIsHandled()`:

  ```cpp
      // --- ink-beauty-pass, D4: круглые торцы ---------------------------------

      // Сценарий «Отрыв пера»: осевая кончается в точке отрыва, а торец -
      // полудиск радиуса полуширины с центром в этой точке, выступающий за неё
      // на полуширину. То же у точки касания.
      void capsAreHalfDiscsAroundStrokeEnds()
      {
          StrokeTessellator tessellator;
          tessellator.setWidth(4.0);
          tessellator.setMinWidthFactor(0.0);

          // Нажим растёт: у начала полуширина 1, у конца 2.
          QList<InkPoint> points;
          for (int i = 0; i < 20; ++i)
              points.append({QPointF(i * 5.0, 50.0), 0.5 + 0.5 * i / 19.0, i / 200.0});
          const QPointF first = points.constFirst().position;
          const QPointF last = points.constLast().position;
          const qreal r0 = tessellator.halfWidthAt(points.constFirst());
          const qreal r1 = tessellator.halfWidthAt(points.constLast());

          const QVector<Vertex> strip = tessellator.tessellate(points);
          QVERIFY(strip.size() >= 2 * kCapVertices);
          QCOMPARE(strip.size() % 2, 0);

          for (int i = 0; i < kCapVertices; ++i) {
              const qreal d = QLineF(toPoint(strip[i]), first).length();
              QVERIFY2(qAbs(d - r0) <= 0.05 * r0,
                       qPrintable(QStringLiteral("начальный торец: вершина %1 в %2 от центра, "
                                                 "радиус %3").arg(i).arg(d).arg(r0)));
          }
          for (int i = int(strip.size()) - kCapVertices; i < strip.size(); ++i) {
              const qreal d = QLineF(toPoint(strip[i]), last).length();
              QVERIFY2(qAbs(d - r1) <= 0.05 * r1,
                       qPrintable(QStringLiteral("конечный торец: вершина %1 в %2 от центра, "
                                                 "радиус %3").arg(i).arg(d).arg(r1)));
          }

          // Кончики торцов - на оси, на полуширину за концами осевой.
          QVERIFY(QLineF(toPoint(strip.constFirst()), first - QPointF(r0, 0.0)).length() < 1e-4);
          QVERIFY(QLineF(toPoint(strip.constLast()), last + QPointF(r1, 0.0)).length() < 1e-4);
          const QRectF bounds = boundsOf(strip);
          QVERIFY(qAbs(bounds.left() - (first.x() - r0)) < 1e-4);
          QVERIFY(qAbs(bounds.right() - (last.x() + r1)) < 1e-4);

          // Центры дуг - концы осевой.
          QVERIFY(QLineF(capCenter(strip, StrokeTessellator::CapEnd::Start), first).length() < 1e-4);
          QVERIFY(QLineF(capCenter(strip, StrokeTessellator::CapEnd::End), last).length() < 1e-4);
      }

      // Направление торца - по хорде длиной не меньше радиуса, а не по
      // последнему отрезку: дотяжка до точки отрыва дописывает отрезок короче
      // шага в случайную сторону.
      void capDirectionFollowsChordNotLastSegment()
      {
          StrokeTessellator tessellator;
          tessellator.setWidth(2.4);
          tessellator.setMinWidthFactor(0.0);

          QList<InkPoint> points;
          for (int i = 0; i < 20; ++i)
              points.append({QPointF(i * 1.2, 50.0), 1.0, i / 200.0});
          // Крючок 0.39 под 50° - короче радиуса 1.2.
          points.append({points.constLast().position + QPointF(0.25, 0.3), 1.0, 20 / 200.0});

          const qreal r = tessellator.halfWidthAt(points.constLast());
          const QPointF end = points.constLast().position;
          // Первая точка назад на расстоянии не меньше r - третья с конца.
          const QPointF far = points[points.size() - 3].position;
          const QPointF chord = (end - far) / QLineF(far, end).length();

          const QPointF direction =
              StrokeTessellator::capDirection(points, StrokeTessellator::CapEnd::End, r);
          QVERIFY2(QLineF(direction, chord).length() < 1e-9,
                   qPrintable(QStringLiteral("направление (%1, %2), хорда (%3, %4)")
                                  .arg(direction.x()).arg(direction.y())
                                  .arg(chord.x()).arg(chord.y())));

          const QVector<Vertex> strip = tessellator.tessellate(points);
          const QPointF tip = toPoint(strip.constLast());
          QVERIFY2(QLineF(tip, end + chord * r).length() < 1e-4,
                   qPrintable(QStringLiteral("кончик торца в (%1, %2)").arg(tip.x()).arg(tip.y())));

          // В начале - то же, только направление от касания вперёд.
          QList<InkPoint> reversed(points.crbegin(), points.crend());
          QVERIFY(QLineF(StrokeTessellator::capDirection(reversed, StrokeTessellator::CapEnd::Start, r),
                         -chord).length() < 1e-9);
      }

      // Хорда ищется не дальше kCapLookback точек: работа мокрого штриха на
      // отсчёт не должна зависеть от того, как долго перо стояло у конца.
      void capDirectionLooksBackBoundedDistance()
      {
          QList<InkPoint> points;
          for (int i = 0; i < 60; ++i)
              points.append({QPointF(0.0, i * 0.01), 1.0, i / 200.0});
          points.append({QPointF(50.0, 0.6), 1.0, 60 / 200.0});

          // Среди первых kCapLookback точек ни одна не отошла на радиус: хорда -
          // до последней из них, а не до далёкой точки за ними.
          const QPointF direction =
              StrokeTessellator::capDirection(points, StrokeTessellator::CapEnd::Start, 1.0);
          QVERIFY2(QLineF(direction, QPointF(0.0, 1.0)).length() < 1e-9,
                   qPrintable(QStringLiteral("(%1, %2)").arg(direction.x()).arg(direction.y())));

          // Все точки в одном месте - направление по умолчанию.
          const QList<InkPoint> stacked(5, InkPoint{QPointF(3, 3), 0.5, 0.0});
          QCOMPARE(StrokeTessellator::capDirection(stacked, StrokeTessellator::CapEnd::End, 1.0),
                   QPointF(1.0, 0.0));
      }

      // Экспорт заливает outline(): с полудисками он остаётся замкнутым
      // контуром, а его площадь - прямоугольник плюс круг.
      void outlineIsClosedContourWithRoundEnds()
      {
          StrokeTessellator tessellator;
          tessellator.setWidth(4.0);
          tessellator.setMinWidthFactor(0.0);

          QList<InkPoint> points;
          for (int i = 0; i <= 20; ++i)
              points.append({QPointF(10.0 + i * 2.0, 30.0), 1.0, i / 200.0});

          const QPolygonF outline = tessellator.outline(points);
          QVERIFY(outline.size() > 2 * kCapVertices);
          QCOMPARE(outline.constFirst(), outline.constLast());

          const qreal r = 2.0;
          const qreal expected = 40.0 * 2.0 * r + M_PI * r * r;
          const qreal area = polygonArea(outline);
          QVERIFY2(qAbs(area - expected) <= 0.005 * expected,
                   qPrintable(QStringLiteral("площадь %1, ждали %2").arg(area).arg(expected)));
          // Ни одна вершина контура не дальше полуширины от осевой.
          for (const QPointF &p : outline)
              QVERIFY(distanceToPolyline(p, points) <= r + 1e-4);
      }

      // Сценарий «Точка над буквой» (D5) в его геометрической части: касание
      // без сдвига - две точки в одном месте - с полудисками на концах уже
      // диск диаметром в толщину при этом нажиме, и в экспорте это круг.
      void tapInOnePlaceIsDisc()
      {
          StrokeTessellator tessellator;
          tessellator.setWidth(3.0);
          const QPointF center(10.0, 10.0);
          const QList<InkPoint> points = {{center, 0.6, 0.0}, {center, 0.6, 0.01}};
          const qreal r = tessellator.halfWidthAt(points.constFirst());

          const QVector<Vertex> strip = tessellator.tessellate(points);
          QVERIFY(!StrokeTessellator::isDegenerate(strip));
          QCOMPARE(strip.size(), 2 * kCapVertices);
          for (const Vertex &vertex : strip)
              QVERIFY(qAbs(QLineF(toPoint(vertex), center).length() - r) < 1e-4);

          const QRectF bounds = boundsOf(strip);
          QVERIFY(qAbs(bounds.width() - 2.0 * r) < 1e-4);
          QVERIFY(qAbs(bounds.height() - 2.0 * r) < 1e-4);

          const qreal area = polygonArea(tessellator.outline(points));
          QVERIFY2(qAbs(area - M_PI * r * r) <= 0.02 * M_PI * r * r,
                   qPrintable(QStringLiteral("площадь %1, круг %2").arg(area).arg(M_PI * r * r)));
      }
  ```

- [x] 6.4 **Тесты мокрого штриха: помощники, «конец линии у пера» по центру дуги,
  круглые торцы на каждом отсчёте, крючок на отрыве.** `tests/tst_wetstroke.cpp`:
  после `#include <QElapsedTimer>` добавить `#include <QLineF>`; перед
  `class TestWetStroke : public QObject` вставить:

  ```cpp
  namespace {

  // Вершин в одном торце: K + 1 пар (design.md, D4).
  constexpr int kCapVertices = 2 * (StrokeTessellator::kCapPairs + 1);

  QPointF toPoint(const WetStroke::Vertex &v)
  {
      return QPointF(v.x, v.y);
  }

  // Центр дуги конечного торца. Торец - последние kCapVertices вершин полосы,
  // его первая пара - боковая, c + r·n и c - r·n, и её середина - центр c.
  // Кончик торца (последняя пара) лежит на полуширину дальше центра, поэтому
  // «конец линии» - центр дуги, а не середина последней пары.
  QPointF endCapCenter(const QVector<WetStroke::Vertex> &strip)
  {
      const int i = int(strip.size()) - kCapVertices;
      return (toPoint(strip[i]) + toPoint(strip[i + 1])) / 2.0;
  }

  bool sameStrip(const QVector<WetStroke::Vertex> &a, const QVector<WetStroke::Vertex> &b)
  {
      if (a.size() != b.size())
          return false;
      for (int i = 0; i < a.size(); ++i) {
          if (qAbs(a[i].x - b[i].x) >= 1e-3 || qAbs(a[i].y - b[i].y) >= 1e-3)
              return false;
      }
      return true;
  }

  } // namespace
  ```

  Заменить целиком функцию `stripEndFollowsThePenOnFastArc()` вместе с её
  комментарием и сразу после неё (перед комментарием «Сценарий «Резкая смена
  направления»») вставить два новых теста:

  ```cpp
      // Сценарий «Кончик линии у пера»: центр дуги торца после каждого отсчёта
      // стоит у последнего положения пера.
      void stripEndFollowsThePenOnFastArc()
      {
          WetStroke wet;
          wet.begin(0, prototype(3.0));
          const QList<InkPoint> input =
              reference::sampledAtSpeed(reference::circle(80.0, 1, QPointF(200, 200)), 1200.0, 240.0);

          for (int i = 0; i < input.size(); ++i) {
              wet.addPoint(input[i]);
              if (wet.strip().size() < 2 * kCapVertices)
                  continue;
              const QPointF end = endCapCenter(wet.strip());
              const qreal lag = QLineF(end, input[i].position).length();
              QVERIFY2(lag <= 2.0,
                       qPrintable(QStringLiteral("конец линии отстал на %1 на отсчёте %2")
                                      .arg(lag)
                                      .arg(i)));
          }
      }

      // ink-beauty-pass, D4: торцы круглые и в окончательной части, и в
      // провизорной - на каждом отсчёте.
      void wetCapsAreRoundAtEveryStep()
      {
          WetStroke wet;
          wet.begin(0, prototype(3.0));
          const QList<InkPoint> input =
              reference::sampledAtSpeed(reference::loop(), 600.0, 481.0);

          int checked = 0;
          for (int i = 0; i < input.size(); ++i) {
              wet.addPoint(input[i]);
              if (wet.settledVertexCount() < kCapVertices)
                  continue;
              const QVector<WetStroke::Vertex> &strip = wet.strip();
              // Начальный торец уже окончательный: полудиск вокруг первой точки.
              const InkPoint &first = wet.points().constFirst();
              const qreal r0 = wet.tessellator().halfWidthAt(first);
              for (int v = 0; v < kCapVertices; ++v)
                  QVERIFY(qAbs(QLineF(toPoint(strip[v]), first.position).length() - r0) <= 0.05 * r0);
              // Конечный торец провизорный: полудиск вокруг конца хвоста.
              const InkPoint &tip = wet.provisionalPoints().isEmpty()
                  ? wet.points().constLast()
                  : wet.provisionalPoints().constLast();
              const qreal r1 = wet.tessellator().halfWidthAt(tip);
              for (int v = int(strip.size()) - kCapVertices; v < strip.size(); ++v)
                  QVERIFY(qAbs(QLineF(toPoint(strip[v]), tip.position).length() - r1) <= 0.05 * r1);
              ++checked;
          }
          QVERIFY2(checked > 100, qPrintable(QStringLiteral("проверено отсчётов: %1").arg(checked)));
      }

      // На отрыве мокрый торец совпадает с сухим и смотрит по хорде, даже если
      // дотяжка до точки контакта дописала короткий отрезок вбок.
      void hookedEndMatchesDryAndFollowsChord()
      {
          WetStroke wet;
          wet.begin(0, prototype(3.0));
          QList<InkPoint> input =
              reference::sampledAtSpeed(reference::straightLine(100, 120.0), 300.0, 240.0);
          input.append({input.constLast().position + QPointF(0.3, 0.4), 0.6,
                        input.constLast().timeSeconds + 1.0 / 240.0});
          for (const InkPoint &point : input)
              wet.addPoint(point);
          const Stroke stroke = wet.finish();

          StrokeTessellator tessellator;
          tessellator.setWidth(stroke.width);
          tessellator.setStyle(stroke.style);
          QVERIFY(sameStrip(wet.strip(), tessellator.tessellate(stroke.points)));

          // Кончик торца - почти по оси x, а не под 53°, как крючок.
          const QPointF end = stroke.points.constLast().position;
          const QPointF tip = toPoint(wet.strip().constLast());
          const qreal angle = std::atan2(tip.y() - end.y(), tip.x() - end.x()) * 180.0 / M_PI;
          QVERIFY2(qAbs(angle) < 20.0, qPrintable(QStringLiteral("торец под %1°").arg(angle)));
      }
  ```

- [x] 6.5 **Тест холста: торец контура экспорта - по центру дуги.**
  `tests/tst_canvasink.cpp`, функция `finishedStrokeIsTheSameEverywhere()`, последний
  блок. Заменить

  ```cpp
          const QPointF capCenter((strip[strip.size() - 2].x + strip[strip.size() - 1].x) / 2.0,
                                  (strip[strip.size() - 2].y + strip[strip.size() - 1].y) / 2.0);
  ```

  на

  ```cpp
          // Торец - последние 2·(K + 1) вершин полосы; первая его пара -
          // боковая, c ± r·n, её середина - центр дуги торца (design.md, D4).
          const int capStart = int(strip.size()) - 2 * (StrokeTessellator::kCapPairs + 1);
          const QPointF capCenter((strip[capStart].x + strip[capStart + 1].x) / 2.0,
                                  (strip[capStart].y + strip[capStart + 1].y) / 2.0);
  ```

  Комментарий над блоком («Экспорт вписывает контур из тех же точек; его торец -
  точка отрыва.») заменить на «Экспорт вписывает контур из тех же точек; центр дуги
  его конечного торца - точка отрыва.»

- [x] 6.6 **Тест экспорта: круглый конец и точка в PDF.** `tests/tst_export.cpp`:
  вставить перед `private:` в конце класса (после `insertInkIsNotExported()`):

  ```cpp
      // ink-beauty-pass, D4 и D5: конец линии в экспорте круглый и выступает за
      // последнюю точку на полуширину, а касание в одной точке - круг. 72 точки
      // на дюйм: пиксель - точка страницы.
      void roundEndsAndDotsAreExported()
      {
          PdfDocumentModel document;
          QVERIFY(document.open(fixtures::noOutline()));
          const Prepared prepared = prepare(document);

          InkDocument ink;
          // Полуширина 5: толщина 10, полный нажим.
          Stroke line;
          line.page = 0;
          line.width = 10.0;
          line.color = QColor(220, 20, 30);
          for (int i = 0; i <= 20; ++i)
              line.points.append({QPointF(100 + i * 5.0, 200), 1.0, i / 200.0});
          QVERIFY(ink.addStroke(line) != 0);
          Stroke dot = line;
          dot.points = {{QPointF(300, 300), 1.0, 0.0}, {QPointF(300, 300), 1.0, 0.01}};
          QVERIFY(ink.addStroke(dot) != 0);

          const QString output = m_dir.filePath(QStringLiteral("round-ends.pdf"));
          QString error;
          QVERIFY2(NoteExporter::exportNow(fixtures::noOutline(), prepared.sizes,
                                           prepared.rotations, ink.allStrokes(), output, &error),
                   qPrintable(error));

          const QString png = m_dir.filePath(QStringLiteral("round-ends"));
          QProcess render;
          render.start(QStringLiteral("pdftoppm"),
                       {QStringLiteral("-f"), QStringLiteral("1"), QStringLiteral("-l"),
                        QStringLiteral("1"), QStringLiteral("-r"), QStringLiteral("72"),
                        QStringLiteral("-png"), output, png});
          QVERIFY(render.waitForFinished(60000));
          const QFileInfo root(png);
          const QStringList produced =
              QDir(root.absolutePath())
                  .entryList({root.fileName() + QStringLiteral("-*.png")}, QDir::Files);
          QVERIFY(!produced.isEmpty());
          const QImage image(QDir(root.absolutePath()).filePath(produced.first()));
          QVERIFY(!image.isNull());

          const auto anyRed = [&image](const QRect &area) {
              for (int y = area.top(); y <= area.bottom(); ++y) {
                  for (int x = area.left(); x <= area.right(); ++x) {
                      const QColor c = image.pixelColor(x, y);
                      if (image.valid(x, y) && c.red() > 150 && c.green() < 110 && c.blue() < 110)
                          return true;
                  }
              }
              return false;
          };

          // Осевая линии кончается в x = 200, торец доходит до 205.
          QVERIFY2(anyRed(QRect(201, 198, 2, 4)), "конец линии в экспорте не скруглён наружу");
          QVERIFY(!anyRed(QRect(207, 195, 4, 10)));
          // Точка - круг диаметром 10 вокруг (300, 300).
          QVERIFY2(anyRed(QRect(298, 298, 4, 4)), "точки нет в экспорте");
          QVERIFY(anyRed(QRect(296, 299, 1, 2)) && anyRed(QRect(303, 299, 1, 2)));
          QVERIFY(!anyRed(QRect(307, 295, 3, 10)));
      }
  ```

- [x] 6.7 **Прогон тестов до реализации (ожидаемый провал).**

  ```sh
  cmake --build build -j$(nproc) 2>&1 | grep -E "error:" | head -20
  ctest --test-dir build -R tst_export --output-on-failure
  ```

  Ожидается: сборка `tst_stroketessellator`, `tst_wetstroke`, `tst_canvasink`
  падает с `error: 'kCapPairs' is not a member of 'StrokeTessellator'`,
  `'CapEnd' is not a member of 'StrokeTessellator'`, `'capDirection' is not a member
  of 'StrokeTessellator'` (тесты опираются на интерфейс D4).
  `tst_export` собирается и падает в `roundEndsAndDotsAreExported` на
  `конец линии в экспорте не скруглён наружу` (плоский торец кончается в x = 200).

- [x] 6.8 **Интерфейс торца в `StrokeTessellator`.** `src/ink/stroketessellator.h`:
  заменить блок от комментария `// Поэлементная выдача - на ней стоит` до объявления
  `emitCap` включительно:

  ```cpp
      // Поэлементная выдача - на ней стоит инкрементальное наращивание «мокрого»
      // штриха: одна и та же математика стыка используется и при полной
      // тесселяции, и при дописывании хвоста.
      //
      // Торец штриха: пара вершин по нормали к направлению.
      void emitCap(QVector<Vertex> &strip, const QPointF &position, qreal halfWidth,
                   const QPointF &direction) const;
  ```

  на

  ```cpp
      // Поэлементная выдача - на ней стоит инкрементальное наращивание «мокрого»
      // штриха: одна и та же математика стыка используется и при полной
      // тесселяции, и при дописывании хвоста.
      //
      // Торец штриха - полудиск радиуса полуширины в точке (design.md, D4):
      // осевая кончается в точке касания и в точке отрыва, а торец выступает за
      // неё на полуширину. Полудиск собран из kCapPairs + 1 симметричных пар
      // вершин c + r·(∓cos αk·t ± sin αk·n), αk = (π/2)·k/K: пары сохраняют
      // чётность полосы, поэтому outline(), экспорт и мокрая полоса работают с
      // торцом без изменений. Пара с k = K - боковая, c ± r·n: у начального
      // торца она последняя, у конечного - первая, и её середина - центр торца.
      enum class CapEnd { Start, End };
      static constexpr int kCapPairs = 8;
      // Дальше стольких точек от конца хорда для направления торца не ищется:
      // столько же точек мокрый штрих держит под рукой, чтобы его торец совпал с
      // сухим, а работа на отсчёт не росла с длиной штриха.
      static constexpr int kCapLookback = 48;

      // direction - направление движения пера у этого конца.
      void emitCap(QVector<Vertex> &strip, const QPointF &position, qreal halfWidth,
                   const QPointF &direction, CapEnd end) const;
      // Направление торца - хорда до первой точки на расстоянии не меньше radius
      // от конца, но не дальше kCapLookback точек, а не последний отрезок: на
      // отрыве дотяжка до точки контакта дописывает отрезок короче шага со
      // случайным направлением. Вырожденный штрих - (1, 0).
      static QPointF capDirection(const QList<InkPoint> &points, CapEnd end, qreal radius);
  ```

- [x] 6.9 **Полудиск и хорда в `stroketessellator.cpp`.** Заменить функцию
  `StrokeTessellator::emitCap` целиком на две функции:

  ```cpp
  void StrokeTessellator::emitCap(QVector<Vertex> &strip, const QPointF &position, qreal halfWidth,
                                  const QPointF &direction, CapEnd end) const
  {
      // Начальный торец идёт от кончика позади точки касания к боковой паре,
      // конечный - от боковой пары к кончику за точкой отрыва: так торец
      // стыкуется с полосой без разворота пар. Кончик - пара из двух совпадающих
      // вершин, её треугольники нулевой площади и ничего не рисуют.
      const QPointF normal = normalOf(direction);
      const qreal sign = end == CapEnd::Start ? -1.0 : 1.0;
      for (int i = 0; i <= kCapPairs; ++i) {
          const int k = end == CapEnd::Start ? i : kCapPairs - i;
          const qreal angle = M_PI_2 * k / kCapPairs;
          const QPointF base = position + direction * (sign * halfWidth * std::cos(angle));
          const QPointF side = normal * (halfWidth * std::sin(angle));
          // Порядок в паре тот же, что у стыка: сначала «левая» сторона.
          strip.append(toVertex(base + side));
          strip.append(toVertex(base - side));
      }
  }

  QPointF StrokeTessellator::capDirection(const QList<InkPoint> &points, CapEnd end, qreal radius)
  {
      const int count = int(points.size());
      if (count < 2)
          return QPointF(1.0, 0.0);
      // k-я точка от того конца, у которого строится торец.
      const auto from = [&points, count, end](int k) {
          return end == CapEnd::Start ? points[k].position : points[count - 1 - k].position;
      };
      const QPointF tip = from(0);
      const int reach = qMin(count, kCapLookback);
      QPointF far = tip;
      for (int k = 1; k < reach; ++k) {
          far = from(k);
          if (std::hypot(far.x() - tip.x(), far.y() - tip.y()) >= radius)
              break;
      }
      // Направление движения пера: в начале - от касания вперёд, в конце - к отрыву.
      const QPointF direction = normalized(end == CapEnd::Start ? far - tip : tip - far);
      return direction.isNull() ? QPointF(1.0, 0.0) : direction;
  }
  ```

  Заменить функцию `StrokeTessellator::tessellate` целиком:

  ```cpp
  QVector<StrokeTessellator::Vertex> StrokeTessellator::tessellate(const QList<InkPoint> &points) const
  {
      QVector<Vertex> strip;
      if (points.size() < 2)
          return strip;

      // Направления сегментов. Вырожденные сегменты (две точки в одном месте)
      // наследуют направление предыдущего, иначе нормаль была бы неопределена.
      QList<QPointF> directions;
      directions.reserve(points.size() - 1);
      QPointF previous(1.0, 0.0);
      for (int i = 0; i + 1 < points.size(); ++i) {
          QPointF direction = normalized(points[i + 1].position - points[i].position);
          if (direction.isNull())
              direction = previous;
          directions.append(direction);
          previous = direction;
      }

      strip.reserve(points.size() * 4 + 4 * (kCapPairs + 1));

      const qreal startRadius = halfWidthAt(points.first());
      emitCap(strip, points.first().position, startRadius,
              capDirection(points, CapEnd::Start, startRadius), CapEnd::Start);
      for (int i = 1; i + 1 < points.size(); ++i) {
          emitJoint(strip, points[i].position, halfWidthAt(points[i]), directions[i - 1],
                    directions[i]);
      }
      const qreal endRadius = halfWidthAt(points.last());
      emitCap(strip, points.last().position, endRadius,
              capDirection(points, CapEnd::End, endRadius), CapEnd::End);

      return strip;
  }
  ```

  `outline()` и `isDegenerate()` не меняются.

- [x] 6.10 **Мокрый штрих: начальный торец по хорде, конечный - по окну.**
  `src/ink/wetstroke.h`: в секции `private:` после `void rebuildTail();` добавить

  ```cpp
      // Начальный торец можно класть в окончательную часть: хорда для его
      // направления больше не изменится.
      bool startCapIsSettled(const QList<InkPoint> &points) const;
  ```

  `src/ink/wetstroke.cpp`: в анонимном пространстве имён после функции
  `normalizedDirection` (перед `} // namespace`) вставить

  ```cpp
  // Последние точки осевой вместе с провизорным хвостом - не больше, чем
  // торец просматривает назад. tail[0] - последняя окончательная точка, она
  // берётся из хвоста. На отрыве окно совпадает с последними точками штриха,
  // поэтому мокрый торец совпадает с сухим вершина в вершину.
  QList<InkPoint> endCapWindow(const QList<InkPoint> &points, const QList<InkPoint> &tail)
  {
      const int lookback = StrokeTessellator::kCapLookback;
      const int fromTail = qMin(int(tail.size()), lookback);
      const int fromPoints = qMin(int(points.size()) - 1, lookback - fromTail);
      QList<InkPoint> window;
      window.reserve(fromPoints + fromTail);
      for (int i = int(points.size()) - 1 - fromPoints; i < int(points.size()) - 1; ++i)
          window.append(points[i]);
      for (int j = int(tail.size()) - fromTail; j < int(tail.size()); ++j)
          window.append(tail[j]);
      return window;
  }
  ```

  Заменить функцию `WetStroke::rebuildTail()` (вместе с комментарием над ней)
  целиком и добавить после неё `startCapIsSettled`:

  ```cpp
  // Дописывает стыки для окончательных точек, которые их ещё не получили, и
  // заново строит провизорную часть. Работа пропорциональна числу новых точек и
  // длине хвоста.
  void WetStroke::rebuildTail()
  {
      const QList<InkPoint> &points = m_sampler.points();
      const QList<InkPoint> &provisional = m_sampler.provisionalPoints();

      // Снимаем провизорную часть.
      m_strip.resize(m_settledVertices);
      if (points.isEmpty())
          return;

      // Направления новых окончательных сегментов.
      QPointF previous = m_directions.isEmpty() ? QPointF(1.0, 0.0) : m_directions.constLast();
      for (int i = m_directions.size(); i + 1 < points.size(); ++i) {
          previous = normalizedDirection(points[i].position, points[i + 1].position, previous);
          m_directions.append(previous);
      }

      if (m_jointedPoints == 0) {
          if (!startCapIsSettled(points)) {
              // Хорда для начального торца ещё не известна: вся полоса
              // провизорная и строится тем же правилом, что и сухая.
              QList<InkPoint> all = points;
              all.append(provisional);
              m_strip = m_tessellator.tessellate(all);
              return;
          }
          const qreal radius = m_tessellator.halfWidthAt(points.first());
          m_tessellator.emitCap(
              m_strip, points.first().position, radius,
              StrokeTessellator::capDirection(points, StrokeTessellator::CapEnd::Start, radius),
              StrokeTessellator::CapEnd::Start);
          m_jointedPoints = 1;
      }

      // Стык в точке i опирается на сегменты i-1 и i, поэтому становится
      // окончательным ровно тогда, когда появилась окончательная точка i+1.
      for (int i = m_jointedPoints; i + 1 < points.size(); ++i) {
          m_tessellator.emitJoint(m_strip, points[i].position, m_tessellator.halfWidthAt(points[i]),
                                  m_directions[i - 1], m_directions[i]);
      }
      m_jointedPoints = qMax(m_jointedPoints, int(points.size()) - 1);
      m_settledVertices = m_strip.size();

      // Провизорная часть: последняя окончательная точка, хвост за ней и торец.
      QList<InkPoint> tail;
      tail.reserve(provisional.size() + 1);
      tail.append(points.constLast());
      tail.append(provisional);

      QPointF incoming = m_directions.constLast();
      for (int j = 0; j + 1 < tail.size(); ++j) {
          const QPointF outgoing =
              normalizedDirection(tail[j].position, tail[j + 1].position, incoming);
          m_tessellator.emitJoint(m_strip, tail[j].position, m_tessellator.halfWidthAt(tail[j]),
                                  incoming, outgoing);
          incoming = outgoing;
      }
      const InkPoint &end = tail.constLast();
      const qreal radius = m_tessellator.halfWidthAt(end);
      m_tessellator.emitCap(m_strip, end.position, radius,
                            StrokeTessellator::capDirection(endCapWindow(points, tail),
                                                            StrokeTessellator::CapEnd::End, radius),
                            StrokeTessellator::CapEnd::End);
  }

  bool WetStroke::startCapIsSettled(const QList<InkPoint> &points) const
  {
      if (points.size() < 2)
          return false;
      // Хорда либо уже найдена среди первых kCapLookback точек, либо искать её
      // дальше незачем - так же решает StrokeTessellator::capDirection.
      if (points.size() >= StrokeTessellator::kCapLookback)
          return true;
      const QPointF start = points.constFirst().position;
      const qreal radius = m_tessellator.halfWidthAt(points.constFirst());
      for (int i = 1; i < points.size(); ++i) {
          const QPointF delta = points[i].position - start;
          if (std::hypot(delta.x(), delta.y()) >= radius)
              return true;
      }
      return false;
  }
  ```

  Почему работа на событие не растёт: до того как начальный торец лёг,
  окончательных точек не больше 48 (или они ещё не отошли на r0 от касания),
  `tessellate(all)` ограничен ими и хвостом; после - как прежде, только новые стыки
  и хвост, плюс `endCapWindow` и `capDirection` по ≤ 48 точкам.

- [x] 6.11 **Прогон тестов ink (ожидаемый проход).**

  ```sh
  cmake --build build -j$(nproc)
  ctest --test-dir build -R 'tst_stroketessellator|tst_wetstroke|tst_strokesampler' --output-on-failure
  ```

  Ожидается: все три зелёные. `perPointWorkDoesNotGrowWithStrokeLength`
  проходит с отношением 28/72 ≈ 0.39 (было 28/56 = 0.50: в первую сотню событий
  теперь входит начальный торец в 18 вершин; порог теста 0.33). Если отношение
  упало ниже 0.33 - работа на событие стала расти, искать в `endCapWindow` или
  в ветке `startCapIsSettled`, а не править порог.

- [x] 6.12 **Прогон холста, экспорта и графа сцены.**

  ```sh
  ctest --test-dir build -R 'tst_canvasink|tst_export|tst_pagescenegraph|tst_documentcanvas' --output-on-failure
  ```

  Ожидается: все зелёные; `tst_export::roundEndsAndDotsAreExported` проходит,
  `tst_canvasink::finishedStrokeIsTheSameEverywhere` проходит с центром дуги в
  точке отрыва (< 1e-3), `tst_pagescenegraph::wetStrokeNodesFollowShrinkingStrip`
  по-прежнему видит укорачивание полосы и второй кусок (`peak > 512`).

- [x] 6.13 **Полный прогон и коммит.**

  ```sh
  ctest --test-dir build --output-on-failure
  git add src/ink/stroketessellator.h src/ink/stroketessellator.cpp \
          src/ink/wetstroke.h src/ink/wetstroke.cpp \
          tests/tst_stroketessellator.cpp tests/tst_wetstroke.cpp \
          tests/tst_canvasink.cpp tests/tst_export.cpp
  git commit -F - <<'EOF'
  Круглые торцы штриха

  Торец - полудиск радиуса полуширины из восьми пар вершин на четверть
  оборота, направление - по хорде длиной не меньше радиуса (design.md, D4).
  Чётность полосы сохраняется: outline(), экспорт и мокрая полоса работают
  без изменений, сухая полоса совпадает с мокрой вершина в вершину.

  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  EOF
  ```

  Ожидается: `100% tests passed` (кроме `tst_idleframes`, который без сессии
  сообщает SKIP), коммит создан.

---

## 7. Точка от касания и отбрасывание задева (D5)

Правило точки живёт в тесселяторе и зависит только от сохранённых точек: если длина
осевой меньше полной толщины пера `width()` (`StrokeTessellator::isDot`), рисуется
капсула от первой до последней точки с наибольшей полушириной среди точек, при
совпадающих концах - диск. Толщину точке даёт `WetStroke`: к такому штриху на
`finish()` дописывается одна новая последняя точка в положении последней с
наибольшим отфильтрованным давлением касания (`StrokeSampler::peakPressure()`) -
окончательные точки не меняются (прототип `outline-caps`). Касание начинается и
кончается почти без нажима, а при сдвиге 0.3-3 px у штриха только две точки -
края касания; без пиковой точки капсула вышла бы тоньше нажима (у `Tapered` - в
разы). Касание из одной точки - частный случай: вторая точка - копия с пиком
(`Stroke::isValid` требует двух). `WetStroke::finish()` отбрасывает случайный
задев: давление есть, касание короче 30 мс и наибольшее давление ниже 0.05. Мышь
давления не сообщает - её касание не отбрасывается.

Мокрая точка во время касания. Сейчас при касании без движения у сэмплера одна
окончательная точка и пустой хвост, `rebuildTail()` выходит на `points + provisional
< 2`, полоса пуста, и `DocumentCanvas::updateWetInkNodes` снимает мокрый узел по
`strip.size() < 4`. Решение: показывать точку делает `WetStroke`, а не холст. Пока
начальный торец не лёг (штрих ещё может оказаться точкой: длина окончательной
осевой меньше `width()`), мокрая полоса - `tessellate(withDotTwin(points +
provisional))`: к точке дописывается та же пиковая точка, что допишет `finish()`, -
с пиком нажима на этот момент. Поэтому толщина мокрой точки во время касания
та же, что у сухой после отрыва. Диск - 36 вершин, проверка `strip.size() < 4` в `DocumentCanvas`
остаётся как есть (она означает «полоса пуста») и мокрый узел появляется с первого
отсчёта; `DocumentCanvas` не меняется. Начальный торец становится окончательным
только когда длина окончательной осевой достигла `width()` - тогда и сухая полоса
уже не точка, поэтому на отрыве мокрая полоса совпадает с сухой в обоих случаях.
Длина копится в `m_settledArc` в том же порядке сложения, что и в
`StrokeTessellator::isDot`.

Касание элементов окна. Кнопки, значок вкладыша и пункты оглавления забирают
нажатие в QML, до `DocumentCanvas::event` оно не доходит - так и остаётся. В
`tst_penonlymode` и `tst_insertoverlay` это уже проверяется `strokeCount() == 0`
после касания элемента, но сейчас касание не ставит точек вообще, и проверка ничего
не доказывает. Поэтому в оба теста добавляется контроль: то же касание по свободному
месту страницы ставит ровно одну точку.

**Files:**
- Modify: `src/ink/stroketessellator.h`, `src/ink/stroketessellator.cpp`
- Modify: `src/ink/strokesampler.h`, `src/ink/strokesampler.cpp`
- Modify: `src/ink/wetstroke.h`, `src/ink/wetstroke.cpp`
- Test: `tests/tst_stroketessellator.cpp`, `tests/tst_strokesampler.cpp`,
  `tests/tst_wetstroke.cpp`, `tests/tst_canvasink.cpp`, `tests/tst_penonlymode.cpp`,
  `tests/tst_insertoverlay.cpp`

- [x] 7.1 **Тест тесселятора: капсула с наибольшей полушириной.**
  `tests/tst_stroketessellator.cpp`: в анонимном пространстве имён после
  `boundsOf` добавить

  ```cpp
  qreal distanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
  {
      return distanceToPolyline(p, {{a, 0.5, 0.0}, {b, 0.5, 0.0}});
  }
  ```

  и перед `void degenerateInputIsHandled()` вставить

  ```cpp
      // --- ink-beauty-pass, D5: точка от касания --------------------------------

      // Сценарий «Касание с едва заметным сдвигом»: осевая короче толщины пера -
      // капсула с наибольшей полушириной, а не чёрточка. Штрих длиннее толщины -
      // обычный, торцы по своим полуширинам.
      void strokeShorterThanPenWidthIsCapsule()
      {
          StrokeTessellator tessellator;
          tessellator.setWidth(3.0);

          const QList<InkPoint> tap = {{QPointF(10.0, 10.0), 0.2, 0.0},
                                       {QPointF(10.5, 10.0), 0.9, 0.01},
                                       {QPointF(11.0, 10.0), 0.3, 0.02}};
          const qreal r = tessellator.halfWidthAt(tap[1]);
          const QVector<Vertex> strip = tessellator.tessellate(tap);
          QVERIFY(!StrokeTessellator::isDegenerate(strip));
          for (const Vertex &vertex : strip) {
              const qreal d = distanceToSegment(toPoint(vertex), tap.constFirst().position,
                                                tap.constLast().position);
              QVERIFY2(qAbs(d - r) < 1e-4,
                       qPrintable(QStringLiteral("вершина в %1 от оси капсулы, радиус %2")
                                      .arg(d).arg(r)));
          }
          const QRectF bounds = boundsOf(strip);
          QVERIFY(qAbs(bounds.width() - (1.0 + 2.0 * r)) < 1e-4);
          QVERIFY(qAbs(bounds.height() - 2.0 * r) < 1e-4);

          // Осевая 4.5 при толщине 3 - уже не точка: начальный торец по своей
          // полуширине, а не по наибольшей.
          const QList<InkPoint> line = {{QPointF(10.0, 10.0), 0.2, 0.0},
                                        {QPointF(12.25, 10.0), 0.9, 0.01},
                                        {QPointF(14.5, 10.0), 0.3, 0.02}};
          const QVector<Vertex> longer = tessellator.tessellate(line);
          const qreal r0 = tessellator.halfWidthAt(line.constFirst());
          QVERIFY(QLineF(toPoint(longer.constFirst()), QPointF(10.0 - r0, 10.0)).length() < 1e-4);
      }
  ```

- [x] 7.2 **Тест сэмплера: длительность касания и пик давления.**
  `tests/tst_strokesampler.cpp`: перед `void resetClearsEverything()` вставить

  ```cpp
      // ink-beauty-pass, D5: касанию нужны длительность по меткам отсчётов и
      // наибольшее отфильтрованное давление - по ним WetStroke отбрасывает
      // задев и даёт точке толщину. Сам сэмплер точку не дописывает: касание без
      // движения у него - одна точка.
      void tapReportsDurationAndPeakPressure()
      {
          StrokeSampler sampler;
          const QPointF at(30, 40);
          sampler.append({at, 0.2, 1.000});
          sampler.append({at, 0.6, 1.010});
          sampler.append({at, 0.4, 1.020});
          QVERIFY(qAbs(sampler.contactDuration() - 0.020) < 1e-9);
          const qreal peak = sampler.peakPressure();
          QVERIFY2(peak > 0.2 && peak <= 0.6, qPrintable(QStringLiteral("пик %1").arg(peak)));
          for (const InkPoint &point : sampler.points())
              QVERIFY(point.pressure <= peak + 1e-12);

          sampler.finish();
          QCOMPARE(sampler.points().size(), 1);
          QCOMPARE(sampler.points()[0].position, at);
          QCOMPARE(sampler.peakPressure(), peak);

          // Без давления пика нет.
          StrokeSampler mouse;
          mouse.append({at, -1.0, 2.0});
          mouse.finish();
          QCOMPARE(mouse.peakPressure(), -1.0);
          QCOMPARE(mouse.contactDuration(), 0.0);
      }
  ```

- [x] 7.3 **Тесты мокрого штриха: точка видна во время касания, задев, капсула,
  толщина точки со сдвигом.** `tests/tst_wetstroke.cpp`: после `#include <QLineF>`
  добавить `#include <QPolygonF>`; в анонимном пространстве имён перед `sameStrip`
  добавить

  ```cpp
  qreal distanceToSegment(const QPointF &p, const QLineF &segment)
  {
      const QPointF ab = segment.p2() - segment.p1();
      const qreal lengthSquared = QPointF::dotProduct(ab, ab);
      const qreal t = lengthSquared < 1e-12
          ? 0.0
          : qBound(0.0, QPointF::dotProduct(p - segment.p1(), ab) / lengthSquared, 1.0);
      return QLineF(p, segment.p1() + ab * t).length();
  }
  ```

  Удалить функцию `singleTapProducesNoStroke()` целиком и на её месте вставить:

  ```cpp
      // --- ink-beauty-pass, D5: точка от касания --------------------------------

      // Сценарий «Точка над буквой»: касание без движения оставляет точку, и
      // видна она уже во время касания, а не только после отрыва.
      void tapWithoutMovementGivesDot()
      {
          WetStroke wet;
          wet.begin(0, prototype());
          const QPointF at(10, 10);
          wet.addPoint({at, 0.5, 0.0});

          // Мокрый диск под пером.
          QCOMPARE(wet.strip().size(), 2 * kCapVertices);
          const qreal r = wet.tessellator().halfWidthAt({at, 0.5, 0.0});
          for (const WetStroke::Vertex &vertex : wet.strip())
              QVERIFY(qAbs(QLineF(toPoint(vertex), at).length() - r) < 1e-4);

          wet.addPoint({at, 0.7, 0.04});
          const Stroke stroke = wet.finish();
          QVERIFY(stroke.isValid());
          QCOMPARE(stroke.points.size(), 2);
          QCOMPARE(stroke.points[0].position, at);
          QCOMPARE(stroke.points[1].position, at);
          // Толщина точки - по самому сильному отфильтрованному нажиму касания.
          QVERIFY(stroke.points[1].pressure > 0.5);

          // Мокрая полоса на отрыве - та же, что сухая.
          StrokeTessellator tessellator;
          tessellator.setWidth(stroke.width);
          tessellator.setStyle(stroke.style);
          QVERIFY(sameStrip(wet.strip(), tessellator.tessellate(stroke.points)));
      }

      // Сценарий «Случайный задев»: короче 30 мс и легче 0.05 - следа нет.
      // Достаточно одного из двух - долгого касания или нажима, - и точка
      // остаётся. У мыши давления нет, её касание не отбрасывается.
      void onlyShortLightTouchIsDiscarded_data()
      {
          QTest::addColumn<qreal>("pressure");
          QTest::addColumn<qreal>("duration");
          QTest::addColumn<bool>("kept");
          QTest::newRow("задев") << 0.03 << 0.01 << false;
          QTest::newRow("короткое с нажимом") << 0.5 << 0.01 << true;
          QTest::newRow("долгое лёгкое") << 0.03 << 0.05 << true;
          QTest::newRow("мышь") << -1.0 << 0.01 << true;
      }

      void onlyShortLightTouchIsDiscarded()
      {
          QFETCH(qreal, pressure);
          QFETCH(qreal, duration);
          QFETCH(bool, kept);

          WetStroke wet;
          wet.begin(0, prototype());
          for (qreal t = 0.0; t <= duration + 1e-9; t += 0.005)
              wet.addPoint({QPointF(10, 10), pressure, t});
          const Stroke stroke = wet.finish();
          QCOMPARE(stroke.isValid(), kept);
          if (!kept)
              QVERIFY(wet.strip().isEmpty());
      }

      // Сценарий «Касание с едва заметным сдвигом»: перо сдвинулось на 0.5 при
      // толщине 3 - капсула одной толщины, а не чёрточка с тонким заходом.
      void tinyMovementGivesCapsule()
      {
          WetStroke wet;
          wet.begin(0, prototype(3.0));
          const qreal pressures[] = {0.2, 0.5, 0.8, 0.8, 0.7, 0.6, 0.5};
          for (int i = 0; i < 7; ++i)
              wet.addPoint({QPointF(10.0 + 0.5 * i / 6.0, 10.0), pressures[i], i / 200.0});
          const Stroke stroke = wet.finish();
          QVERIFY(stroke.isValid());

          // Радиус капсулы - наибольшая полуширина среди точек штриха.
          qreal r = 0.0;
          for (const InkPoint &point : stroke.points)
              r = qMax(r, wet.tessellator().halfWidthAt(point));
          const QLineF axis(stroke.points.constFirst().position, stroke.points.constLast().position);
          for (const WetStroke::Vertex &vertex : wet.strip()) {
              const qreal d = distanceToSegment(toPoint(vertex), axis);
              QVERIFY2(qAbs(d - r) < 1e-4,
                       qPrintable(QStringLiteral("вершина в %1 от оси, радиус капсулы %2").arg(d).arg(r)));
          }

          StrokeTessellator tessellator;
          tessellator.setWidth(stroke.width);
          tessellator.setStyle(stroke.style);
          QVERIFY(sameStrip(wet.strip(), tessellator.tessellate(stroke.points)));
      }

      // Сценарий «Касание с едва заметным сдвигом» при живом нажиме: касание
      // начинается и кончается почти без давления, а точка обязана быть толщиной
      // в нажим касания - по его пику, и во время касания, и после отрыва.
      void shiftedTapHasPeakWidth_data()
      {
          QTest::addColumn<qreal>("shift");
          QTest::newRow("0.3") << 0.3;
          QTest::newRow("1.5") << 1.5;
          QTest::newRow("3.0") << 3.0;
      }

      void shiftedTapHasPeakWidth()
      {
          QFETCH(qreal, shift);
          // Толщина 4 - все сдвиги короче её; касание 40 мс на частоте пера.
          const qreal width = 4.0;
          QList<InkPoint> input;
          for (int k = 0; k <= 19; ++k) {
              const qreal u = k / 19.0;
              input.append({QPointF(10.0 + shift * u, 10.0), 0.05 + 0.55 * std::sin(M_PI * u),
                            k / 481.0});
          }
          StrokeSampler reference;
          for (const InkPoint &point : input)
              reference.append(point);
          const qreal peak = reference.peakPressure();

          WetStroke wet;
          wet.begin(0, prototype(width));
          for (const InkPoint &point : input)
              wet.addPoint(point);
          const qreal r = wet.tessellator().halfWidthAt({QPointF(), peak, 0.0});
          // Во время касания: мокрая капсула уже толщиной по пику.
          QPolygonF during;
          for (const WetStroke::Vertex &vertex : wet.strip())
              during.append(toPoint(vertex));
          QVERIFY2(qAbs(during.boundingRect().height() - 2.0 * r) <= 0.05 * 2.0 * r,
                   qPrintable(QStringLiteral("мокрая точка высотой %1, ждали %2")
                                  .arg(during.boundingRect().height()).arg(2.0 * r)));

          const Stroke stroke = wet.finish();
          QVERIFY(stroke.isValid());
          // После отрыва: последняя точка - с пиком нажима, капсула - его толщины.
          QCOMPARE(stroke.points.constLast().pressure, peak);
          const QLineF axis(stroke.points.constFirst().position, stroke.points.constLast().position);
          for (const WetStroke::Vertex &vertex : wet.strip()) {
              const qreal d = distanceToSegment(toPoint(vertex), axis);
              QVERIFY2(qAbs(d - r) <= 0.05 * r,
                       qPrintable(QStringLiteral("вершина в %1 от оси, радиус по пику %2").arg(d).arg(r)));
          }
          StrokeTessellator tessellator;
          tessellator.setWidth(stroke.width);
          tessellator.setStyle(stroke.style);
          QVERIFY(sameStrip(wet.strip(), tessellator.tessellate(stroke.points)));
      }
  ```

- [x] 7.4 **Тесты холста: точка, задев, сохранение, экспорт и ластик.**
  `tests/tst_canvasink.cpp`: после `#include <QMouseEvent>` добавить
  `#include <QLineF>`. Удалить функцию `tapWithoutMovementLeavesNoStroke()` целиком и
  на её месте вставить:

  ```cpp
      // ink-beauty-pass, D5, сценарий «Точка над буквой»: касание пером без
      // движения оставляет точку - видимую уже во время касания, одним штрихом
      // в истории и в геометрии страницы.
      void tapWithoutMovementLeavesDot()
      {
          m_canvas.goToPage(0);
          const QPointF at(450, 350);
          const int page = m_canvas.pageAtView(at);
          QVERIFY(page >= 0);

          QVERIFY(m_canvas.beginInkAt(at, 0.5, false));
          // Мокрая точка видна под пером до отрыва: диск из двух полудисков.
          QVERIFY2(m_canvas.wetStripVertexCount() >= 4 * (StrokeTessellator::kCapPairs + 1),
                   qPrintable(QStringLiteral("мокрая полоса: %1 вершин")
                                  .arg(m_canvas.wetStripVertexCount())));
          m_canvas.endInk();

          QCOMPARE(m_ink.strokeCount(), 1);
          const Stroke dot = m_ink.allStrokes().first().stroke;
          QCOMPARE(dot.points.size(), 2);
          QCOMPARE(dot.points[0].position, dot.points[1].position);
          // Модель штриха считает во float: точка касания - с точностью до 1e-3.
        QVERIFY(QLineF(dot.points[0].position, m_canvas.viewToPage(page, at)).length() < 1e-3);
          QCOMPARE(m_canvas.inkGeometryStrokeCount(page), 1);

          // Отмена и повтор - как у любого штриха.
          m_ink.undo();
          QCOMPARE(m_ink.strokeCount(), 0);
          QCOMPARE(m_canvas.inkGeometryStrokeCount(page), 0);
          m_ink.redo();
          QCOMPARE(m_ink.strokeCount(), 1);
          QCOMPARE(m_canvas.inkGeometryStrokeCount(page), 1);
      }

      // Сценарий «Случайный задев»: касание короче 30 мс с давлением ниже 0.05
      // следа не оставляет. Мышь давления не сообщает - её щелчок ставит точку.
      void accidentalGrazeLeavesNothing()
      {
          m_canvas.goToPage(0);
          const QPointF at(450, 350);
          QVERIFY(m_canvas.beginInkAt(at, 0.03, false, 20.000));
          m_canvas.continueInkAt(at + QPointF(0.2, 0.0), 0.04, 20.010);
          m_canvas.endInk();
          QCOMPARE(m_ink.strokeCount(), 0);
          QVERIFY(!m_canvas.isDrawing());

          QVERIFY(m_canvas.beginInkAt(at, -1.0, false, 21.000));
          m_canvas.endInk();
          QCOMPARE(m_ink.strokeCount(), 1);
      }

      // Сценарий «Точка над буквой»: точка сохраняется в заметке, в контуре
      // экспорта она круг, и ластик стирает её, как любой штрих.
      void dotIsSavedExportedAndErased()
      {
          QTemporaryDir dir;
          QVERIFY(dir.isValid());
          const QString notePath = dir.filePath(QStringLiteral("dot.uqn"));
          NoteStore store;
          QVERIFY(store.create(notePath, fixtures::textbook()));
          store.attach(&m_ink);

          m_canvas.goToPage(0);
          const QPointF at(450, 350);
          QVERIFY(m_canvas.beginInkAt(at, 0.6, false, 30.0));
          m_canvas.endInk();
          QCOMPARE(m_ink.strokeCount(), 1);
          const Stroke dot = m_ink.allStrokes().first().stroke;

          QVERIFY(store.flush());
          {
              NoteStore reopened;
              InkDocument restored;
              QVERIFY(reopened.openNote(notePath));
              QVERIFY(reopened.load(&restored));
              QCOMPARE(restored.strokeCount(), 1);
              const QList<InkPoint> points = restored.allStrokes().first().stroke.points;
              QCOMPARE(points.size(), 2);
              for (const InkPoint &point : points)
                  QVERIFY(QLineF(point.position, dot.points.first().position).length() < 1e-3);
          }

          // Контур экспорта - круг диаметром в толщину точки.
          StrokeTessellator tessellator;
          tessellator.setWidth(dot.width);
          tessellator.setStyle(dot.style);
          const QRectF bounds = tessellator.outline(dot.points).boundingRect();
          const qreal diameter = 2.0 * tessellator.halfWidthAt(dot.points.last());
          QVERIFY(qAbs(bounds.width() - diameter) < 1e-3);
          QVERIFY(qAbs(bounds.height() - diameter) < 1e-3);

          m_canvas.setTool(DocumentCanvas::Tool::Eraser);
          drawLine(&m_canvas, at + QPointF(-6, 0), at + QPointF(6, 0));
          QCOMPARE(m_ink.strokeCount(), 0);
      }
  ```

- [x] 7.5 **Тест «Только перо»: касание элементов пером не ставит точку.**
  `tests/tst_penonlymode.cpp`: после функции `fingerTapsOnControlsAreIgnoredPenTapsWork()`
  вставить

  ```cpp
      // ink-beauty-pass, D5, сценарий «Касание элемента окна пером»: касание
      // пером кнопки, значка вкладыша или пункта оглавления срабатывает на
      // элементе, а точки на странице не оставляет - без режима и в режиме
      // «Только перо». Контроль: то же касание по странице точку оставляет,
      // значит, точек не было потому, что касание забрал элемент.
      void penTapsOnControlsLeaveNoDot()
      {
          InkDocument *ink = m_canvas->ink();
          QVERIFY(ink);
          const quint64 insert = ink->addInsert(0, QRectF(120, 160, 200, 140));
          ink->setInsertOpen(insert, false);
          QCoreApplication::processEvents();

          QQuickItem *toggle = button(QStringLiteral("penOnlyButton"));
          QQuickItem *anchor = button(QStringLiteral("insertAnchor"));
          QVERIFY(toggle);
          QVERIFY(anchor);
          QQuickItem *conclusion = nullptr;
          QTRY_VERIFY((conclusion = findVisibleText(m_window->contentItem(),
                                                    QStringLiteral("4 Conclusion"))));
          QCOMPARE(m_canvas->tool(), DocumentCanvas::Tool::Pen);

          // Без режима: кнопка панели.
          penClick(m_window, centerOf(toggle));
          QVERIFY(m_guard->penOnly());
          QCOMPARE(ink->strokeCount(), 0);

          // В режиме: значок вкладыша и пункт оглавления.
          penClick(m_window, centerOf(anchor));
          QVERIFY(ink->insertById(insert).open);
          QCOMPARE(ink->strokeCount(), 0);
          penClick(m_window, centerOf(conclusion));
          QTRY_COMPARE(m_canvas->currentPage(), 12);
          QCOMPARE(ink->strokeCount(), 0);

          // Контроль: касание страницы тем же пером - точка.
          settle();
          QVERIFY(m_canvas->pageAtView(m_canvas->mapFromScene(canvasCenter())) >= 0);
          penClick(m_window, canvasCenter());
          QCOMPARE(ink->strokeCount(), 1);
      }
  ```

- [x] 7.6 **Тест вкладышей: контроль щелчка мимо элементов.**
  `tests/tst_insertoverlay.cpp`, функция `controlsDriveInsertsWithoutInkingThePage()`:
  в самый конец функции, после строки
  `QCOMPARE(ink->strokesInInsert(id).size(), 1);`, добавить

  ```cpp
          // ink-beauty-pass, D5. Контроль сценария «Касание значка не рисует»:
          // тот же щелчок по странице мимо элементов вкладыша ставит точку -
          // значит, выше точки не было потому, что касание забрал значок.
          QCoreApplication::processEvents();
          const QPointF free = canvas->documentToView(canvas->layout().pageRect(0).topLeft()
                                                      + QPointF(100, 450));
          QCOMPARE(canvas->pageAtView(free), 0);
          QCOMPARE(canvas->insertAtView(free), quint64(0));
          click(window, scene(free));
          QCOMPARE(ink->allStrokes().size(), 1);
  ```

- [x] 7.7 **Прогон тестов до реализации (ожидаемый провал).**

  ```sh
  cmake --build build -j$(nproc) 2>&1 | grep -E "error:" | head
  ctest --test-dir build -R 'tst_stroketessellator|tst_wetstroke|tst_canvasink|tst_penonlymode|tst_insertoverlay' --output-on-failure
  ```

  Ожидается:
  - сборка `tst_strokesampler` и `tst_wetstroke` падает: `'class StrokeSampler' has
    no member named 'contactDuration'` и `... 'peakPressure'` (в `tst_wetstroke`
    пик берёт `shiftedTapHasPeakWidth`);
  - `tst_stroketessellator::strokeShorterThanPenWidthIsCapsule` - FAIL
    `вершина в 0.6 от оси капсулы, радиус 1.3875` (торцы по своим полуширинам);
  - когда `contactDuration`/`peakPressure` объявлены (7.9) и сборка проходит, а
    `WetStroke` ещё прежний, `tst_wetstroke` падает так (проверено в scratchpad):
    `tapWithoutMovementGivesDot` - на `QCOMPARE(wet.strip().size(), 2 * kCapVertices)`
    (0 против 36: полоса из одной точки пуста); `onlyShortLightTouchIsDiscarded` - в
    строках «короткое с нажимом», «долгое лёгкое», «мышь» (`stroke.isValid()` -
    false: одна точка); `tinyMovementGivesCapsule` - `вершина в 0.6 от оси, радиус
    капсулы 0.9375`; `shiftedTapHasPeakWidth` - во всех трёх строках на
    `QCOMPARE(stroke.points.constLast().pressure, peak)` (с фильтром давления шага 1
    капсула к тому же тоньше пика);
  - `tst_canvasink::tapWithoutMovementLeavesDot` - FAIL на `wetStripVertexCount()`
    (0); `accidentalGrazeLeavesNothing` - FAIL на последнем
    `QCOMPARE(m_ink.strokeCount(), 1)`; `dotIsSavedExportedAndErased` - FAIL на
    `QCOMPARE(m_ink.strokeCount(), 1)`;
  - `tst_penonlymode::penTapsOnControlsLeaveNoDot` и
    `tst_insertoverlay::controlsDriveInsertsWithoutInkingThePage` - FAIL на
    контрольном касании страницы (0 вместо 1).

- [x] 7.8 **Точка в тесселяторе.** `src/ink/stroketessellator.h`: перед
  комментарием `// Полуширина штриха в конкретной точке` (публичная секция)
  вставить

  ```cpp
      // Точка (design.md, D5): осевая короче полной толщины пера. Такой штрих
      // tessellate() рисует капсулой, а WetStroke дописывает к нему точку с
      // наибольшим нажимом касания - решение у обоих одно.
      bool isDot(const QList<InkPoint> &points) const;

  ```

  и в секции `private:` перед `qreal m_width = 2.4;` вставить

  ```cpp
      // Капсула от первой до последней точки с наибольшей полушириной; при
      // совпадающих концах - диск.
      void emitDot(QVector<Vertex> &strip, const QList<InkPoint> &points) const;
  ```

  `src/ink/stroketessellator.cpp`: после функции `capDirection` добавить

  ```cpp
  bool StrokeTessellator::isDot(const QList<InkPoint> &points) const
  {
      // Длина осевой копится в том же порядке, что и у мокрого штриха
      // (WetStroke::rebuildTail): решение «точка или нет» обязано совпасть.
      qreal arc = 0.0;
      for (int i = 1; i < points.size(); ++i) {
          const QPointF delta = points[i].position - points[i - 1].position;
          arc += std::hypot(delta.x(), delta.y());
          if (arc >= m_width)
              return false;
      }
      return true;
  }

  void StrokeTessellator::emitDot(QVector<Vertex> &strip, const QList<InkPoint> &points) const
  {
      // Толщина точки - по самому сильному нажиму касания, а не по слабым его
      // краям: касание начинается и кончается почти без давления.
      qreal radius = 0.0;
      for (const InkPoint &point : points)
          radius = qMax(radius, halfWidthAt(point));
      const QPointF from = points.constFirst().position;
      const QPointF to = points.constLast().position;
      QPointF direction = normalized(to - from);
      if (direction.isNull())
          direction = QPointF(1.0, 0.0);
      // Два полудиска: при совпадающих концах они складываются в диск.
      emitCap(strip, from, radius, direction, CapEnd::Start);
      emitCap(strip, to, radius, direction, CapEnd::End);
  }
  ```

  В `StrokeTessellator::tessellate` сразу после

  ```cpp
      QVector<Vertex> strip;
      if (points.size() < 2)
          return strip;
  ```

  вставить

  ```cpp

      // Касание без движения или с едва заметным сдвигом - точка, а не
      // чёрточка (design.md, D5). Правило зависит только от сохранённых точек,
      // поэтому сухой вид совпадает с мокрым и с тем, что откроется из файла.
      if (isDot(points)) {
          strip.reserve(4 * (kCapPairs + 1));
          emitDot(strip, points);
          return strip;
      }
  ```

  Прогон:

  ```sh
  cmake --build build -j$(nproc) --target tst_stroketessellator && ctest --test-dir build -R tst_stroketessellator --output-on-failure
  ```

  Ожидается: зелёный.

- [x] 7.9 **Пик давления и длительность касания в сэмплере.**
  `src/ink/strokesampler.h`: после `bool isEmpty() const { return m_points.isEmpty(); }`
  вставить

  ```cpp

      // Касание (design.md, D5). Длительность - от первого до последнего
      // принятого отсчёта по их собственным меткам времени, в секундах.
      qreal contactDuration() const { return m_started ? m_lastInputTime : 0.0; }
      // Наибольшее отфильтрованное давление за касание; -1, если устройство
      // давления не сообщает. Давление любой точки штриха не больше него.
      qreal peakPressure() const { return m_peakPressure; }
  ```

  и после поля `qreal m_lastInputPressure = -1.0;` добавить поле

  ```cpp
      qreal m_peakPressure = -1.0;
  ```

  `src/ink/strokesampler.cpp`:
  - в `StrokeSampler::reset()` после `m_lastInputPressure = -1.0;` добавить
    `m_peakPressure = -1.0;`;
  - в `StrokeSampler::append()` после `m_lastInputPressure = pressure;` (блок
    после успешного `m_modeler->Update`) добавить
    `m_peakPressure = qMax(m_peakPressure, pressure);` - отфильтрованное давление
    этого отсчёта; без давления оно -1 и пик остаётся -1.

  `finish()` не меняется: точку к касанию дописывает `WetStroke` (7.10).

  Прогон:

  ```sh
  cmake --build build -j$(nproc) --target tst_strokesampler && ctest --test-dir build -R tst_strokesampler --output-on-failure
  ```

  Ожидается: зелёный, включая `tapReportsDurationAndPeakPressure`,
  `finishEndsExactlyAtLastContactPoint` и `resetClearsEverything`.

- [x] 7.10 **Мокрая точка и задев в `WetStroke`.** `src/ink/wetstroke.h`: заменить
  комментарий над `startCapIsSettled`

  ```cpp
      // Начальный торец можно класть в окончательную часть: хорда для его
      // направления больше не изменится.
  ```

  на

  ```cpp
      // Начальный торец можно класть в окончательную часть: штрих уже не станет
      // точкой, и хорда для направления торца больше не изменится.
  ```

  после объявления `startCapIsSettled` добавить

  ```cpp
      // Точка (design.md, D5): если осевая короче толщины пера, к ней дописывается
      // ещё одна точка в положении последней с наибольшим нажимом касания.
      // Окончательные точки не меняются - только добавляется одна.
      QList<InkPoint> withDotTwin(QList<InkPoint> points) const;
  ```

  и после поля `int m_jointedPoints = 0;` добавить

  ```cpp
      qreal m_settledArc = 0.0;    // длина окончательной осевой
  ```

  `src/ink/wetstroke.cpp`:
  - в анонимном пространстве имён перед `QPointF normalizedDirection(` вставить

    ```cpp
    // Порог случайного задева (design.md, D5).
    constexpr qreal kGrazeSeconds = 0.030;
    constexpr qreal kGrazePressure = 0.05;

    ```

  - в `WetStroke::reset()` после `m_jointedPoints = 0;` добавить `m_settledArc = 0.0;`;
  - в `rebuildTail()` заменить цикл направлений

    ```cpp
        // Направления новых окончательных сегментов.
        QPointF previous = m_directions.isEmpty() ? QPointF(1.0, 0.0) : m_directions.constLast();
        for (int i = m_directions.size(); i + 1 < points.size(); ++i) {
            previous = normalizedDirection(points[i].position, points[i + 1].position, previous);
            m_directions.append(previous);
        }
    ```

    на

    ```cpp
        // Направления новых окончательных сегментов и длина окончательной осевой.
        QPointF previous = m_directions.isEmpty() ? QPointF(1.0, 0.0) : m_directions.constLast();
        for (int i = m_directions.size(); i + 1 < points.size(); ++i) {
            const QPointF delta = points[i + 1].position - points[i].position;
            m_settledArc += std::hypot(delta.x(), delta.y());
            previous = normalizedDirection(points[i].position, points[i + 1].position, previous);
            m_directions.append(previous);
        }
    ```

  - там же заменить ветку провизорной полосы

    ```cpp
            if (!startCapIsSettled(points)) {
                // Хорда для начального торца ещё не известна: вся полоса
                // провизорная и строится тем же правилом, что и сухая.
                QList<InkPoint> all = points;
                all.append(provisional);
                m_strip = m_tessellator.tessellate(all);
                return;
            }
    ```

    на

    ```cpp
            if (!startCapIsSettled(points)) {
                // Штрих ещё может оказаться точкой (design.md, D5), и хорда для
                // начального торца ещё не известна: вся полоса провизорная и
                // строится тем же правилом, что и сухая. Касание показывает точку
                // сразу, а не пустое место до отрыва, и той толщины, которой её
                // оставит finish(), - по наибольшему нажиму касания.
                QList<InkPoint> all = points;
                all.append(provisional);
                m_strip = m_tessellator.tessellate(withDotTwin(all));
                return;
            }
    ```

  - в `startCapIsSettled()` заменить

    ```cpp
        if (points.size() < 2)
            return false;
    ```

    на

    ```cpp
        // Точкой штрих перестаёт быть навсегда: окончательные точки только
        // добавляются, длина осевой только растёт.
        if (points.size() < 2 || m_settledArc < m_tessellator.width())
            return false;
    ```

  - заменить функцию `WetStroke::finish()` целиком:

    ```cpp
    Stroke WetStroke::finish()
    {
        if (!m_active)
            return {};

        m_sampler.finish();
        rebuildTail();
        m_active = false;

        // Случайный задев, а не точка: короче 30 мс и почти без нажима
        // (design.md, D5). У мыши давления нет, её касание не отбрасывается.
        const qreal peak = m_sampler.peakPressure();
        if (peak >= 0.0 && peak < kGrazePressure && m_sampler.contactDuration() < kGrazeSeconds) {
            m_strip.clear();
            m_settledVertices = 0;
            return {};
        }

        Stroke stroke = m_prototype;
        stroke.points = withDotTwin(m_sampler.points());
        return stroke;
    }

    QList<InkPoint> WetStroke::withDotTwin(QList<InkPoint> points) const
    {
        // Касание начинается и кончается почти без нажима, а точки касания со
        // сдвигом - это его края: без этой точки капсула вышла бы тоньше нажима.
        // Касание из одной точки - частный случай: Stroke::isValid требует двух.
        if (points.isEmpty() || !m_tessellator.isDot(points))
            return points;
        InkPoint twin = points.constLast();
        if (twin.hasPressure())
            twin.pressure = m_sampler.peakPressure();
        points.append(twin);
        return points;
    }
    ```

  Почему мокрое совпадает с сухим: на отрыве `rebuildTail()` (если начальный торец
  не лёг) строит `tessellate(withDotTwin(points))` - те же точки, что уходят в
  штрих; пиковая точка добавляет отрезок нулевой длины, решение `isDot` от неё не
  меняется. Если торец лёг, осевая уже не короче толщины, и `withDotTwin` ничего не
  дописывает.

  `DocumentCanvas` не меняется: `endInk()` уже кладёт в документ только
  `stroke.isValid()`, а мокрый узел появляется, как только полоса не пуста.

- [x] 7.11 **Прогон (ожидаемый проход).**

  ```sh
  cmake --build build -j$(nproc)
  ctest --test-dir build -R 'tst_stroketessellator|tst_wetstroke|tst_strokesampler|tst_canvasink|tst_penonlymode|tst_insertoverlay|tst_export|tst_pagescenegraph' --output-on-failure
  ```

  Ожидается: все зелёные; `shiftedTapHasPeakWidth` во всех трёх строках (сдвиги
  0.3, 1.5, 3.0 при толщине 4): полуширина капсулы по пику нажима в пределах 5% и
  во время касания, и после отрыва. `perPointWorkDoesNotGrowWithStrokeLength` - то
  же отношение ≈ 0.39. Если `tst_insertoverlay` падает на `QCOMPARE(canvas->pageAtView(free), 0)`
  или `insertAtView(free)`, точка `free` попала за страницу или на вкладыш при другом
  масштабе окна - сдвинуть смещение `QPointF(100, 450)` внутрь страницы, ниже
  нижнего края вкладыша, и не трогать остальное.

- [x] 7.12 **Полный прогон и коммит.**

  ```sh
  ctest --test-dir build --output-on-failure
  git add src/ink/stroketessellator.h src/ink/stroketessellator.cpp \
          src/ink/strokesampler.h src/ink/strokesampler.cpp \
          src/ink/wetstroke.h src/ink/wetstroke.cpp \
          tests/tst_stroketessellator.cpp tests/tst_strokesampler.cpp tests/tst_wetstroke.cpp \
          tests/tst_canvasink.cpp tests/tst_penonlymode.cpp tests/tst_insertoverlay.cpp
  git commit -F - <<'EOF'
  Касание пером оставляет точку

  Осевая короче толщины пера рисуется капсулой с наибольшей полушириной,
  касание без сдвига - диском; к такому штриху на отрыве дописывается точка с
  наибольшим нажимом касания (design.md, D5, как в прототипе outline-caps).
  Точка видна уже во время касания и той же толщины. Касание короче 30 мс
  легче 0.05 отбрасывается как задев.

  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  EOF
  ```

---

## 8. Гейт D6 на стенде: повторный прогон (D6 отложен)

Две правки D6 идут только вместе и только после гейта на стенде (design.md, D6):

- **Узел острия** (`exp/dry-refine`, `dr-resample-tol`): точка плотного сплайна,
  пропущенная шагом нарезки 1.2 px, становится точкой нарезки, если отходит от
  следующей хорды больше чем на 0.1 экранного px и поворот в ней круче 45°.
- **Окно [1, 2, 1]/4** (`exp/rnote`, `e3-raw121`): следование тянет узел модели не
  к сырому отсчёту, а к отсчёту, сглаженному центрированным окном; узлы отсчёта n
  окончательны с приходом отсчёта n + 1.

Гейт строже первоначального (по наблюдениям
предварительного прогона). Вариант `d6-raw121-cusp` - копия `StrokeSampler` после
шагов 1-2 с обеими правками; `d6-copy` - та же копия без правок, обязана совпасть с
`baseline` по осевой, иначе сравнение ничего не значит.

| Критерий | Метрика стенда | Условие (масштабы 1 и 2.5) |
|---|---|---|
| вершина острия в максимуме ≤ 0.6 px | `corner_dist_max_px`, максимум по корпусу без `corner_N_nostop_*` | ≤ 0.6 |
| граней не хуже baseline | `facet_count_2deg@sum` | ≤ baseline |
| избыток кривизны не хуже baseline | `curvature_tv_excess_per100px@mean` | ≤ baseline |
| окончательные точки не меняются | `settled_mutations@sum` | ≤ baseline |
| ложных острий не больше | `false_cusp_count` (8.1): поворот осевой круче 45° дальше 2 px от любой эталонной вершины с поворотом круче 45°, концы штриха по 2 px не считаются | сумма по всему корпусу ≤ baseline |
| ложных острий на рядах «о» и петлях нет сверх baseline | `false_cusp_count` на `small_o_8`, `small_o_12`, `small_o_20`, `fast_eeee` | ≤ baseline на каждом |
| острие без стоянки пера не хуже | `corner_dist_max_px` на `corner_N_nostop_240`, `corner_N_nostop_481` (8.2) | ≤ baseline |
| зубец короче кадра с запасом | `tooth_peak_ratio` на `tooth_240` (8.2) - то, что проверяет `tst_strokesampler::toothShorterThanFrameSurvives` (порог 0.5) | ≥ 0.525 |

**Результат прогона (стенд `exp/stroke-lab` с правками 8.1-8.4 поверх кода групп
6-7 в scratchpad; давление ещё до шага 1, на осевую оно не влияет - `d6-copy`
совпала с `baseline` без единого расхождения): гейт не пройден.** Одинаково на
масштабах 1 и 2.5:

| Критерий | baseline | d6-raw121-cusp | |
|---|---|---|---|
| вершина острия в максимуме, px | 1.413 | 0.485 | ok |
| граней > 2° | 2894 | 1455 | ok |
| избыток неровности кривизны | 3.00 | 1.52 | ok |
| settled_mutations | 1 | 1 | ok |
| ложных острий по корпусу | 82 | 94 | **FAIL** |
| `small_o_8/12/20` | 0/1/0 | 0/1/0 | ok |
| `fast_eeee` | 3 | 4 | **FAIL** |
| `corner_N_nostop_240`, px | 1.055 | 0.938 | ok |
| `corner_N_nostop_481`, px | 1.222 | 0.293 | ok |
| `tooth_240` | 0.9875 | 0.500 | **FAIL** |

Ложных острий прибавилось ещё на `fast_scribble` 1 → 3, `slow_arc_tremor` 1 → 3,
`small_e_10` 2 → 3, `word_minimum` 18 → 22, `word_minimum_small` 20 → 24,
`word_privet` 12 → 14.

Причины разделяются (прогон каждой правки по отдельности тем же вариантом,
масштаб 1): ложные острия даёт узел острия (он один: 82 → 105, `fast_eeee` 3 → 4;
окно одно: 82 → 80, но `fast_eeee` 3 → 6), а зубец срезает окно (оно одно: 0.443,
узел один: 1.000). Одной настройкой обе причины не снимаются. Попробована одна -
порог поворота узла острия 60° вместо 45° при полном окне (`d6-raw121-cusp60`):
вершина острия 0.484, граней 1460, избыток кривизны 1.40, ложных острий по корпусу
82 → 78 (ok), но `fast_eeee` 3 → 4 и зубец 0.443 - **гейт не пройден**. Зубец
структурно требует переделки окна (например, сглаживать только соседей не дальше
3 мс - на 240 Гц окно выключается), это вторая настройка и уже работа отложенного
изменения про траекторию.

Поэтому D6 вынесен из изменения (design.md, D6 - там уже записан этот
результат). Группа 8 только даёт стенду метрики, сэмплы и вариант гейта
(8.1-8.4) и повторяет прогон на настоящем коде после шага 1 (8.5);
`StrokeSampler` в шаге 2 не меняется. Вариант `d6-raw121-cusp` на стенде -
основа отложенного изменения про траекторию.

**Files:**
- Modify: `tools/stroke-lab/metrics.cpp`, `tools/stroke-lab/corpus.h`,
  `tools/stroke-lab/corpus.cpp`, `tools/stroke-lab/README.md`
- Create: `tools/stroke-lab/variants/d6_raw121_cusp.cpp`, `tools/stroke-lab/gate_d6.py`
- Modify: `openspec/changes/ink-beauty-pass/design.md` (D6 - только если числа повторного прогона отличаются)

- [x] 8.1 **Метрики ложных острий и зубца на стенде.** `tools/stroke-lab/metrics.cpp`,
  функция `computeMetrics`:
  - после объявления `int facetCount = 0;` добавить

    ```cpp
        int falseCuspCount = 0;
        double toothPeak = 0.0;
    ```

  - в блоке «// 2. Грани: скачок угла поворота относительно соседей.» заменить
    конец цикла граней и закрывающую скобку блока

    ```cpp
                    if (f > 2.0)
                        ++facetCount;
                }
            }
    ```

    на

    ```cpp
                    if (f > 2.0)
                        ++facetCount;
                }
                // Ложные острия (design.md, D6): поворот осевой круче 45° дальше 2 px
                // от любой эталонной вершины с поворотом круче 45°. Концы штриха
                // (2 px) не считаются: там дотяжка до точки отрыва даёт крючок.
                for (size_t i = 1; i + 1 < c.size(); ++i) {
                    if (arc[i] < 2.0 || total - arc[i] < 2.0 || nearAny(c[i], corners, 2.0))
                        continue;
                    if (std::abs(th[i]) > M_PI / 4.0)
                        ++falseCuspCount;
                }
            }

            // Зубец короче кадра: насколько осевая поднялась над прямой у вершины.
            if (sample.toothAmplitude > 0.0) {
                const double baselineY = sample.toothApex.y() + sample.toothAmplitude;
                for (const QPointF &p : center) {
                    if (std::abs(p.x() - sample.toothApex.x()) < 6.0)
                        toothPeak = std::max(toothPeak, baselineY - p.y());
                }
            }
    ```

  - после строки `m["turn_max_deg"] = r(turn.max());` добавить

    ```cpp
        m["false_cusp_count"] = falseCuspCount;
        if (sample.toothAmplitude > 0.0)
            m["tooth_peak_ratio"] = r(toothPeak / sample.toothAmplitude, 4);
    ```

  Сборка - в 8.2 вместе с полями `Sample`, которые здесь используются.

- [x] 8.2 **Сэмплы: острие без стоянки пера и зубец короче кадра.**
  `tools/stroke-lab/corpus.h`, структура `Sample`: после поля
  `QRectF bounds;                 // охват эталона, px` добавить

  ```cpp
      // Зубец короче кадра (сэмпл tooth_240): вершина зубца и его высота над
      // прямой, px. Высота 0 - зубца в сэмпле нет.
      QPointF toothApex;
      double toothAmplitude = 0.0;
  ```

  `tools/stroke-lab/corpus.cpp`:
  - заменить структуру `Spec` целиком

    ```cpp
    struct Spec
    {
        std::string id;
        std::string title;
        std::vector<std::string> tags;
        double size;
        std::vector<Shape> strokes;
        Motion motion;
    };
    ```

    на

    ```cpp
    struct Spec
    {
        std::string id;
        std::string title;
        std::vector<std::string> tags;
        double size;
        std::vector<Shape> strokes;
        Motion motion;
        // Частота пера, если не та, что у корпуса; 0 - CorpusOptions::rateHz.
        double rateHz = 0.0;
        // Эталонные вершины острий, которых нет между частями (острие внутри
        // части - разворот без остановки), в координатах фигуры.
        std::vector<QPointF> extraCorners;
    };
    ```

  - в `realize()` заменить

    ```cpp
        double t = 0.0;
        for (const Shape &raw : spec.strokes) {
    ```

    на

    ```cpp
        CorpusOptions sampling = options;
        if (spec.rateHz > 0.0)
            sampling.rateHz = spec.rateHz;
        for (const QPointF &corner : spec.extraCorners)
            sample.corners.push_back(corner + QPointF(16.0 - minX, 16.0 - minY));

        double t = 0.0;
        for (const Shape &raw : spec.strokes) {
    ```

    и там же строку
    `        sampleEvents(stroke, m, options, 7300.0 + seed * 10.0, rng);` на
    `        sampleEvents(stroke, m, sampling, 7300.0 + seed * 10.0, rng);`;
  - перед функцией `QList<QPointF> toQList(const reference::Cardiogram &c)`
    вставить

    ```cpp
    // Ломаная одной частью, плотно (шаг 0.05 px): её вершины - развороты без
    // остановки пера. Скорость в них падает только по закону 2/3 (до 0.35 от
    // крейсерской), а не до нуля, как в остриях между частями.
    Shape densePolyline(const QList<QPointF> &points)
    {
        std::vector<QPointF> dense;
        for (int i = 0; i + 1 < points.size(); ++i) {
            const QPointF a = points[i];
            const QPointF b = points[i + 1];
            const int steps = std::max(1, int(std::ceil(len(b - a) / 0.05)));
            for (int k = 0; k < steps; ++k)
                dense.push_back(a + (b - a) * (double(k) / steps));
        }
        dense.push_back(points.constLast());
        Shape shape;
        shape.parts.push_back(dense);
        return shape;
    }

    // Зубец короче кадра - ровно эталон tst_strokesampler::toothShorterThanFrameSurvives:
    // один отсчёт на 240 Гц уходит вверх на 3 px. Без тремора и шума датчика,
    // метки времени - те же, что в тесте.
    Sample toothSample()
    {
        const reference::ToothStroke tooth = reference::toothShorterThanFrame();
        Sample sample;
        sample.id = "tooth_240";
        sample.title = "Зубец 3 px в один отсчёт, 300 px/s, 240 Гц";
        sample.tags = {"tooth"};
        sample.size = tooth.amplitude;
        SampleStroke stroke;
        double maxX = 0.0, maxY = 0.0;
        for (const InkPoint &p : tooth.points) {
            stroke.events.push_back(p);
            stroke.eventTrueTime.push_back(p.timeSeconds);
            stroke.truth.t.push_back(p.timeSeconds);
            stroke.truth.p.push_back(p.position);
            stroke.truth.pressure.push_back(p.pressure);
            stroke.truth.speed.push_back(300.0);
            maxX = std::max(maxX, p.position.x());
            maxY = std::max(maxY, p.position.y());
        }
        sample.strokes.push_back(std::move(stroke));
        sample.toothApex = tooth.apex;
        sample.toothAmplitude = tooth.amplitude;
        sample.bounds = QRectF(0, 0, std::ceil(maxX + 16.0), std::ceil(maxY + 16.0));
        return sample;
    }

    ```

  - в конце `buildCorpus()` заменить

    ```cpp
        std::vector<Sample> samples;
        for (const Spec &spec : specs)
            samples.push_back(realize(spec, spec.motion.seed, options));
        return samples;
    ```

    на

    ```cpp
        // Острия без стоянки пера (гейт D6): «N» эталона, перо разворачивается на
        // ходу, на частотах 240 и 481 Гц.
        for (const double rate : {240.0, 481.0}) {
            const QList<QPointF> n = reference::cornerN();
            Spec spec{rate < 300.0 ? "corner_N_nostop_240" : "corner_N_nostop_481",
                      rate < 300.0 ? "«N» без стоянки в остриях, 600 px/s, 240 Гц"
                                   : "«N» без стоянки в остриях, 600 px/s, 481 Гц",
                      {"corner", "nostop"}, 80, {densePolyline(n)},
                      motion(600, 40, 0.12, rate < 300.0 ? 34 : 35)};
            spec.rateHz = rate;
            spec.extraCorners = {n[1], n[2]};
            specs.push_back(spec);
        }

        std::vector<Sample> samples;
        for (const Spec &spec : specs)
            samples.push_back(realize(spec, spec.motion.seed, options));
        samples.push_back(toothSample());
        return samples;
    ```

  Прогон:

  ```sh
  cmake --build build -j$(nproc) --target stroke-lab
  ./build/tools/stroke-lab/stroke-lab --list | grep -E "nostop|tooth"
  ./build/tools/stroke-lab/stroke-lab --variants baseline --samples 'corner_N_nostop*,tooth_240,small_o_12' --scales 1 --no-png --out /tmp/uq-lab-check
  python3 -c "import json; [print(s['sample'], s.get('corner_dist_max_px'), s['false_cusp_count'], s.get('tooth_peak_ratio')) for s in json.load(open('/tmp/uq-lab-check/baseline/metrics.json'))['samples']]"
  ```

  Ожидается: в списке `corner_N_nostop_240` (75 отсчётов), `corner_N_nostop_481`
  (151), `tooth_240` (120); метрики baseline - `corner_N_nostop_240 1.055 …`,
  `corner_N_nostop_481 1.222 …`, `small_o_12 None 1 None`, `tooth_240 None 0
  0.9875` (зубец 2.96 из 3, как в `toothShorterThanFrameSurvives`).

- [x] 8.3 **Вариант стенда `d6-raw121-cusp`.** Создать
  `tools/stroke-lab/variants/d6_raw121_cusp.cpp` (CMake стенда подхватывает
  `variants/*.cpp` сам):

  ```cpp
  // Гейт D6 (openspec/changes/ink-beauty-pass/design.md, D6): узел острия в
  // нарезке и окно [1, 2, 1]/4 на входе следования - вместе, поверх траектории
  // приложения после шагов 1 и 2.
  //
  // GateSampler - копия src/ink/strokesampler.cpp (давление по шагу 1: One Euro
  // 12 Гц / 0.5, давление узла по его времени, отфильтрованное давление на
  // отрыве; точка-касание по шагу 2) с двумя переключателями:
  //  - raw121: следование тянет узел модели не к сырому отсчёту, а к отсчёту,
  //    сглаженному центрированным окном [1, 2, 1]/4; узлы отсчёта n
  //    окончательны, когда пришёл отсчёт n + 1 (exp/rnote, e3-raw121,
  //    rnote_kit.h: smoothAt / finalizeSmooth / followTarget);
  //  - cusp: точка плотного сплайна, пропущенная шагом нарезки, становится
  //    точкой нарезки, если отходит от следующей хорды больше чем на 0.1
  //    экранного px при повороте круче 45° (exp/dry-refine, dr-resample-tol:
  //    Walker / farSkipped / walk / flushTowards).
  //
  // Варианты:
  //  d6-copy        - оба переключателя выключены. Обязан совпасть с baseline по
  //                   осевой (dev, facets, corner, curvTV): так проверяется, что
  //                   копия не разошлась с src/ink/strokesampler.cpp.
  //  d6-raw121-cusp - оба включены: то, что пойдёт в приложение, если гейт пройден.
  //
  // Геометрия - StrokeTessellator приложения на окончательных точках, как у
  // baseline. Мокрая геометрия не строится: гейт смотрит на осевую, а время на
  // отсчёт здесь - только сэмплер, без тесселяции мокрой полосы.

  #include "lab.h"

  #include <QtGlobal>

  #include <cmath>
  #include <memory>
  #include <utility>
  #include <vector>

  #include "ink/inktypes.h"
  #include "ink/oneeurofilter.h"
  #include "ink/stroketessellator.h"

  #include "ink_stroke_modeler/params.h"
  #include "ink_stroke_modeler/stroke_modeler.h"
  #include "ink_stroke_modeler/types.h"

  namespace {

  namespace ism = ink::stroke_model;

  // --- Параметры: ровно как в src/ink/strokesampler.cpp -------------------------

  constexpr float kSpringMassConstant = 2.2e-4f;
  constexpr float kDragConstant = 94.0f;
  constexpr qreal kFollowSpeedLower = 20.0;
  constexpr qreal kFollowSpeedUpper = 200.0;
  constexpr qreal kFollowSpeedTimeConstant = 0.01;
  constexpr int kRawHistory = 64;
  constexpr double kWobbleTimeout = 0.04;
  constexpr float kWobbleSpeedFloor = 1.31f;
  constexpr float kWobbleSpeedCeiling = 1.44f;
  constexpr double kMinOutputRate = 100.0;
  constexpr float kEndOfStrokeStoppingDistance = 0.01f;
  constexpr int kEndOfStrokeMaxIterations = 20;
  constexpr int kMaxOutputsPerCall = 100000;
  // Шаг 1 (D2): One Euro давления 12 Гц, beta 0.5. Срез производной - прежний.
  constexpr qreal kPressureMinCutoff = 12.0;
  constexpr qreal kPressureBeta = 0.5;
  constexpr qreal kPressureDerivativeCutoff = 10.0;
  constexpr qreal kFlattenFraction = 0.5;
  constexpr qreal kEndSnapDistance = 1e-4;

  // D6.
  constexpr qreal kCuspTolerance = 0.1;   // экранные px
  constexpr qreal kCuspTurn = M_PI / 4.0; // 45°
  constexpr int kMaxSkipped = 256;

  // Шаг 2 (D5): порог случайного задева, как в src/ink/wetstroke.cpp.
  constexpr qreal kGrazeSeconds = 0.030;
  constexpr qreal kGrazePressure = 0.05;

  struct Options
  {
      bool raw121 = false;
      bool cusp = false;
  };

  ism::StrokeModelParams modelParams()
  {
      ism::StrokeModelParams params;
      params.wobble_smoother_params.is_enabled = true;
      params.wobble_smoother_params.timeout = ism::Duration(kWobbleTimeout);
      params.wobble_smoother_params.speed_floor = kWobbleSpeedFloor;
      params.wobble_smoother_params.speed_ceiling = kWobbleSpeedCeiling;
      params.position_modeler_params.spring_mass_constant = kSpringMassConstant;
      params.position_modeler_params.drag_constant = kDragConstant;
      params.sampling_params.min_output_rate = kMinOutputRate;
      params.sampling_params.end_of_stroke_stopping_distance = kEndOfStrokeStoppingDistance;
      params.sampling_params.end_of_stroke_max_iterations = kEndOfStrokeMaxIterations;
      params.sampling_params.max_outputs_per_call = kMaxOutputsPerCall;
      params.prediction_params = ism::StrokeEndPredictorParams{};
      return params;
  }

  qreal distance(const QPointF &a, const QPointF &b)
  {
      return std::hypot(b.x() - a.x(), b.y() - a.y());
  }

  qreal distanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
  {
      const QPointF ab = b - a;
      const qreal lengthSquared = QPointF::dotProduct(ab, ab);
      const qreal u = lengthSquared < 1e-18
          ? 0.0
          : qBound(0.0, QPointF::dotProduct(p - a, ab) / lengthSquared, 1.0);
      return distance(p, a + ab * u);
  }

  qreal turnAngle(const QPointF &in, const QPointF &out)
  {
      return std::abs(std::atan2(in.x() * out.y() - in.y() * out.x(), QPointF::dotProduct(in, out)));
  }

  QPointF catmullRom(const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3,
                     qreal u)
  {
      const auto knot = [](const QPointF &a, const QPointF &b) {
          return std::sqrt(qMax(distance(a, b), 1e-6));
      };
      const qreal t0 = 0.0;
      const qreal t1 = t0 + knot(p0, p1);
      const qreal t2 = t1 + knot(p1, p2);
      const qreal t3 = t2 + knot(p2, p3);
      const qreal t = t1 + (t2 - t1) * u;
      const QPointF a1 = p0 * ((t1 - t) / (t1 - t0)) + p1 * ((t - t0) / (t1 - t0));
      const QPointF a2 = p1 * ((t2 - t) / (t2 - t1)) + p2 * ((t - t1) / (t2 - t1));
      const QPointF a3 = p2 * ((t3 - t) / (t3 - t2)) + p3 * ((t - t2) / (t3 - t2));
      const QPointF b1 = a1 * ((t2 - t) / (t2 - t0)) + a2 * ((t - t0) / (t2 - t0));
      const QPointF b2 = a2 * ((t3 - t) / (t3 - t1)) + a3 * ((t - t1) / (t3 - t1));
      return b1 * ((t2 - t) / (t2 - t1)) + b2 * ((t - t1) / (t2 - t1));
  }

  qreal lerpPressure(const InkPoint &from, const InkPoint &to, qreal u)
  {
      if (from.hasPressure() && to.hasPressure())
          return from.pressure + (to.pressure - from.pressure) * u;
      return to.pressure;
  }

  // Положение пера в момент time по треку: между соседними отсчётами - по
  // сплайну, у последнего интервала сосед отражается.
  QPointF penAt(const QList<InkPoint> &track, qreal time)
  {
      if (track.isEmpty())
          return {};
      if (time >= track.constLast().timeSeconds)
          return track.constLast().position;
      for (qsizetype i = track.size() - 1; i > 0; --i) {
          const InkPoint &a = track[i - 1];
          const InkPoint &b = track[i];
          if (time >= a.timeSeconds) {
              const qreal span = b.timeSeconds - a.timeSeconds;
              const qreal u = span > 0.0 ? (time - a.timeSeconds) / span : 1.0;
              const QPointF p0 = i >= 2 ? track[i - 2].position : a.position * 2.0 - b.position;
              const QPointF p3 =
                  i + 1 < track.size() ? track[i + 1].position : b.position * 2.0 - a.position;
              return catmullRom(p0, a.position, b.position, p3, u);
          }
      }
      return track.constFirst().position;
  }

  class GateSampler
  {
  public:
      explicit GateSampler(Options options)
          : m_options(options)
          , m_modeler(std::make_unique<ism::StrokeModeler>())
          , m_pressureFilter(kPressureMinCutoff, kPressureBeta, kPressureDerivativeCutoff)
      {
          [[maybe_unused]] const absl::Status status = m_modeler->Reset(modelParams());
      }

      void setScale(qreal scale) { m_scale = qBound(0.01, scale, 100.0); }
      qreal spacing() const { return m_screenSpacing / m_scale; }

      void reset()
      {
          [[maybe_unused]] const absl::Status status = m_modeler->Reset();
          m_pressureFilter.reset();
          m_started = false;
          m_finished = false;
          m_timeOrigin = 0.0;
          m_lastInputTime = 0.0;
          m_lastInputScreen = QPointF();
          m_lastInputPressure = -1.0;
          m_peakPressure = -1.0;
          m_lastRaw = InkPoint();
          m_follow = FollowState();
          m_rawHistory.clear();
          m_rawCount = 0;
          m_smoothedCount = 0;
          m_smoothTrack.clear();
          m_pendingResults.clear();
          m_skipped.clear();
          m_nodes.clear();
          m_nextSegment = 0;
          m_points.clear();
          m_provisional.clear();
      }

      const QList<InkPoint> &points() const { return m_points; }
      const QList<InkPoint> &provisionalPoints() const { return m_provisional; }

      // Шаг 2 (D5), WetStroke::finish: случайный задев штриха не оставляет, а к
      // точке (осевая короче толщины пера) дописывается точка с пиком нажима.
      bool isGraze() const
      {
          return m_peakPressure >= 0.0 && m_peakPressure < kGrazePressure
              && m_lastInputTime < kGrazeSeconds;
      }
      void discard() { m_points.clear(); }
      void appendDotTwin(const StrokeTessellator &tessellator)
      {
          if (m_points.isEmpty() || !tessellator.isDot(m_points))
              return;
          InkPoint twin = m_points.constLast();
          if (twin.hasPressure())
              twin.pressure = m_peakPressure;
          m_points.append(twin);
      }

      void append(const InkPoint &raw)
      {
          if (m_finished)
              return;
          const QPointF screen = raw.position * m_scale;
          qreal time = raw.timeSeconds;
          if (m_started) {
              time = qMax(time - m_timeOrigin, m_lastInputTime);
              if (time == m_lastInputTime && screen == m_lastInputScreen)
                  return;
          } else {
              m_timeOrigin = time;
              time = 0.0;
          }
          const qreal pressure = raw.hasPressure()
              ? qBound(0.0, m_pressureFilter.filter(raw.pressure, time), 1.0)
              : -1.0;

          ism::Input input;
          input.event_type = m_started ? ism::Input::EventType::kMove : ism::Input::EventType::kDown;
          input.position = {float(screen.x()), float(screen.y())};
          input.time = ism::Time(time);
          input.pressure = float(pressure);
          m_results.clear();
          if (!m_modeler->Update(input, m_results).ok())
              return;

          m_started = true;
          m_lastInputTime = time;
          m_lastInputScreen = screen;
          m_lastInputPressure = pressure;
          m_peakPressure = qMax(m_peakPressure, pressure);
          m_lastRaw = raw;
          m_rawHistory.append({screen, pressure, time});
          while (m_rawHistory.size() > kRawHistory)
              m_rawHistory.removeFirst();
          ++m_rawCount;

          if (m_options.raw121) {
              m_pendingResults.emplace_back(m_rawCount - 1, m_results);
              settleSmoothedSamples(false);
          } else {
              for (const ism::Result &result : m_results)
                  appendNode(result);
          }
          emitSettled(false);
          rebuildProvisional();
      }

      void finish()
      {
          if (m_finished || !m_started)
              return;
          m_finished = true;
          m_provisional.clear();

          ism::Input up;
          up.event_type = ism::Input::EventType::kUp;
          up.position = {float(m_lastInputScreen.x()), float(m_lastInputScreen.y())};
          up.time = ism::Time(m_lastInputTime);
          up.pressure = float(m_lastInputPressure);
          m_results.clear();
          const bool caughtUp = m_modeler->Update(up, m_results).ok();
          if (m_options.raw121)
              settleSmoothedSamples(true);
          if (caughtUp) {
              for (const ism::Result &result : m_results)
                  appendNode(result);
          }
          emitSettled(true);

          // Шаг 1: последняя точка - с отфильтрованным давлением.
          InkPoint end = m_lastRaw;
          if (end.hasPressure())
              end.pressure = m_lastInputPressure;
          InkPoint last = m_points.constLast();
          QList<InkPoint> tail;
          walkTowards(last, m_skipped, end, tail);
          flushCusps(last, m_skipped, end, tail);
          if (distance(last.position, end.position) > kEndSnapDistance)
              tail.append(end);
          m_points.append(tail);
      }

  private:
      struct FollowState
      {
          qreal speed = 0.0;
          qreal time = 0.0;
          bool primed = false;
      };

      // Шаг 1 (D2): отфильтрованное давление пера в момент time.
      qreal pressureAt(qreal time) const
      {
          const QList<InkPoint> &h = m_rawHistory;
          if (h.isEmpty())
              return -1.0;
          if (time >= h.constLast().timeSeconds)
              return h.constLast().pressure;
          for (qsizetype i = h.size() - 1; i > 0; --i) {
              if (time >= h[i - 1].timeSeconds) {
                  const qreal span = h[i].timeSeconds - h[i - 1].timeSeconds;
                  const qreal u = span > 0.0 ? (time - h[i - 1].timeSeconds) / span : 1.0;
                  return h[i - 1].pressure + (h[i].pressure - h[i - 1].pressure) * u;
              }
          }
          return h.constFirst().pressure;
      }

      InkPoint smoothedSample(int i, int last) const
      {
          const int first = m_rawCount - int(m_rawHistory.size());
          const InkPoint &sample = m_rawHistory[i - first];
          if (i <= 0 || i >= last)
              return sample;
          InkPoint smoothed = sample;
          smoothed.position = (m_rawHistory[i - 1 - first].position + sample.position * 2.0
                               + m_rawHistory[i + 1 - first].position)
              / 4.0;
          return smoothed;
      }

      void settleSmoothedSamples(bool finishing)
      {
          const int last = m_rawCount - 1;
          while (m_smoothedCount <= last
                 && (finishing || m_smoothedCount == 0 || m_smoothedCount < last)) {
              m_smoothTrack.append(smoothedSample(m_smoothedCount, last));
              ++m_smoothedCount;
          }
          while (m_smoothTrack.size() > kRawHistory)
              m_smoothTrack.removeFirst();
          while (!m_pendingResults.empty() && m_pendingResults.front().first < m_smoothedCount) {
              for (const ism::Result &result : m_pendingResults.front().second)
                  appendNode(result);
              m_pendingResults.erase(m_pendingResults.begin());
          }
      }

      QList<InkPoint> provisionalPenTrack() const
      {
          if (!m_options.raw121)
              return m_rawHistory;
          QList<InkPoint> track = m_smoothTrack;
          for (int i = m_smoothedCount; i < m_rawCount; ++i)
              track.append(smoothedSample(i, m_rawCount - 1));
          return track;
      }

      InkPoint followedNode(const ism::Result &result, FollowState &state,
                            const QList<InkPoint> &penTrack) const
      {
          const qreal time = result.time.Value();
          const qreal speed = std::hypot(result.velocity.x, result.velocity.y);
          if (state.primed) {
              const qreal dt = qMax(0.0, time - state.time);
              state.speed += (1.0 - std::exp(-dt / kFollowSpeedTimeConstant)) * (speed - state.speed);
          } else {
              state.speed = speed;
              state.primed = true;
          }
          state.time = time;
          const qreal x = qBound(
              0.0, (state.speed - kFollowSpeedLower) / (kFollowSpeedUpper - kFollowSpeedLower), 1.0);
          const qreal share = x * x * (3.0 - 2.0 * x);
          const QPointF modeled(result.position.x, result.position.y);
          InkPoint node;
          node.position = modeled + (penAt(penTrack, time) - modeled) * share;
          node.pressure = m_lastInputPressure < 0.0 ? -1.0 : pressureAt(time);
          node.timeSeconds = qMin(time, m_lastInputTime);
          return node;
      }

      void appendNode(const ism::Result &result)
      {
          const InkPoint node =
              followedNode(result, m_follow, m_options.raw121 ? m_smoothTrack : m_rawHistory);
          if (!m_nodes.isEmpty() && distance(m_nodes.constLast().position, node.position) < 1e-3
              && m_nextSegment + 1 < m_nodes.size()) {
              m_nodes.last().pressure = node.pressure;
              m_nodes.last().timeSeconds = node.timeSeconds;
              return;
          }
          m_nodes.append(node);
      }

      InkPoint toPage(const InkPoint &node) const
      {
          return {node.position / m_scale, node.pressure, node.timeSeconds + m_timeOrigin};
      }

      template <typename Sink>
      void flattenSegment(const QList<InkPoint> &nodes, int k, Sink sink) const
      {
          const InkPoint &n1 = nodes[k];
          const InkPoint &n2 = nodes[k + 1];
          const QPointF p1 = n1.position;
          const QPointF p2 = n2.position;
          const QPointF p0 = k > 0 ? nodes[k - 1].position : p1 * 2.0 - p2;
          const QPointF p3 = k + 2 < nodes.size() ? nodes[k + 2].position : p2 * 2.0 - p1;
          const int steps = qBound(
              1, int(std::ceil(distance(p1, p2) / (m_screenSpacing * kFlattenFraction))), 4096);
          for (int i = 1; i <= steps; ++i) {
              const qreal u = qreal(i) / steps;
              InkPoint dense;
              dense.position = i == steps ? p2 : catmullRom(p0, p1, p2, p3, u);
              dense.pressure = lerpPressure(n1, n2, u);
              dense.timeSeconds = n1.timeSeconds + (n2.timeSeconds - n1.timeSeconds) * u;
              sink(toPage(dense));
          }
      }

      int cuspAmong(const InkPoint &last, const QList<InkPoint> &skipped, const QPointF &to) const
      {
          if (!m_options.cusp)
              return -1;
          qreal farthest = kCuspTolerance / m_scale;
          int cusp = -1;
          for (int i = 0; i < skipped.size(); ++i) {
              const QPointF p = skipped[i].position;
              const qreal offset = distanceToSegment(p, last.position, to);
              if (offset <= farthest || turnAngle(p - last.position, to - p) < kCuspTurn)
                  continue;
              farthest = offset;
              cusp = i;
          }
          return cusp;
      }

      void walkTowards(InkPoint &last, QList<InkPoint> &skipped, const InkPoint &target,
                       QList<InkPoint> &out) const
      {
          const qreal step = spacing();
          while (true) {
              const QPointF delta = target.position - last.position;
              const qreal length = std::hypot(delta.x(), delta.y());
              if (length < step) {
                  if (m_options.cusp) {
                      skipped.append(target);
                      if (skipped.size() > kMaxSkipped)
                          skipped.remove(0, kMaxSkipped / 2);
                  }
                  break;
              }
              const int cusp = cuspAmong(last, skipped, target.position);
              if (cusp >= 0) {
                  out.append(skipped[cusp]);
                  last = skipped[cusp];
                  skipped.remove(0, cusp + 1);
                  continue;
              }
              const qreal t = step / length;
              InkPoint next;
              next.position = last.position + delta * t;
              next.timeSeconds = last.timeSeconds + (target.timeSeconds - last.timeSeconds) * t;
              next.pressure = lerpPressure(last, target, t);
              out.append(next);
              last = next;
              skipped.clear();
          }
      }

      void flushCusps(InkPoint &last, QList<InkPoint> &skipped, const InkPoint &end,
                      QList<InkPoint> &out) const
      {
          while (!skipped.isEmpty()) {
              const int cusp = cuspAmong(last, skipped, end.position);
              if (cusp < 0)
                  break;
              out.append(skipped[cusp]);
              last = skipped[cusp];
              skipped.remove(0, cusp + 1);
          }
      }

      void emitSettled(bool toEnd)
      {
          if (m_nodes.isEmpty())
              return;
          QList<InkPoint> added;
          if (m_points.isEmpty())
              m_points.append(toPage(m_nodes.constFirst()));
          InkPoint last = m_points.constLast();
          while (m_nextSegment + 1 < m_nodes.size() && (toEnd || m_nextSegment + 2 < m_nodes.size())) {
              flattenSegment(m_nodes, m_nextSegment, [&](const InkPoint &dense) {
                  walkTowards(last, m_skipped, dense, added);
              });
              ++m_nextSegment;
          }
          m_points.append(added);
          const int drop = m_nextSegment - 1;
          if (drop > 0) {
              m_nodes.remove(0, drop);
              m_nextSegment -= drop;
          }
      }

      void rebuildProvisional()
      {
          m_provisional.clear();
          if (m_points.isEmpty())
              return;
          QList<InkPoint> tail = m_nodes.mid(qMax(0, m_nextSegment - 1));
          const int first = m_nextSegment > 0 ? 1 : 0;
          FollowState follow = m_follow;
          const QList<InkPoint> track = provisionalPenTrack();
          const auto push = [&](const ism::Result &result) {
              const InkPoint node = followedNode(result, follow, track);
              if (!tail.isEmpty() && distance(tail.constLast().position, node.position) < 1e-3)
                  return;
              tail.append(node);
          };
          for (const auto &pending : m_pendingResults) {
              for (const ism::Result &result : pending.second)
                  push(result);
          }
          m_prediction.clear();
          if (m_modeler->Predict(m_prediction).ok()) {
              for (const ism::Result &result : m_prediction)
                  push(result);
          }
          InkPoint last = m_points.constLast();
          QList<InkPoint> skipped = m_skipped;
          for (int k = first; k + 1 < tail.size(); ++k) {
              flattenSegment(tail, k, [&](const InkPoint &dense) {
                  walkTowards(last, skipped, dense, m_provisional);
              });
          }
          if (tail.size() > first) {
              const InkPoint end = toPage(tail.constLast());
              flushCusps(last, skipped, end, m_provisional);
              if (distance(last.position, end.position) > 1e-9)
                  m_provisional.append(end);
          }
      }

      Options m_options;
      qreal m_screenSpacing = 1.2;
      qreal m_scale = 1.0;
      std::unique_ptr<ism::StrokeModeler> m_modeler;
      std::vector<ism::Result> m_results;
      std::vector<ism::Result> m_prediction;
      OneEuroFilter m_pressureFilter;
      bool m_started = false;
      bool m_finished = false;
      qreal m_timeOrigin = 0.0;
      qreal m_lastInputTime = 0.0;
      QPointF m_lastInputScreen;
      qreal m_lastInputPressure = -1.0;
      qreal m_peakPressure = -1.0;
      InkPoint m_lastRaw;
      FollowState m_follow;
      QList<InkPoint> m_rawHistory;
      int m_rawCount = 0;
      int m_smoothedCount = 0;
      QList<InkPoint> m_smoothTrack;
      std::vector<std::pair<int, std::vector<ism::Result>>> m_pendingResults;
      QList<InkPoint> m_skipped;
      QList<InkPoint> m_nodes;
      int m_nextSegment = 0;
      QList<InkPoint> m_points;
      QList<InkPoint> m_provisional;
  };

  class GateVariant : public lab::Variant
  {
  public:
      explicit GateVariant(Options options) : m_sampler(options) {}

      void begin(const lab::StrokeSetup &setup) override
      {
          m_sampler.reset();
          m_sampler.setScale(setup.scale);
          m_tessellator.setWidth(setup.width);
          // Новые штрихи пера - заострённые (шаг 1, D3), как в DocumentCanvas::beginInkAt.
          m_tessellator.setStyle(StrokeStyle::Tapered);
      }

      void addEvent(const InkPoint &raw) override { m_sampler.append(raw); }

      void wet(int from, lab::WetState &out) const override
      {
          const QList<InkPoint> &points = m_sampler.points();
          out.settledCount = int(points.size());
          for (int i = qMax(0, from); i < points.size(); ++i)
              out.settledFrom.push_back(toCenter(points[i]));
          for (const InkPoint &p : m_sampler.provisionalPoints())
              out.provisional.push_back(toCenter(p));
      }

      void finish() override
      {
          m_sampler.finish();
          if (m_sampler.isGraze())
              m_sampler.discard();
          else
              m_sampler.appendDotTwin(m_tessellator);
      }

      std::vector<lab::CenterPoint> centerline() const override
      {
          std::vector<lab::CenterPoint> out;
          for (const InkPoint &p : m_sampler.points())
              out.push_back(toCenter(p));
          return out;
      }

      lab::Geometry geometry() const override
      {
          lab::Geometry g;
          const QVector<StrokeTessellator::Vertex> strip = m_tessellator.tessellate(m_sampler.points());
          if (StrokeTessellator::isDegenerate(strip))
              return g;
          g.kind = lab::Geometry::Kind::TriangleStrip;
          for (const auto &v : strip)
              g.vertices.emplace_back(v.x, v.y);
          return g;
      }

  private:
      lab::CenterPoint toCenter(const InkPoint &p) const
      {
          return {p.position, 2.0 * m_tessellator.halfWidthAt(p), p.pressure, p.timeSeconds};
      }

      GateSampler m_sampler;
      StrokeTessellator m_tessellator;
  };

  struct D6Copy : GateVariant
  {
      D6Copy() : GateVariant(Options{false, false}) {}
  };

  struct D6Raw121Cusp : GateVariant
  {
      D6Raw121Cusp() : GateVariant(Options{true, true}) {}
  };

  } // namespace

  STROKE_LAB_VARIANT(D6Copy, "d6-copy",
                     "Копия StrokeSampler без D6: сверка копии с baseline по осевой");
  STROKE_LAB_VARIANT(D6Raw121Cusp, "d6-raw121-cusp",
                     "D6: узел острия в нарезке (0.1 px, 45°) + окно [1,2,1]/4 на входе следования");
  ```

  Если шаг 1 записал в `src/ink/strokesampler.cpp` другие константы давления
  (`kPressureMinCutoff`, `kPressureBeta`, `kPressureDerivativeCutoff`) или иначе
  выбрал давление узла и точки отрыва - привести копию к коду приложения. Осевая от
  давления не зависит, но от пика давления зависит отбрасывание задева, а с ним
  `centerline_points`, который сверяет 8.5; толщина нужна картинкам стенда.

- [x] 8.4 **Проверка гейта скриптом.** Создать `tools/stroke-lab/gate_d6.py`:

  ```python
  # Гейт D6 (openspec/changes/ink-beauty-pass/design.md, D6) по результатам
  #   stroke-lab --variants baseline,d6-copy,<вариант> --no-png --out DIR
  # Запуск: python3 tools/stroke-lab/gate_d6.py DIR [вариант]. Код выхода 0 - гейт пройден.
  import json
  import sys

  out = sys.argv[1]
  candidate = sys.argv[2] if len(sys.argv) > 2 else 'd6-raw121-cusp'
  variants = ('baseline', 'd6-copy', candidate)
  summary = json.load(open(f'{out}/summary.json'))
  per = {}
  for v in variants:
      samples = json.load(open(f'{out}/{v}/metrics.json'))['samples']
      per[v] = {(s['sample'], float(s['scale'])): s for s in samples}
  base, cand = per['baseline'], per[candidate]
  scales = sorted({scale for _, scale in base})

  passed = True


  def check(name, condition, detail):
      global passed
      print(('ok    ' if condition else 'FAIL  ') + name + ': ' + detail)
      passed = passed and condition


  # Сверка копии сэмплера с приложением: без D6 осевая та же до бита.
  keys = ['centerline_dev_mean_px', 'centerline_dev_max_px', 'facet_count_2deg',
          'curvature_tv_excess_per100px', 'corner_dist_max_px', 'settled_mutations',
          'false_cusp_count', 'centerline_points']
  diff = [(k, key) for k in base for key in keys if base[k].get(key) != per['d6-copy'][k].get(key)]
  check('d6-copy совпадает с baseline', not diff,
        f'расхождений {len(diff)}' + (f', первое {diff[0]}' if diff else ''))

  # Вершина острия в максимуме - по корпусу без острий без стоянки: у тех свой
  # критерий ниже.
  for scale in scales:
      b = max(s['corner_dist_max_px'] for k, s in base.items()
              if k[1] == scale and s.get('corner_dist_max_px') is not None and 'nostop' not in k[0])
      d = max(s['corner_dist_max_px'] for k, s in cand.items()
              if k[1] == scale and s.get('corner_dist_max_px') is not None and 'nostop' not in k[0])
      check(f'x{scale:g}: вершина острия в максимуме <= 0.6 px', d <= 0.6, f'{b} -> {d}')

  for tag in ('s1', 's2.5'):
      b = summary['baseline'][tag]
      d = summary[candidate][tag]
      for key, title, limit in (
              ('facet_count_2deg@sum', 'граней > 2° не больше baseline', None),
              ('curvature_tv_excess_per100px@mean', 'избыток неровности кривизны не больше baseline', None),
              ('settled_mutations@sum', 'окончательные точки не меняются', None)):
          bound = limit if limit is not None else b[key]
          check(f'{tag} {title}', d[key] <= bound, f'{b[key]} -> {d[key]}')

  # Ложные острия: поворот > 45° дальше 2 px от эталонных острий.
  for scale in scales:
      b = sum(s['false_cusp_count'] for k, s in base.items() if k[1] == scale)
      d = sum(s['false_cusp_count'] for k, s in cand.items() if k[1] == scale)
      check(f'x{scale:g}: ложных острий по корпусу не больше baseline', d <= b, f'{b} -> {d}')
  for sample in ('small_o_8', 'small_o_12', 'small_o_20', 'fast_eeee'):
      for scale in scales:
          b = base[(sample, scale)]['false_cusp_count']
          d = cand[(sample, scale)]['false_cusp_count']
          check(f'{sample} x{scale:g}: ложных острий не прибавилось', d <= b, f'{b} -> {d}')

  # Острие без стоянки пера: не хуже baseline.
  for sample in ('corner_N_nostop_240', 'corner_N_nostop_481'):
      for scale in scales:
          b = base[(sample, scale)]['corner_dist_max_px']
          d = cand[(sample, scale)]['corner_dist_max_px']
          check(f'{sample} x{scale:g}: вершина острия не хуже baseline', d <= b, f'{b} -> {d}')

  # Зубец короче кадра: tst_strokesampler::toothShorterThanFrameSurvives требует
  # половину высоты; гейт - с запасом 5% от порога.
  for scale in scales:
      b = base[('tooth_240', scale)]['tooth_peak_ratio']
      d = cand[('tooth_240', scale)]['tooth_peak_ratio']
      check(f'tooth_240 x{scale:g}: зубец >= 0.525 высоты', d >= 0.5 * 1.05, f'{b} -> {d}')

  # Для записи в design.md, не гейт: где ложных острий прибавилось.
  for (sample, scale), b in sorted(base.items()):
      d = cand[(sample, scale)]
      if d['false_cusp_count'] > b['false_cusp_count']:
          print(f'note  {sample} x{scale:g}: ложных острий {b["false_cusp_count"]} -> '
                f'{d["false_cusp_count"]}')

  print('ГЕЙТ D6 ПРОЙДЕН' if passed else 'ГЕЙТ D6 НЕ ПРОЙДЕН')
  sys.exit(0 if passed else 1)
  ```

  В `tools/stroke-lab/README.md` в конец добавить раздел:

  ````markdown
  ## Гейт D6 (ink-beauty-pass)

  Узел острия в нарезке и окно [1, 2, 1]/4 на входе следования
  (`variants/d6_raw121_cusp.cpp`) проверяются против приложения без них:

  ```sh
  ./build/tools/stroke-lab/stroke-lab --variants baseline,d6-copy,d6-raw121-cusp --no-png --out /tmp/uq-gate-d6
  python3 tools/stroke-lab/gate_d6.py /tmp/uq-gate-d6
  ```

  `d6-copy` - копия сэмплера без правок, сверка того, что копия не разошлась с
  приложением. Ложные острия - `false_cusp_count`, острие без стоянки пера -
  `corner_N_nostop_240/481`, зубец короче кадра - `tooth_240`
  (`tooth_peak_ratio`). На шаге 2 ink-beauty-pass гейт не пройден (design.md, D6).
  ````

- [x] 8.5 **Прогон гейта.**

  ```sh
  cmake --build build -j$(nproc) --target stroke-lab
  rm -rf /tmp/uq-gate-d6
  ./build/tools/stroke-lab/stroke-lab --variants baseline,d6-copy,d6-raw121-cusp --no-png --out /tmp/uq-gate-d6
  python3 tools/stroke-lab/gate_d6.py /tmp/uq-gate-d6 | tee /tmp/uq-gate-d6/gate.txt
  ```

  Ожидается (по прогону в scratchpad), на каждом из масштабов 1 и 2.5:

  ```text
  ok    d6-copy совпадает с baseline: расхождений 0
  ok    x1: вершина острия в максимуме <= 0.6 px: 1.413 -> 0.485
  ok    s1 граней > 2° не больше baseline: 2894 -> 1455
  ok    s1 избыток неровности кривизны не больше baseline: 3.0014666666666674 -> 1.5207666666666668
  ok    s1 окончательные точки не меняются: 1 -> 1
  FAIL  x1: ложных острий по корпусу не больше baseline: 82 -> 94
  ok    small_o_8 x1: ложных острий не прибавилось: 0 -> 0
  ok    small_o_12 x1: ложных острий не прибавилось: 1 -> 1
  ok    small_o_20 x1: ложных острий не прибавилось: 0 -> 0
  FAIL  fast_eeee x1: ложных острий не прибавилось: 3 -> 4
  ok    corner_N_nostop_240 x1: вершина острия не хуже baseline: 1.055 -> 0.938
  ok    corner_N_nostop_481 x1: вершина острия не хуже baseline: 1.222 -> 0.293
  FAIL  tooth_240 x1: зубец >= 0.525 высоты: 0.9875 -> 0.5
  note  fast_scribble x1: ложных острий 1 -> 3
  note  slow_arc_tremor x1: ложных острий 1 -> 3
  note  small_e_10 x1: ложных острий 2 -> 3
  note  word_minimum x1: ложных острий 18 -> 22
  note  word_minimum_small x1: ложных острий 20 -> 24
  note  word_privet x1: ложных острий 12 -> 14
  ГЕЙТ D6 НЕ ПРОЙДЕН
  ```

  код выхода 1. После шага 1 числа давления-зависимых метрик могут чуть сдвинуться
  (задевы зависят от пика давления), но три провала - по разным причинам: лишние
  повороты от узла острия и срез зубца окном. Если первая строка `FAIL` - это
  расхождение копии с `src/ink/strokesampler.cpp` после шага 1, а не результат D6:
  привести копию в 8.3 к коду приложения и повторить 8.5. Если вдруг все строки
  `ok` (`ГЕЙТ D6 ПРОЙДЕН`) - остановиться и сообщить пользователю: D6 включается
  отдельным изменением, в этот план продуктовые правки сэмплера не входят.

- [x] 8.6 **Закоммитить стенд и сверить результат гейта с design.md.** Сначала
  коммит стенда (его хэш нужен в абзаце дизайна):

  ```sh
  git add tools/stroke-lab/metrics.cpp tools/stroke-lab/corpus.h tools/stroke-lab/corpus.cpp \
          tools/stroke-lab/variants/d6_raw121_cusp.cpp tools/stroke-lab/gate_d6.py \
          tools/stroke-lab/README.md
  git commit -F - <<'EOF'
  Стенд: гейт D6

  Вариант d6-raw121-cusp - копия сэмплера с узлом острия и окном [1, 2, 1]/4,
  d6-copy - та же копия без них для сверки с приложением. Метрики
  false_cusp_count (поворот круче 45° дальше 2 px от эталонных острий) и
  tooth_peak_ratio, сэмплы острия без стоянки пера на 240 и 481 Гц и зубца
  короче кадра; gate_d6.py проверяет критерии design.md, D6.

  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  EOF
  git log -1 --format=%h
  ```

  Затем сверить `/tmp/uq-gate-d6/gate.txt` с числами в design.md, раздел «D6»
  (они сняты при составлении плана на копии кода). Если хоть одно число
  отличается, заменить его в абзаце «Гейт не пройден» на число прогона, дописать
  в конец абзаца «(повторный прогон <дата>, коммит стенда <hash>)» и закоммитить:

  ```sh
  git add openspec/changes/ink-beauty-pass/design.md
  git commit -F - <<'EOF'
  Повторный прогон гейта D6 в дизайне

  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  EOF
  ```

  Если числа совпали, design.md не трогать.

## 9. Финальная проверка шага 2, версия 0.9.0

**Files:**
- Modify: `CMakeLists.txt`, `packaging/PKGBUILD`

- [x] 9.1 **Полный прогон тестов.**

  ```sh
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUQ_BUILD_STROKE_LAB=ON
  cmake --build build -j$(nproc)
  ctest --test-dir build --output-on-failure
  ```

  Ожидается: `100% tests passed` (`tst_idleframes` без графической сессии -
  SKIP). Отдельно убедиться, что `tst_export` зелёный - это гейт шага 2 из
  design.md D9: `ctest --test-dir build -R tst_export --output-on-failure`.

- [x] 9.2 **Гейт `--endcheck`: торцы и касание.**

  ```sh
  ./build/tools/stroke-lab/stroke-lab --endcheck --variants baseline --width 2.4
  ```

  Ожидается: в первой таблице у `baseline` три строки (100/300/600 px/с), в
  столбце `cap_dev` везде `0,000` - не больше 0.05 (критерий D9: отклонение
  вершин торца от окружности радиуса полутолщины ≤ 0.05r; до шага 2 там стояло
  1.000 - «ничего за концом»). Во второй таблице `tap: points` = 2 и `aspect`
  между 1.1 и 1.3 (капсула 0.3 + 2r на 2r; в scratchpad было 1.19-1.22; до шага 2
  ≈ 0.2 - отрезок поперёк). Если `cap_dev` > 0.05 - торец не на окружности:
  проверить, что `geometry()` варианта `baseline` строится `tessellate`, а не
  старым кодом стенда.

- [x] 9.3 **Прогон корпуса стенда и сверка с design.md.**

  ```sh
  rm -rf /tmp/uq-step2
  ./build/tools/stroke-lab/stroke-lab --variants baseline --min-width-factor 0.08 --out /tmp/uq-step2
  sed -n '/### scale 1/,/### scale 2.5/p' /tmp/uq-step2/summary.md
  python3 -c "import json; [print(s['sample'], s['scale'], s['centerline_points']) for s in json.load(open('/tmp/uq-step2/baseline/metrics.json'))['samples'] if s['sample'] == 'dots_taps']"
  ```

  Числа ниже сняты при составлении плана на корпусе без сэмплов `dots_*`
  шага 0. Если они расходятся, сравнивать не с ними, а с прогоном того же
  корпуса на коммите шага 1 (`git worktree add /tmp/uq-step1 <коммит 5.11>`,
  сборка стенда там, та же команда): траектория в шаге 2 не меняется, поэтому
  осевые метрики обязаны совпасть с ним.

  Ожидается: `mut` (settled_mutations) не больше, чем до шага 2; траектория в
  шаге 2 не менялась (D6 отложен), поэтому осевые метрики те же, что на шаге 1, с
  поправкой на сэмплы 8.2 (в scratchpad: `corner max` 1.413, `facets>2°` 2894,
  `curvTV excess` 3.00 на корпусе с `corner_N_nostop_*` и `tooth_240`). В `dots_taps` (если шаг 0 перенёс этот сэмпл из
  `exp/google-ink`) `centerline_points` ≥ 12: все шесть касаний дали штрих, ни
  одно не отброшено как задев. Глазами: `/tmp/uq-step2/compare/dots_taps_s2.5.png`
  - круглые точки, `/tmp/uq-step2/compare/corner_N_s2.5.png` - круглые концы.

- [x] 9.4 **Версия 0.9.0.** `CMakeLists.txt`, строка 2: `project(uq-notes VERSION 0.8.0 LANGUAGES CXX)`
  → `project(uq-notes VERSION 0.9.0 LANGUAGES CXX)`. `packaging/PKGBUILD`:
  `pkgver=0.8.0` → `pkgver=0.9.0` (`pkgrel=1` не меняется).

  ```sh
  cmake --build build -j$(nproc) && ctest --test-dir build -R tst_buildenv --output-on-failure
  grep -n "VERSION 0.9.0" CMakeLists.txt && grep -n "pkgver=0.9.0" packaging/PKGBUILD
  ```

  Ожидается: обе строки найдены, `tst_buildenv` зелёный.

- [x] 9.5 **Финальный коммит шага 2.** Числа в абзаце про D6 - из
  `/tmp/uq-gate-d6/gate.txt` (ниже - прогона в scratchpad):

  ```sh
  git add CMakeLists.txt packaging/PKGBUILD
  git commit -F - <<'EOF'
  Круглые концы штриха и точки от касания, версия 0.9.0

  Концы штриха теперь круглые: торец - полудиск радиуса полуширины, осевая
  кончается в точке касания и в точке отрыва, а торец выступает за неё на
  полуширину. Направление торца берётся по хорде длиной не меньше радиуса, а
  не по последнему отрезку: дотяжка до точки контакта на отрыве дописывает
  отрезок короче шага в случайную сторону, и плоский торец по нему косил. Торец
  собран из восьми пар вершин на четверть оборота, поэтому полоса сохраняет
  чётность: контур экспорта, мокрая полоса и её куски работают без изменений, а
  сухая полоса совпадает с мокрой вершина в вершину. На стенде отклонение
  вершин торца от окружности - 0.000 радиуса; толстое перо на крупном масштабе
  больше не теряет чернила на концах.

  Касание пером без движения оставляет точку. Осевая короче толщины пера
  рисуется капсулой, касание без сдвига - диском; к такому штриху на отрыве
  дописывается точка с самым сильным нажимом касания, и толщина точки - по
  этому нажиму (как в прототипе outline-caps: касание со сдвигом в доли толщины
  - это две точки с давлением краёв касания, и без пиковой точки капсула
  выходила тоньше нажима). Правило зависит только от сохранённых точек, поэтому точка та же в
  заметке, после отмены и повтора и в экспорте, и стирается ластиком, как любой
  штрих. Видна она уже во время касания: пока штрих может оказаться точкой, вся
  мокрая полоса строится тем же правилом, что сухая. Касание короче 30 мс
  легче 0.05 считается задевом и следа не оставляет; мышь давления не
  сообщает, её щелчок ставит точку. Касание пером кнопок, значков вкладышей и
  пунктов оглавления по-прежнему забирает интерфейс - тесты режима «Только
  перо» и вкладышей теперь проверяют это с контрольной точкой на странице.

  Узел острия в нарезке и окно [1, 2, 1]/4 на входе следования гейт на
  стенде не прошли и отложены в изменение про траекторию. Вершину острия они
  возвращают (1.41 → 0.49 px) и шум датчика гасят (граней 2894 → 1455), но
  узел острия добавляет ложные острия на быстрых петлях и треморе (82 → 94 по
  корпусу, fast_eeee 3 → 4), а окно срезает зубец в один отсчёт на 240 Гц
  ровно вдвое - до порога теста. Стенд получил то, чем это мерить: ложные
  острия вдали от эталонных, острие без стоянки пера на 240 и 481 Гц, зубец
  короче кадра; результат записан в design.md, D6.

  Версия поднята до 0.9.0 в CMakeLists.txt и PKGBUILD.

  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  EOF
  git log -1 --stat
  ```

  Ожидается: коммит с изменёнными `CMakeLists.txt` и `packaging/PKGBUILD`.

---

### Что этот раздел не покрывает

- Полувеер торца с центральной вершиной и `outline()` из примитивов - шаг 3 (D7);
  пары вершин здесь сознательно временные.
- Узел острия и окно [1, 2, 1]/4 (D6): гейт не пройден, правки отложены в
  изменение про траекторию; основа - вариант стенда `d6-raw121-cusp`.
- Запись с устройства и проверку D6 на реальном шуме: гейт - только на
  синтетике корпуса.
- `DocumentCanvas` не меняется; мокрая точка показывается через `WetStroke`.

### Допущения раздела, которые не проверены

- Шаг 1 оставил в `StrokeSampler` прежнюю структуру (`append`, `finish`,
  `followedNode`, `rawAt`, `walkTowards`, `emitSettled`, `rebuildProvisional`,
  поле `m_rawHistory` с отфильтрованным давлением) и добавил `pressureAt(time)`;
  точка отрыва получает `m_lastInputPressure`. Правки 7.9 привязаны к
  этим местам; где шаг 1 мог написать иначе, это оговорено в шаге.
- После шага 1 `WetStroke::begin` зовёт `setStyle`, а `StrokeTessellator` по
  умолчанию - стиль `Legacy` (доля 0.25): тесты сравнивают мокрую полосу с
  `tessellate` при `setStyle(stroke.style)`.
- Стенд шага 0 содержит `--endcheck` из `exp/apps-survey` с той же таблицей
  (`cap_dev`, `tap ... aspect`), `summary.json` с ключами `<метрика>@max|@sum|@mean`
  и тегами масштабов `s1`, `s2.5`, корпус с `small_o_8/12/20` и `fast_eeee`; блок
  «2. Грани» в `metrics.cpp`, структуры `Spec`/`Sample` и функция `realize()` в
  `corpus.cpp` те же, что в `exp/stroke-lab` (правки 8.1-8.2 привязаны к ним).
- `tst_canvasink`, `tst_penonlymode`, `tst_insertoverlay`, `tst_export` в этой
  среде не собирались (нет `libqpdf`): координаты контрольной точки в
  `tst_insertoverlay` (`pageRect(0).topLeft() + (100, 450)`) и наличие «4 Conclusion»
  при открытом вкладыше в `tst_penonlymode` проверяются только прогоном 7.11.
- Числа гейта D6 получены с давлением до шага 1; на осевую давление не влияет
  (проверено: `d6-copy` совпала с `baseline`), но от пика давления зависит
  отбрасывание задевов, поэтому `centerline_points` на сэмплах с короткими
  касаниями после шага 1 может отличаться от предварительного прогона.


# Шаг 3: материал чернил

Исходное состояние: шаги 0-2 выполнены. Стенд в `tools/stroke-lab`, собирается при
`-DUQ_BUILD_STROKE_LAB=ON`; `StrokeStyle` и `StrokeTessellator::setStyle`; `CapEnd`,
`emitCap(..., CapEnd)` с парами K = 8, `capDirection`, `isDot` и `WetStroke::withDotTwin`;
капсула касания в `tessellate()`. D6 из изменения исключён (гейт D6 не пройден):
`StrokeSampler` в шаге 2 не меняется, и группы 10-14 от D6 не зависят - ни код, ни
тесты, ни ожидаемые числа стенда (все сняты без D6). MSAA 4x в `main.cpp` в этом шаге
остаётся (выключение - шаг 4, D8).

Сборка и прогоны во всех группах:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUQ_BUILD_STROKE_LAB=ON
cmake --build build -j$(nproc)
ctest --test-dir build -R <tst_name> --output-on-failure
```

### Что проверено сборкой при составлении плана

Копия `src/ink` и `tests` без D6 (`scratchpad/p3n`): шаг 2 по плану из `scratchpad/p2/mod`
(торцы, точка, `withDotTwin`; `StrokeSampler` - только пик давления D5), сэмплер шага 1
(D2) из `scratchpad/probe1/new` с наложенным пиком D5, закон толщины шага 1, затем код
этого раздела. Отдельные CMake-проекты: libqpdf в среде нет, поэтому без
`DocumentCanvas`, `NoteExporter` и QML.

| Что | Результат |
|---|---|
| Группа 10 на стенде шага 0 (`scratchpad/p0/tree3`) с приложением после шагов 1-2 без D6 | собирается; `aa-fringe` после правки не изменился до бита (15 растровых ключей); гейт `gate-rho.sh` пройден с принятой ценой: две строки B на петлях хуже варианта без предела на 2.3 и 1.0/255 при допуске 3/255 (см. 10.10), около 30 с |
| `aa-gpu-bench` из `exp/antialias` в стенде шага 0 | собирается и работает (Iris Xe, Mesa GL) |
| Группа 11: `tst_stroketessellator`, `tst_wetstroke`, `tst_strokesampler`, `tst_inkcoverage` | `Totals`: 25, 23, 37, 8 passed (с init/cleanup), 0 failed - без D6; на копии с D6 было бы так же, группа от D6 не зависит; мутация «без предела ρ» ловится |
| Стенд с приложением после группы 11: `app-ink` против `aa-fringe-rho` | собирается после переименования структуры прототипа (11.13); снимки совпадают, кроме 0-2 пикселей у торцов (без D6) |
| Шейдеры `qsb --batchable --glsl "100 es,120,150" --hlsl 50 --msl 12` | собраны; входы `center`=0, `dir`=1, `hw`=2, `s`=3, `_qt_order`=7 (батчируемый вариант); униформы `qt_Matrix` 0, `color` 64, `pxPerUnit` 80, `minHwPx` 84, `fringePx` 88, размер блока 92 |
| `InkMaterial` + `qt_add_shaders` в статической библиотеке + `tst_inkmaterial` | 11/11; ресурсы `.qsb` доходят до теста без ручной инициализации |
| `tst_inkmaterialgpu` (12.9): граф сцены Qt Quick через `QQuickRenderControl` на настоящем RHI | Iris Xe, OpenGL: профиль кромки совпадает с CPU-зеркалом до 0.002 на зумах 1, 2, 1.5, 3; 80 узлов в слитом батче (`merged: 1`), зум - только матрица, батч не перезаливается. Под `offscreen` - SKIP. Мутация «pxPerUnit без матрицы» ловится |
| Группа 13: `tst_inkparity` | `Totals: 7 passed` (без D6); исправлен most vexing parse; мутация торца контура ловится |
| `QSG_RENDERER_DEBUG=upload` | вывод уходит в журнал, если stderr не терминал: нужен `QT_FORCE_STDERR_LOGGING=1` (исправлено в 14.2); формат строки ` - batch ... merged: 1 ... vbo: 0x0 : 297920` подтверждён |

Не проверено: всё в `DocumentCanvas` (12.10-12.12, 11.9), `tst_pagescenegraph`,
`tst_canvasink`, `tst_export` (13.4), сборка полного дерева, устройство (группа 14).

### Решения, общие для групп 10-14

**Топология - треугольная полоса остаётся.** Веер вокруг центра стыка и торца
кладётся в полосу повтором центральной вершины: `C, O0, C, O1, ...` - треугольники
`(C, Ok, C)` вырождены, `(Ok, C, Ok+1)` - веер. Так сделано в прототипе
(`exp/antialias`, `aa-fringe`), на нём сняты все числа D7 (+18% вершин). Почему не
индексированные треугольники:

- Батчинг полос в Qt Quick есть. `prepareAlphaBatches` сливает узлы с одинаковыми
  `drawingMode`, `attributes()`, `type()` и `compare() == 0`, а `uploadBatch` для
  `DrawTriangleStrip` сам строит индексы: дублирует первую и последнюю вершину
  каждой полосы (вырожденные треугольники между полосами, `qsg_fixIndexCount`:
  `iCount + 2`). Отсечение граней в Qt Quick выключено, поэтому смена чётности
  на стыке двух полос (полоса нечётной длины) ничего не ломает. Фрагментный
  шейдер берёт `|d|`, ориентация треугольника ему тоже безразлична.
- Куски мокрого штриха по 512 вершин с перекрытием 2 воспроизводят все
  треугольники полосы при любом её содержимом: треугольник `i` - это вершины
  `i, i+1, i+2`. Для индексированных треугольников пришлось бы переиндексировать
  каждый кусок и дублировать вершины на границах.
- `canMerge` требует `indexType() == UnsignedShortType`. У неиндексированной
  полосы его ставит сам Qt и сам режет батч на наборы по 65534 вершины. Штрих
  длиннее 65535 вершин в индексированном виде потребовал бы 32-битных индексов
  и выпал бы из батча.

**Вершина** `AxisVertex {cx, cy, nx, ny, hw, s}` - 24 байта, как в D7. У внутренней
вершины стыка радиус кривизны хранится длиной направления: `|n| = 1 + ρ`
(ρ - в точках страницы, не меньше 0.002). У остальных вершин `|n| = 1`, у центра
веера `n = 0, s = 0`. Отдельный седьмой float вырастил бы VBO плотной страницы
ещё на 17%. Шейдер различает вершины по `|n| > 1.001`. Зеркало шейдера на CPU
(`src/ink/inkcoverage.h`) и шейдер обязаны меняться вместе.

**Предел по кривизне:** `ρ = max(L₋, L₊) / (2·sin(|θ|/2))`, где L₋ и L₊ - длины
соседних сегментов, θ - поворот в стыке. На равномерной нарезке (1.2 px) это
радиус окружности через три точки: внутренняя вершина петли встаёт в её центр
и дальше не идёт. `max`, а не `min` - потому что у конца штриха бывает отрезок
короче шага (`walkTowards`): с `min` предел схлопнул бы внутреннюю вершину в
центр стыка и оставил выемку. Вынос внутренней вершины в шейдере -
`min(hwPx + 1, ρ·pxPerUnit)`, `d = s·вынос`. При таком пределе `d` у вершины
остаётся её точным расстоянием до прямых обоих сегментов, то есть точность
стыка из D7 сохраняется.

**Батчи.** Материал ставит флаги `Blending | RequiresFullMatrixExceptTranslate`.
Батч-рендер переносит на CPU только сдвиг атрибута-позиции (центра `c`), а
масштаб зума остаётся в `qt_Matrix`. Поэтому `n` и `hw` в точках страницы
правильны и в слитом батче. Корень холста меняет матрицу на каждом зуме и при
числе узлов больше 64 становится корнем батча (`turnNodeIntoBatchRoot`), а
страницы под ним сдвинуты только переносом. У мокрого слоя свой `type()`, у
каждого куска мокрого штриха свой ключ в `compare()`. Каждый кусок тогда -
отдельный батч, и на событие пера перезаливается только изменившийся кусок,
а не весь мокрый штрих.

---

## 10. Гейт: ограничение внутреннего выноса по радиусу кривизны (стенд, до кода в приложении)

**Files:**
- Create (из `exp/antialias`, по таблице 1.20 шага 0): `tools/stroke-lab/variants/aa_fringe.cpp`,
  `tools/stroke-lab/variants/aa_fringe.h`, `tools/stroke-lab/variants/app_pipeline.h`,
  `tools/stroke-lab/gpu/{bench.cpp,ink.vert,ink.frag,flat.vert,flat.frag}`
- Modify: `tools/stroke-lab/CMakeLists.txt` (цель `aa-gpu-bench`)
- Modify: `tools/stroke-lab/variants/aa_fringe.cpp` (вариант `aa-fringe-rho`)
- Create: `tools/stroke-lab/gate-rho.sh`
- Modify: `tools/stroke-lab/README.md`
- Modify: `openspec/changes/ink-beauty-pass/design.md` (D7, Risks - результат гейта)

- [x] 10.1 **Перенести в стенд варианты D7 и GPU-замер из `exp/antialias`**

Шаг 0 приносит движок (`aaraster.{h,cpp}`, `rastermetrics`, хук `Variant::rasterize`,
метрику `raster_vs_ideal`), а варианты D7 и `aa-gpu-bench` по его таблице 1.20 переносит
шаг 3. Проверить движок и перенести файлы без изменений:

```
grep -n 'raster_vs_ideal_p99' tools/stroke-lab/main.cpp
grep -n 'virtual bool rasterize' tools/stroke-lab/lab.h
mkdir -p tools/stroke-lab/gpu
for f in variants/aa_fringe.cpp variants/aa_fringe.h variants/app_pipeline.h \
         gpu/bench.cpp gpu/ink.vert gpu/ink.frag gpu/flat.vert gpu/flat.frag; do
  git show exp/antialias:tools/stroke-lab/$f > tools/stroke-lab/$f
done
{ echo; git show exp/antialias:tools/stroke-lab/CMakeLists.txt | sed -n '/^# aa-gpu-bench/,$p'; } \
  >> tools/stroke-lab/CMakeLists.txt
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUQ_BUILD_STROKE_LAB=ON
cmake --build build -j$(nproc) --target stroke-lab aa-gpu-bench
./build/tools/stroke-lab/stroke-lab --list | grep -E '^ *(baseline|aa-fringe)\b'
```

Ожидаемо: обе строки `grep` что-то печатают; сборка без ошибок; в списке `baseline` и
`aa-fringe` (на копии стенда шага 0 с приложением после шага 2 - так и есть). Если
`grep` пуст, шаг 0 не завершён: остановиться и вернуться к шагу 0, движок стенда в
рамках шага 3 не чинить. `aa-gpu-bench` проверяется так:
`QT_QPA_PLATFORM=wayland ./build/tools/stroke-lab/aa-gpu-bench --out /tmp/aa-gpu --frames 5`
- первая строка `GPU: ...`, в столбце fringe GPU↔emu max 1 уровень на масштабе 1.

- [x] 10.2 **Снять контрольный прогон aa-fringe до правки** (правка не должна менять вариант без предела)

```
./build/tools/stroke-lab/stroke-lab --variants aa-fringe --samples corner_M,small_e_10 \
  --scales 1 --dpr 2 --out /tmp/rho-before
```

Ожидаемо: `/tmp/rho-before/aa-fringe/metrics.json` с двумя записями.

- [x] 10.3 **aa_fringe.cpp: поле предела в `AxisVertex` стенда**

В начало файла, к остальным `#include`, добавить `#include <limits>`. Структуру
`AxisVertex` в анонимном пространстве имён заменить целиком:

```cpp
struct AxisVertex
{
    QPointF c;
    QPointF n;
    qreal hw = 0;
    int side = 0;
    qreal dscale = 1.0;
    // aa-fringe-rho: предел выноса внутренней вершины стыка, точки страницы, -
    // радиус кривизны осевой. У остальных вершин предела нет.
    qreal radius = std::numeric_limits<qreal>::infinity();
};
```

- [x] 10.4 **aa_fringe.cpp: `AxisTessellator` получает режим предела и длины сегментов**

Конструктор `AxisTessellator` заменить:

```cpp
    AxisTessellator(const std::function<qreal(const InkPoint &)> &halfWidth, bool exactJoins,
                    bool radiusLimit = false, qreal radiusFactor = 1.0)
        : m_halfWidth(halfWidth)
        , m_exact(exactJoins)
        , m_radiusLimit(radiusLimit)
        , m_radiusFactor(radiusFactor)
    {
    }
```

В `tessellate()` цикл стыков заменить:

```cpp
        for (int i = 1; i + 1 < points.size(); ++i) {
            const QPointF before = points[i].position - points[i - 1].position;
            const QPointF after = points[i + 1].position - points[i].position;
            joint(out, points[i].position, m_halfWidth(points[i]), dirs[i - 1], dirs[i],
                  std::max(std::hypot(before.x(), before.y()), std::hypot(after.x(), after.y())));
        }
```

В конец класса, к `m_halfWidth` и `m_exact`, добавить члены:

```cpp
    bool m_radiusLimit = false;
    qreal m_radiusFactor = 1.0;
```

- [x] 10.5 **aa_fringe.cpp: `joint()` ставит предел внутренней вершине**

Сигнатуру `joint` заменить, начало тела дополнить расчётом радиуса:

```cpp
    void joint(std::vector<AxisVertex> &out, const QPointF &c, qreal hw, const QPointF &in,
               const QPointF &outDir, qreal span) const
    {
        const qreal turn = std::atan2(cross(in, outDir), QPointF::dotProduct(in, outDir));
        const qreal cosHalf = m_exact ? std::cos(turn / 2) : 1.0;
        // Предел по кривизне: внутренние вершины соседних стыков не заходят друг
        // за друга, пока вынос вдоль биссектрисы не больше
        // max(L-, L+) / (2·sin(|θ|/2)) - на равномерной нарезке это радиус
        // окружности через три точки.
        const qreal halfSine = std::sin(std::abs(turn) / 2);
        const qreal radius = m_radiusLimit && halfSine > 1e-6
            ? m_radiusFactor * span / (2.0 * halfSine)
            : std::numeric_limits<qreal>::infinity();
```

Ветку пологого стыка заменить:

```cpp
        if (std::abs(turn) < kRoundJoinThreshold) {
            QPointF b = normalized(normalOf(in) + normalOf(outDir));
            if (b.isNull())
                b = normalOf(outDir);
            AxisVertex l{c, b, hw, +1, cosHalf};
            AxisVertex r{c, -b, hw, -1, cosHalf};
            // Внутренняя сторона - та, куда поворачивает штрих.
            (turn > 0.0 ? l : r).radius = radius;
            out.push_back(l);
            out.push_back(r);
            return;
        }
```

Строку `const AxisVertex I{c, innerN, hw, innerSide, cosHalf};` заменить:

```cpp
        AxisVertex I{c, innerN, hw, innerSide, cosHalf};
        if (!degenerate)
            I.radius = radius;
```

- [x] 10.6 **aa_fringe.cpp: `FringeVariant` - параметр предела и вынос с пределом в `rasterize()`**

Конструктор `FringeVariant` заменить:

```cpp
    FringeVariant(Blend blend, Profile profile, double minHalfWidthPx, bool exactJoins = true,
                  bool radiusLimit = false)
        : m_blend(blend)
        , m_profile(profile)
        , m_minHw(minHalfWidthPx)
        , m_exactJoins(exactJoins)
        , m_radiusLimit(radiusLimit)
    {
    }
```

В `axisStrip()` строку создания тесселятора заменить:

```cpp
        AxisTessellator t([this](const InkPoint &p) { return halfWidthAt(p); }, m_exactJoins,
                          m_radiusLimit, radiusFactor());
```

В `rasterize()` ветку `else` при расчёте вершины заменить:

```cpp
            } else {
                // aa-fringe-rho: внутренняя вершина стыка не дальше радиуса
                // кривизны (в пикселях). d остаётся её точным расстоянием до
                // прямых обоих сегментов: вершина лишь ближе на той же биссектрисе.
                const double reach = std::min(ext, a.radius * s);
                out.p = c + a.n * reach; // T - подобие без отражения
                d = (a.side == -1 ? -reach : reach) * a.dscale;
            }
```

В секцию `private:` класса `FringeVariant` добавить:

```cpp
    // Множитель радиуса для разбора провала гейта (AA_RHO_FACTOR, по умолчанию 1).
    // Гейт гоняется с 1: подбор множителя - решение пользователя, не гейта.
    static qreal radiusFactor()
    {
        static const qreal factor = [] {
            bool ok = false;
            const qreal value = qEnvironmentVariable("AA_RHO_FACTOR").toDouble(&ok);
            return ok && value > 0.0 ? value : 1.0;
        }();
        return factor;
    }

    bool m_radiusLimit = false;
```

(`m_blend`, `m_profile`, `m_minHw`, `m_exactJoins` уже есть.)

- [x] 10.7 **aa_fringe.cpp: зарегистрировать вариант `aa-fringe-rho`**

После `class FringeHairline ... };` добавить:

```cpp
class FringeRho : public FringeVariant
{
public:
    FringeRho() : FringeVariant(Blend::Over, Profile::Box, 0.15, true, true) {}
};
```

После регистрации `aa-fringe-hairline` добавить:

```cpp
STROKE_LAB_VARIANT(FringeRho, "aa-fringe-rho",
                   "как aa-fringe, но вынос внутренней вершины стыка ограничен радиусом "
                   "кривизны осевой max(L-, L+) / (2·sin(|θ|/2)) - против складки бахромы на "
                   "тугих петлях (design.md, D7)");
```

- [x] 10.8 **Собрать стенд и убедиться, что aa-fringe не изменился**

```
cmake --build build -j$(nproc) --target stroke-lab
./build/tools/stroke-lab/stroke-lab --list | grep aa-fringe-rho
./build/tools/stroke-lab/stroke-lab --variants aa-fringe --samples corner_M,small_e_10 \
  --scales 1 --dpr 2 --out /tmp/rho-after
python3 - <<'EOF'
import json
a = json.load(open('/tmp/rho-before/aa-fringe/metrics.json'))['samples']
b = json.load(open('/tmp/rho-after/aa-fringe/metrics.json'))['samples']
keys = [k for k in a[0] if k.startswith(('raster_', 'aa_'))]
diff = [(x['sample'], k, x[k], y[k]) for x, y in zip(a, b) for k in keys if x.get(k) != y.get(k)]
print('aa-fringe не изменился' if not diff else diff)
EOF
```

Ожидаемо: сборка без ошибок, `aa-fringe-rho` в списке, `aa-fringe не изменился`
(растровые метрики совпадают до бита: путь без предела не тронут). На копии так и
вышло: 15 растровых ключей совпали.

- [x] 10.9 **Создать `tools/stroke-lab/gate-rho.sh`**

```bash
#!/usr/bin/env bash
# Гейт D7 (design.md ink-beauty-pass, D9 шаг 3): ограничение внутреннего выноса
# бахромы по радиусу кривизны осевой. Гоняется до кода в приложении.
#
#   tools/stroke-lab/gate-rho.sh [каталог-результатов]
#
# Метрика - raster_vs_ideal_p99: покрытие картинки варианта против идеальной
# круглой кисти по той же осевой, p99 по пикселям с покрытием. Критерии:
#   A. Петли (small_e_10, small_o_8, word_minimum_small): aa-fringe-rho не хуже
#      baseline. Не оцениваются сочетания, где
#        - перо толще петли: полная толщина пера на экране (перо в pt × масштаб)
#          не меньше 0.4 x-height сэмпла - глазок «e» и просвет «o», «n», «m»
#          тогда заливает сама линия, и бахроме сравнивать нечего;
#        - p99 у baseline упирается в 1.000 - метрика там ничего не различает.
#   B. Все сэмплы и сочетания: aa-fringe-rho не хуже aa-fringe с допуском 3/255.
#      Предел местами хуже варианта без него на 1-2.3/255 (small_e_10, small_o_8
#      на масштабе 2.5) - решение пользователя: принять как цену ради петель и
#      острий, где он лучше (design.md, D7).
set -euo pipefail

LAB=${LAB:-./build/tools/stroke-lab/stroke-lab}
OUT=${1:-/tmp/gate-rho}
SAMPLES='small_e_10,small_o_8,word_minimum_small,corner_M,corner_N,corner_check,ref_zigzag,ref_letter_e'

rm -rf "$OUT"
for dpr in 1.75 2; do
    for width in 1 2.6 4.5; do
        echo "dpr $dpr, перо $width pt" >&2
        "$LAB" --variants baseline,aa-fringe,aa-fringe-rho --samples "$SAMPLES" \
            --scales 1,2.5 --width "$width" --dpr "$dpr" \
            --out "$OUT/dpr${dpr}_w${width}" > /dev/null
    done
done

python3 - "$OUT" <<'EOF'
import json, pathlib, sys

root = pathlib.Path(sys.argv[1])
KEY = "raster_vs_ideal_p99"
TOL = 1.0 / 255
TOL_B = 3.0 / 255
# x-height сэмплов петель в экранных px - как в tools/stroke-lab/corpus.cpp.
XHEIGHT = {"small_e_10": 10.0, "small_o_8": 8.0, "word_minimum_small": 8.0}
failures, rows = [], []

for run in sorted(p for p in root.iterdir() if p.is_dir()):
    table = {}
    for variant in ("baseline", "aa-fringe", "aa-fringe-rho"):
        data = json.loads((run / variant / "metrics.json").read_text())
        width = data["width_pt"]
        table[variant] = {(s["sample"], s["scale"]): s.get(KEY) for s in data["samples"]}
    for key in sorted(table["aa-fringe-rho"]):
        sample, scale = key
        base = table["baseline"].get(key)
        free = table["aa-fringe"].get(key)
        rho = table["aa-fringe-rho"].get(key)
        if None in (base, free, rho):
            failures.append(f"{run.name} {sample} x{scale}: нет {KEY}")
            continue
        note = ""
        if sample in XHEIGHT:
            if width * scale >= 0.4 * XHEIGHT[sample]:
                note = "A: перо толще петли"
            elif base >= 0.999:
                note = "A: p99 baseline = 1"
            elif rho > base + TOL:
                failures.append(f"A {run.name} {sample} x{scale:g}: rho {rho:.3f} > baseline {base:.3f}")
        if rho > free + TOL_B:
            failures.append(f"B {run.name} {sample} x{scale:g}: rho {rho:.3f} > aa-fringe {free:.3f}")
        rows.append((run.name, sample, scale, base, free, rho, note))

print("| прогон | сэмпл | масштаб | baseline | aa-fringe | aa-fringe-rho | не оценивается |")
print("|---|---|---|---|---|---|---|")
for run, sample, scale, base, free, rho, note in rows:
    print(f"| {run} | {sample} | {scale:g} | {base:.3f} | {free:.3f} | {rho:.3f} | {note} |")
print()
if failures:
    print("ГЕЙТ НЕ ПРОЙДЕН:")
    print("\n".join(failures))
    sys.exit(1)
print("ГЕЙТ ПРОЙДЕН")
EOF
```

```
chmod +x tools/stroke-lab/gate-rho.sh
```

- [x] 10.10 **Прогнать гейт**

```
tools/stroke-lab/gate-rho.sh /tmp/gate-rho | tee /tmp/gate-rho.txt
```

Таблица на 6 прогонов × 8 сэмплов × 2 масштаба, около 30 секунд; столбец «не
оценивается» помечает сочетания, исключённые из критерия A. Сравнение идёт с baseline
того же прогона: после шагов 1-2 baseline сдвинулся с 0.24 из отчёта.

**Что получилось на копии** (стенд шага 0, приложение после шагов 1-2 без D6,
`scratchpad/gate-rho-n.txt`, пересчёт по критерию B с допуском 3/255):
`ГЕЙТ ПРОЙДЕН`, код 0.

- Критерий A выполнен во всех оцениваемых сочетаниях. Исключено 18 строк из 36 по
  петлям: перо толще петли (`width × scale ≥ 0.4 x-height`) - все сочетания пера 4.5 и
  пера 2.6 на масштабе 2.5; p99 = 1 у baseline не встретился.
- Острия: предел нигде не хуже `aa-fringe`.
- Хуже `aa-fringe` в двух строках, обе в пределах допуска B: `small_e_10` при dpr 1.75,
  пере 1 pt, масштабе 2.5: 0.110 → 0.119 (2.3/255); `small_o_8` при пере 4.5 pt на
  масштабе 2.5: 0.683 → 0.687 (1.0/255, перо толще петли). Это цена, принятая
  пользователем (design.md, D7).
- В пользу предела 24 сочетания из 96: `word_minimum_small` 0.216 → 0.167 (dpr 1.75,
  перо 1, x2.5), `corner_N` 0.093 → 0.066 (dpr 2, перо 2.6, x2.5), `small_o_8` 0.194 →
  0.093 (dpr 1.75, перо 4.5, x1); среднее p99 по всем строкам 0.146 → 0.140.

Если при исполнении строка B превышает 3/255 или появляется строка A - это провал
(10.12), допуск не расширять.

- [x] 10.11 **Посмотреть складку глазами**

Открыть рядом `/tmp/gate-rho/dpr2_w1/aa-fringe/png/` и `/tmp/gate-rho/dpr2_w1/aa-fringe-rho/png/`:
снимки `small_e_10` при масштабе 1 и их `_ideal`, а также увеличения из `crops/`. Тот же
случай в прототипе - `git show exp/outline:tools/stroke-lab/README.md`, `e2_fold_small_e_s1_dpr2`.
Критерий: петля «e» у `aa-fringe-rho` не бледнее и не залита сильнее, чем `_ideal`; на
`corner_M` и `corner_N` вершина острия не стала тупее, чем у `aa-fringe`.

- [x] 10.12 **Действие при провале гейта**

Если 10.10 печатает `ГЕЙТ НЕ ПРОЙДЕН` (код 1) или 10.11 показывает складку:
- группы 11-14 не начинать, кода в `src/` не писать;
- запасной путь из Risks (капсулы на пару точек с объединением внутри штриха,
  `outline-sdf`) **не реализовывать**: он требует слоя «один раз на штрих» и
  вдвое больше вершин, это решение пользователя;
- вернуться к пользователю: `/tmp/gate-rho.txt`, список строк A/B, пары снимков из 10.11
  и, для разбора, тот же гейт с `AA_RHO_FACTOR=1.5` и `AA_RHO_FACTOR=0.75`
  (`AA_RHO_FACTOR=1.5 tools/stroke-lab/gate-rho.sh /tmp/gate-rho-15`). Вариант стенда
  можно закоммитить как исследовательский (`Стенд: вариант aa-fringe-rho`), но абзац D7
  в design.md тогда вернуть к «не проверено».

- [x] 10.13 **При проходе сверить результат с design.md**

В `openspec/changes/ink-beauty-pass/design.md`, D7, в пункте «Внутренний вынос ограничен
радиусом кривизны» уже стоит абзац о результате гейта и принятой цене (вписан по решению
пользователя, текст - ниже). Сверить его числа с `/tmp/gate-rho.txt`: строки
`word_minimum_small` dpr1.75_w1 x2.5, `corner_N` dpr2_w2.6 x2.5, `small_e_10` dpr1.75_w1
x2.5, `small_o_8` dpr1.75_w4.5 x2.5, число строк, где `aa-fringe-rho` < `aa-fringe`
(24 из 96), и что на `corner_*`, `ref_zigzag` предел нигде не хуже. При расхождении
обновить числа в абзаце на значения прогона.

```
Предел - `max(L₋, L₊) / (2·sin(|θ|/2))`, где L₋ и L₊ - длины соседних
сегментов, θ - поворот в стыке; на равномерной нарезке это радиус окружности через
три точки. В вершине он хранится длиной направления (`|n| = 1 + ρ`), поэтому
вершина остаётся 24-байтной. Гейт на стенде (`aa-fringe-rho`,
`tools/stroke-lab/gate-rho.sh`; dpr 1.75 и 2, перья 1, 2.6 и 4.5 pt, масштабы 1 и
2.5; метрика `raster_vs_ideal_p99`): на петлях (`small_e_10`, `small_o_8`,
`word_minimum_small`) предел не хуже baseline во всех оцениваемых сочетаниях. Не
оцениваются сочетания, где полная толщина пера на экране не меньше 0.4 x-height
сэмпла - петлю там заливает сама линия, - и где p99 у baseline равен 1. Против
варианта без предела он лучше в 24 сочетаниях из 96 (`word_minimum_small` 0.216 →
0.167 при dpr 1.75, пере 1 pt и масштабе 2.5; `corner_N` 0.093 → 0.066 при dpr 2,
пере 2.6 pt и масштабе 2.5) и хуже в двух: `small_e_10` 0.110 → 0.119 (2.3/255;
dpr 1.75, перо 1 pt, масштаб 2.5) и `small_o_8` 0.683 → 0.687 (1/255; перо 4.5 pt
на масштабе 2.5, линия толще петли). Это принятая цена. На остриях (`corner_*`,
`ref_zigzag`) предел не хуже варианта без него.
```

Если в Risks ещё прежний текст пункта «Складка бахромы на тугих петлях», заменить его.


```
- **Складка бахромы на тугих петлях.** Закрыта пределом выноса по радиусу кривизны, гейт
  на стенде пройден (D7). Остаётся риск на записях с устройства: петли там могут
  оказаться туже синтетических. Тогда тот же `tools/stroke-lab/gate-rho.sh` надо
  прогнать на записях. Запасной путь - капсулы на пару точек с объединением внутри
  штриха (`outline-sdf`): нужен слой «один раз на штрих» и вдвое больше вершин.
```

- [x] 10.14 **Дописать стенд в README**

В `tools/stroke-lab/README.md`, в конец раздела о вариантах сглаживания, добавить:

```
`aa-fringe-rho` - как `aa-fringe`, но вынос внутренней вершины стыка ограничен радиусом
кривизны осевой `max(L₋, L₊) / (2·sin(|θ|/2))` (D7). Множитель радиуса для разбора -
переменная `AA_RHO_FACTOR` (по умолчанию 1). Гейт шага 3:

    tools/stroke-lab/gate-rho.sh /tmp/gate-rho

Он гоняет baseline, aa-fringe и aa-fringe-rho при dpr 1.75 и 2, перьях 1, 2.6 и 4.5 pt и
масштабах 1 и 2.5 на тугих петлях (`small_e_10`, `small_o_8`, `word_minimum_small`) и
остриях. Критерии: на петлях `raster_vs_ideal_p99` не хуже baseline, кроме сочетаний
«перо толще петли» (толщина пера на экране ≥ 0.4 x-height) и p99 = 1 у baseline; везде
не хуже aa-fringe с допуском 3/255.
```

- [x] 10.15 **Коммит**

```
git add tools/stroke-lab/variants/aa_fringe.cpp tools/stroke-lab/variants/aa_fringe.h \
        tools/stroke-lab/variants/app_pipeline.h tools/stroke-lab/gpu tools/stroke-lab/CMakeLists.txt \
        tools/stroke-lab/gate-rho.sh tools/stroke-lab/README.md \
        openspec/changes/ink-beauty-pass/design.md
git commit -m "$(cat <<'EOF'
Стенд: гейт предела выноса по радиусу кривизны

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
)"
```

---

## 11. Оболочка AxisVertex: точные стыки, полувеер торцов, зеркало покрытия

> **Изменение при исполнении (решение пользователя после группы 10, design.md
> D7):** предела внутреннего выноса по радиусу кривизны нет. Ограничение прошло
> гейт, но вырезало пиксели внутри чернил на вершинах арок «m» и «n». Поэтому
> в группах 11 и 12 всё, что относится к ρ, не реализуется: нет
> `curvatureRadius`, нет кодирования `|n| = 1 + ρ` и `AxisVertex::kRadiusTag`
> (направление вершины всегда единичное, кроме нулевого у центров вееров), нет
> теста `innerReachIsLimitedByCurvatureRadius`, в `ink.vert` вынос всегда
> `ext` без ветки по метке. Остальное - точные стыки, веер вокруг центра,
> полувеер торцов, пол толщины, зеркало покрытия - как написано. В 11.13-11.14
> вариант `app-ink` сверяется с `aa-fringe`, а не с `aa-fringe-rho`.


Шаги 11.1-11.12 собраны и прогнаны на копии `src/ink` + `tests` без D6 (`scratchpad/p3n`:
шаг 2 по плану из `scratchpad/p2/mod`, шаг 1, затем этот раздел; отдельный CMake-проект
без `DocumentCanvas`/`NoteExporter`). Результаты: `tst_stroketessellator` - `Totals: 25
passed`, `tst_wetstroke` - 23, `tst_strokesampler` - 37, `tst_inkcoverage` - 8. От D6
группа не зависит: она трогает тесселятор, мокрую полосу и их тесты, сэмплер - нет.
Мутации проверены: без предела по ρ падает `innerReachIsLimitedByCurvatureRadius`.

Внимание: `outline()` переписывается здесь же, а не в группе 13. Старый контур
читает `.x/.y` вершин полосы, которых больше нет, и собирает чётные и нечётные
вершины в две стороны - с центрами вееров эта схема не работает (D7). Без нового
контура промежуточный коммит не собрался бы или экспортировал мусор. Группа 13
закрепляет паритет контура и полосы тестами.

**Files:**
- Modify (целиком): `src/ink/stroketessellator.h`, `src/ink/stroketessellator.cpp`
- Modify: `src/ink/wetstroke.cpp` (радиус в стыках)
- Create: `src/ink/inkcoverage.h` (CPU-зеркало шейдеров)
- Modify: `src/CMakeLists.txt` (`ink/inkcoverage.h` в SOURCES)
- Modify: `src/canvas/documentcanvas.cpp` (`uploadStrip` - мост до материала группы 12)
- Test: `tests/tst_stroketessellator.cpp`, `tests/tst_wetstroke.cpp`, `tests/tst_canvasink.cpp`
- Create: `tests/tst_inkcoverage.cpp`; Modify: `tests/CMakeLists.txt`
- Modify: `tools/stroke-lab/variants/baseline.cpp`, `tools/stroke-lab/variants/app_pipeline.h`,
  `tools/stroke-lab/variants/aa_fringe.cpp` (переименование структуры) и прочие варианты
  стенда, которые берут `StrokeTessellator::Vertex` (`d6_raw121_cusp.cpp`, если шаг 2 его оставил)
- Create: `tools/stroke-lab/variants/app_ink.cpp`

- [x] 11.1 **Падающие тесты геометрии в `tests/tst_stroketessellator.cpp`**

Файл после шага 2 (шаг 2, 6.1-6.3, 7.1). В анонимном пространстве имён функцию `toPoint`
заменить: имя остаётся, теперь это вершина «в покое», - и добавить `nearPoint`:

```cpp
// Вершина полосы «в покое» - без выноса бахромы, на полуширине (у внутренней
// вершины стыка - не дальше радиуса кривизны). Это та же фигура, что раньше
// давала плоская полоса; у центра веера - сам центр.
QPointF toPoint(const Vertex &v)
{
    return v.at(v.hw);
}

bool nearPoint(const Vertex &v, const QPointF &p, qreal tolerance = 1e-3)
{
    return std::hypot(v.cx - p.x(), v.cy - p.y()) < tolerance;
}
```

Блок от `// Вершин в одном торце: K + 1 пар.` до конца функции `sideWidths` (константа
`kCapVertices`, `capCenter`, `sideWidths`) заменить:

```cpp
// Вершин в торцах: полувеер из K + 1 центров и K + 1 вершин дуги, у начального
// торца ещё правая вершина боковой пары, у конечного - вся боковая пара.
constexpr int kStartCapVertices = StrokeTessellator::kStartCapVertices;
constexpr int kEndCapVertices = StrokeTessellator::kEndCapVertices;

// Центр торца - центр любой его вершины: первая вершина полосы - центр
// начального полувеера, последняя - вершина дуги конечного.
QPointF capCenter(const QVector<Vertex> &strip, StrokeTessellator::CapEnd end)
{
    return end == StrokeTessellator::CapEnd::Start ? strip.constFirst().center()
                                                   : strip.constLast().center();
}

// Кончик торца - вершина дуги на оси штриха (φ = π/2), в покое.
QPointF capTip(const QVector<Vertex> &strip, StrokeTessellator::CapEnd end)
{
    const int half = StrokeTessellator::kCapFanSteps / 2;
    return end == StrokeTessellator::CapEnd::Start
        ? toPoint(strip[2 * half + 1])
        : toPoint(strip[int(strip.size()) - 1 - 2 * (StrokeTessellator::kCapFanSteps - half)]);
}

// Полные толщины поперечных пар: боковая пара начального торца, пары на
// биссектрисах, боковая пара конечного. У прямого штриха это ровно по одной
// паре на точку.
QList<qreal> sideWidths(const QVector<Vertex> &strip)
{
    QList<qreal> widths;
    for (int i = 0; i + 1 < strip.size(); ++i) {
        const Vertex &a = strip[i];
        const Vertex &b = strip[i + 1];
        if (a.isCenter() || b.isCenter() || a.s <= 0.0f || b.s != -a.s || a.cx != b.cx
            || a.cy != b.cy)
            continue;
        widths.append(QLineF(toPoint(a), toPoint(b)).length());
        ++i;
    }
    return widths;
}
```

Перед `void degenerateInputIsHandled()` вставить новые слоты:

```cpp
    // ink-beauty-pass, D7: центр каждой вершины - точка осевой; направление
    // выноса нулевое (центр веера), единичное или у внутренней вершины стыка
    // несёт радиус кривизны: |n| = 1 + ρ; |s| не больше единицы.
    void axisVerticesAreWellFormed()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(5.0);
        const QList<InkPoint> points = reference::fromPositions(reference::letterE());
        const QVector<Vertex> strip = tessellator.tessellate(points);
        QVERIFY(!StrokeTessellator::isDegenerate(strip));

        for (int i = 0; i < strip.size(); ++i) {
            const Vertex &v = strip[i];
            bool onAxis = false;
            for (const InkPoint &point : points) {
                if (nearPoint(v, point.position)) {
                    onAxis = true;
                    break;
                }
            }
            QVERIFY2(onAxis, qPrintable(QStringLiteral("центр вершины %1 не на осевой").arg(i)));
            QVERIFY(v.hw > 0.0f);
            QVERIFY(std::abs(v.s) <= 1.0f + 1e-6f);
            if (v.isCenter()) {
                QCOMPARE(v.s, 0.0f);
                continue;
            }
            if (std::isfinite(v.radiusLimit())) {
                QVERIFY(v.directionLength() > AxisVertex::kRadiusTag);
                QVERIFY(v.radiusLimit() >= AxisVertex::kMinRadius - 1e-6);
            } else {
                QVERIFY2(std::abs(v.directionLength() - 1.0) < 1e-5,
                         qPrintable(QStringLiteral("|n| = %1 у вершины %2")
                                        .arg(v.directionLength())
                                        .arg(i)));
            }
        }
    }

    // D7, «Стыки точные»: у внутренней вершины d = ±(hw + e)·cos(θ/2) - её
    // настоящее расстояние до прямых обоих сегментов при любом выносе.
    void innerJointVertexCarriesExactDistance()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(4.0);
        // Вправо по y = 0, затем вниз по x = 20: поворот на +90°.
        const QList<InkPoint> points = {{QPointF(0, 0), 1.0, 0.0},
                                        {QPointF(20, 0), 1.0, 0.01},
                                        {QPointF(20, 20), 1.0, 0.02}};
        const QVector<Vertex> strip = tessellator.tessellate(points);
        const qreal cosHalf = std::cos(M_PI / 4.0);

        int inner = 0;
        for (const Vertex &v : strip) {
            if (v.isCenter() || std::abs(std::abs(v.s) - 1.0f) < 1e-6f)
                continue;
            ++inner;
            QVERIFY(nearPoint(v, QPointF(20, 0)));
            QVERIFY(std::abs(std::abs(v.s) - cosHalf) < 1e-5);
            for (const qreal extension : {2.0, 3.0, 5.0}) {
                const QPointF p = v.at(extension);
                const qreal d = std::abs(v.s) * v.reach(extension);
                QVERIFY(std::abs(std::abs(p.y()) - d) < 1e-4);        // до прямой y = 0
                QVERIFY(std::abs(std::abs(p.x() - 20.0) - d) < 1e-4); // до прямой x = 20
            }
        }
        QVERIFY2(inner >= 1, "у стыка нет внутренней вершины");
    }

    // D7: внешняя дуга стыка - веер вокруг центра стыка (d = 0), а не из
    // внутренней точки, как было.
    void roundJoinFansAroundJointCenter()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(4.0);
        const QList<InkPoint> points = {{QPointF(0, 0), 1.0, 0.0},
                                        {QPointF(20, 0), 1.0, 0.01},
                                        {QPointF(20, 20), 1.0, 0.02}};
        const QVector<Vertex> strip = tessellator.tessellate(points);

        int centers = 0;
        int arc = 0;
        for (const Vertex &v : strip) {
            if (!nearPoint(v, QPointF(20, 0)))
                continue;
            if (v.isCenter()) {
                ++centers;
                QCOMPARE(v.s, 0.0f);
            } else if (std::abs(std::abs(v.s) - 1.0f) < 1e-6f) {
                ++arc;
                // Снаружи поворота: от нормали входящего (0, -1) к нормали
                // исходящего (1, 0).
                const QPointF n = v.direction();
                QVERIFY(n.x() >= -1e-6 && n.y() <= 1e-6);
                QVERIFY(std::abs(v.directionLength() - 1.0) < 1e-5);
            }
        }
        QVERIFY2(centers >= 5, qPrintable(QStringLiteral("центров веера %1").arg(centers)));
        QVERIFY2(arc >= 6, qPrintable(QStringLiteral("вершин дуги %1").arg(arc)));
    }

    // D7, «Торцы»: полувеер с центральной вершиной; дуга - позади начала и
    // впереди конца штриха, полукругом от одной стороны до другой.
    void capsAreHalfFansAroundEndPoints()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(4.0);
        tessellator.setMinWidthFactor(0.0);
        QList<InkPoint> points;
        for (int i = 0; i < 10; ++i)
            points.append({QPointF(10 + i * 5.0, 50.0), 1.0, i / 200.0});
        const QVector<Vertex> strip = tessellator.tessellate(points);

        const QPointF first(10, 50);
        const QPointF last(55, 50);
        int startCenters = 0;
        int endCenters = 0;
        bool startBack = false;
        bool endFront = false;
        for (const Vertex &v : strip) {
            if (nearPoint(v, first)) {
                QVERIFY(std::abs(v.hw - 2.0f) < 1e-5f);
                if (v.isCenter()) {
                    ++startCenters;
                    continue;
                }
                QVERIFY2(v.direction().x() <= 1e-6, "дуга начального торца впереди начала");
                startBack = startBack || v.direction().x() < -0.999;
            } else if (nearPoint(v, last)) {
                QVERIFY(std::abs(v.hw - 2.0f) < 1e-5f);
                if (v.isCenter()) {
                    ++endCenters;
                    continue;
                }
                QVERIFY2(v.direction().x() >= -1e-6, "дуга конечного торца позади конца");
                endFront = endFront || v.direction().x() > 0.999;
            }
        }
        QCOMPARE(startCenters, StrokeTessellator::kCapFanSteps + 1);
        QCOMPARE(endCenters, StrokeTessellator::kCapFanSteps + 1);
        QVERIFY2(startBack && endFront, "дуга торца не доходит до оси");
    }

    // D7, «Внутренний вынос ограничен радиусом кривизны»: на петле радиусом
    // меньше полуширины внутренние вершины встают в центр петли и дальше не
    // идут при любом выносе - соседние стыки не перехлёстываются.
    void innerReachIsLimitedByCurvatureRadius()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(6.0); // полуширина 3 при полном нажиме
        tessellator.setMinWidthFactor(0.0);
        const qreal radius = 2.0;
        const QPointF loopCenter(50, 50);
        QList<InkPoint> points;
        const QList<QPointF> path = reference::circle(radius, 1, loopCenter, 60);
        for (int i = 0; i < path.size(); ++i)
            points.append({path[i], 1.0, i / 200.0});
        const QVector<Vertex> strip = tessellator.tessellate(points);

        int limited = 0;
        for (const Vertex &v : strip) {
            if (!std::isfinite(v.radiusLimit()))
                continue;
            ++limited;
            QVERIFY2(std::abs(v.radiusLimit() - radius) < 0.1 * radius,
                     qPrintable(QStringLiteral("ρ = %1 при радиусе петли %2")
                                    .arg(v.radiusLimit())
                                    .arg(radius)));
            const QPointF far = v.at(100.0);
            QVERIFY(std::hypot(far.x() - v.cx, far.y() - v.cy) <= v.radiusLimit() + 1e-4);
            QVERIFY2(std::hypot(far.x() - loopCenter.x(), far.y() - loopCenter.y()) < 0.1 * radius,
                     "внутренняя вершина ушла за центр петли");
        }
        QVERIFY2(limited > 10, qPrintable(QStringLiteral("с пределом %1 вершин").arg(limited)));
    }

    // Пологий стык - пара на биссектрисе с s = ±cos(θ/2).
    void gentleJointPairCarriesHalfAngleCosine()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(6.0);
        const QList<InkPoint> points = {{QPointF(0, 0), 1.0, 0.0},
                                        {QPointF(10, 0), 1.0, 0.01},
                                        {QPointF(20, 1), 1.0, 0.02}};
        const QVector<Vertex> strip = tessellator.tessellate(points);
        const int startCap = 2 * (StrokeTessellator::kCapFanSteps + 1) + 1;
        QCOMPARE(strip.size(), startCap + 2 + 2 + 2 * (StrokeTessellator::kCapFanSteps + 1));

        const qreal cosHalf = std::cos(std::atan2(1.0, 10.0) / 2.0);
        const Vertex &left = strip[startCap];
        const Vertex &right = strip[startCap + 1];
        QVERIFY(nearPoint(left, QPointF(10, 0)) && nearPoint(right, QPointF(10, 0)));
        QVERIFY(std::abs(left.s - cosHalf) < 1e-5);
        QVERIFY(std::abs(right.s + cosHalf) < 1e-5);
        const QPointF l = left.direction();
        const QPointF r = right.direction();
        QVERIFY(std::abs(l.x() + r.x()) < 1e-5 && std::abs(l.y() + r.y()) < 1e-5);
    }
```

Тест шага 1 `styleSelectsThicknessLaw` (шаг 1, 4.1) проверяет полосу длиной пар и
`strip.size() == points.size() * 2` - заменить его целиком:

```cpp
    // ink-beauty-pass, D3: закон толщины (тест шага 1; полоса проверяется по hw
    // вершин, а не по длине пар).
    void styleSelectsThicknessLaw()
    {
        const auto widthAt = [](const StrokeTessellator &tessellator, qreal pressure) {
            return 2.0 * tessellator.halfWidthAt({QPointF(0, 0), pressure, 0.0});
        };
        const auto near = [](qreal a, qreal b) { return qAbs(a - b) < 1e-9; };

        StrokeTessellator tessellator;
        tessellator.setWidth(4.0);
        QVERIFY(near(widthAt(tessellator, 0.0), 4.0 * 0.25));
        tessellator.setStyle(StrokeStyle::Tapered);
        QVERIFY(near(widthAt(tessellator, 0.0), 4.0 * 0.08));
        QVERIFY(near(widthAt(tessellator, -1.0), 4.0 * 0.7375));

        // Полоса идёт по тому же закону: прямая с нулевым нажимом.
        QList<InkPoint> points;
        for (int i = 0; i < 10; ++i)
            points.append({QPointF(i * 5.0, 50.0), 0.0, i / 200.0});
        const QVector<Vertex> strip = tessellator.tessellate(points);
        QVERIFY(!StrokeTessellator::isDegenerate(strip));
        for (const Vertex &v : strip)
            QVERIFY2(qAbs(v.hw - 4.0f * 0.08f / 2.0f) < 1e-5f,
                     qPrintable(QStringLiteral("hw %1").arg(v.hw)));
    }
```

- [x] 11.2 **Прогон - ожидаемый провал сборки**

```
cmake --build build -j$(nproc) --target tst_stroketessellator
```

Ожидаемо: ошибка компиляции теста - у `StrokeTessellator` нет `kStartCapVertices`,
`kCapFanSteps`, у вершины нет `at`, `isCenter`, `cx`; нет `AxisVertex`.

- [x] 11.3 **`src/ink/stroketessellator.h` - заменить целиком**

Заголовок после шага 2 (`kCapPairs`, `emitDot`) заменяется полностью. `setStyle`,
`capDirection` и публичный `isDot` - объявления шагов 1-2 без изменений; `kCapPairs` и
`emitDot` уходят (капсулу касания строит `axisOf`), `kCapLookback` и `isDot` остаются -
ими пользуются `WetStroke::startCapIsSettled`, `endCapWindow` и `withDotTwin`.

```cpp
#pragma once

#include <QList>
#include <QPointF>
#include <QPolygonF>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>

#include "ink/inktypes.h"

// Вершина оболочки штриха с осью (design.md ink-beauty-pass, D7). Полоса не
// хранит готовых позиций: вершинный шейдер выносит вершину на 1 device px за
// кромку - pos = c + n̂·вынос - и отдаёт d = s·вынос, знаковое расстояние
// поперёк штриха. Поэтому зум не пересобирает геометрию, а кромка
// сглаживается точной долей пикселя.
//
// Вершины трёх видов:
// - центр веера (стыка или торца): n = 0, s = 0;
// - вершина кромки: |n| = 1; s = ±1 на нормали и дуге, ±cos(θ/2) у пары на
//   биссектрисе пологого стыка;
// - внутренняя вершина стыка: |n| = 1 + ρ, где ρ - радиус кривизны осевой в
//   точках страницы. Вынос такой вершины не больше ρ: на тугой петле
//   внутренние вершины соседних стыков иначе заходят друг за друга, и бахрома
//   складывается. Радиус хранится длиной направления, чтобы вершина осталась
//   в 24 байтах.
//
// Тот же разбор вершины - в src/canvas/shaders/ink.vert и src/ink/inkcoverage.h.
struct AxisVertex
{
    // |n| больше этого - у вершины есть предел выноса; совпадает с kRadiusTag
    // в ink.vert.
    static constexpr float kRadiusTag = 1.001f;
    // Границы хранимого радиуса, точки страницы: снизу - чтобы метка не
    // слилась с единичной нормалью, сверху - чтобы 1 + ρ не потеряло точность.
    static constexpr qreal kMinRadius = 0.002;
    static constexpr qreal kMaxRadius = 1.0e4;

    float cx = 0.0f; // центр стыка или торца, точки страницы
    float cy = 0.0f;
    float nx = 0.0f; // направление выноса
    float ny = 0.0f;
    float hw = 0.0f; // полуширина, точки страницы
    float s = 0.0f;  // d = s·вынос

    QPointF center() const { return QPointF(cx, cy); }
    bool isCenter() const { return nx == 0.0f && ny == 0.0f; }
    qreal directionLength() const { return std::hypot(qreal(nx), qreal(ny)); }
    QPointF direction() const
    {
        const qreal length = directionLength();
        return length > 0.0 ? QPointF(nx, ny) / length : QPointF();
    }
    // Предел выноса, точки страницы; бесконечность - предела нет.
    qreal radiusLimit() const
    {
        const qreal length = directionLength();
        return length > qreal(kRadiusTag) ? length - 1.0
                                          : std::numeric_limits<qreal>::infinity();
    }
    // Вынос с учётом предела, точки страницы.
    qreal reach(qreal extension) const { return std::min(extension, radiusLimit()); }
    // Положение вершины при выносе extension: at(hw) - кромка без бахромы.
    QPointF at(qreal extension) const { return center() + direction() * reach(extension); }
};
static_assert(sizeof(AxisVertex) == 6 * sizeof(float), "VBO и шейдер ждут 24 байта на вершину");

// Тесселяция штриха переменной ширины в треугольную полосу.
//
// Полоса, а не путь: чернила должны жить в графе сцены как обычная геометрия,
// иначе на каждом кадре пришлось бы растеризовать путь на CPU - ровно та
// проблема, из-за которой отвергнут cairo-подход (design.md, D4).
//
// Полоса несёт ось штриха (AxisVertex), а не готовые позиции: кромку выносит и
// сглаживает материал чернил (design.md ink-beauty-pass, D7). Веера вокруг
// центров стыков и торцов лежат в той же полосе повтором центральной вершины.
class StrokeTessellator
{
public:
    using Vertex = AxisVertex;

    // Толщина при полном нажиме, в точках страницы.
    void setWidth(qreal width);
    qreal width() const { return m_width; }

    // Доля толщины, остающаяся при нулевом нажиме: линия не должна
    // схлопываться в ничто на слабом касании. Обычно задаётся через setStyle.
    void setMinWidthFactor(qreal factor);

    // Закон толщины штриха: доля при нулевом нажиме 0.25 у Legacy и 0.08 у
    // Tapered (ink-beauty-pass, design.md, D3). Задаётся перед каждым штрихом -
    // и мокрым, и сухим, и для экспорта: один тесселятор рисует штрихи разных
    // законов.
    void setStyle(StrokeStyle style);

    // Давление, которым рисуется штрих, если устройство давления не сообщает.
    // Такой штрих идёт по закону Legacy при любом стиле: толщина пера без
    // давления задаётся настройками и от заострения не меняется (D3).
    void setConstantPressure(qreal pressure);

    // Полная тесселяция траектории в треугольную полосу.
    QVector<Vertex> tessellate(const QList<InkPoint> &points) const;

    // Поэлементная выдача - на ней стоит инкрементальное наращивание «мокрого»
    // штриха: одна и та же математика стыка используется и при полной
    // тесселяции, и при дописывании хвоста.
    enum class CapEnd { Start, End };
    // Шаг полувеера торца: π / 0.28 с округлением вверх - тот же шаг, что у
    // дуги стыка. Вершин в начальном торце 2·(K + 1) + 1, в конечном
    // 2 + 2·(K + 1).
    static constexpr int kCapFanSteps = 12;
    static constexpr int kStartCapVertices = 2 * (kCapFanSteps + 1) + 1;
    static constexpr int kEndCapVertices = 2 + 2 * (kCapFanSteps + 1);
    // Дальше стольких точек от конца хорда для направления торца не ищется:
    // столько же точек мокрый штрих держит под рукой, чтобы его торец совпал с
    // сухим, а работа на отсчёт не росла с длиной штриха.
    static constexpr int kCapLookback = 48;
    // Торец - полувеер вокруг точки (design.md, D4, D7): центр (d = 0)
    // чередуется с вершинами дуги радиуса полуширины; осевая кончается в точке,
    // а торец выступает за неё на полуширину. direction - направление движения
    // пера у этого конца.
    void emitCap(QVector<Vertex> &strip, const QPointF &position, qreal halfWidth,
                 const QPointF &direction, CapEnd end) const;
    // Направление торца - хорда до первой точки на расстоянии не меньше radius
    // от конца, но не дальше kCapLookback точек, а не последний отрезок: на
    // отрыве дотяжка до точки контакта дописывает отрезок короче шага со
    // случайным направлением. Вырожденный штрих - (1, 0).
    static QPointF capDirection(const QList<InkPoint> &points, CapEnd end, qreal radius);
    // Точка (design.md, D5): осевая короче полной толщины пера. Такой штрих
    // tessellate() рисует капсулой, а WetStroke дописывает к нему точку с
    // наибольшим нажимом касания - решение у обоих одно.
    bool isDot(const QList<InkPoint> &points) const;
    // Стык: пара на биссектрисе на пологом повороте; на резком - внутренняя
    // вершина с точным d = ±(hw + e)·cos(θ/2) и веер внешней дуги вокруг центра
    // стыка. radius - предел выноса внутренней вершины (curvatureRadius).
    void emitJoint(QVector<Vertex> &strip, const QPointF &position, qreal halfWidth,
                   const QPointF &incoming, const QPointF &outgoing,
                   qreal radius = std::numeric_limits<qreal>::infinity()) const;
    // Радиус кривизны осевой в стыке: max(L-, L+) / (2·sin(|θ|/2)). На
    // равномерной нарезке - радиус окружности через три точки. Направления
    // передаются отдельно: у вырожденного сегмента оно унаследовано.
    static qreal curvatureRadius(const QPointF &previous, const QPointF &position,
                                 const QPointF &next, const QPointF &incoming,
                                 const QPointF &outgoing);

    // Замкнутый контур штриха для экспорта: дуга начального торца, левая
    // сторона, дуга конечного, правая сторона назад. Точки - там, где у полосы
    // на экране проходит кромка |d| = hw, из тех же примитивов: стороны, дуги
    // торцов, внешние дуги стыков (design.md, D7, «Экспорт»). Заливать по
    // ненулевому обходу: на петлях контур перекрывает сам себя.
    QPolygonF outline(const QList<InkPoint> &points) const;

    // Полуширина штриха в конкретной точке - по ней тесты проверяют, что
    // толщина следует за давлением.
    qreal halfWidthAt(const InkPoint &point) const;

    static bool isDegenerate(const QVector<Vertex> &strip);

private:
    // Осевая в том виде, в каком её обходят и полоса, и контур. Касание (D5)
    // уже свёрнуто в капсулу из двух точек.
    struct Axis
    {
        QList<QPointF> positions;
        QList<qreal> halfWidths;
        QList<QPointF> directions; // направления сегментов, на один меньше точек
        QPointF startDirection;    // направление движения пера у торцов
        QPointF endDirection;
    };
    Axis axisOf(const QList<InkPoint> &points) const;

    qreal m_width = 2.4;
    qreal m_minWidthFactor = 0.25;
    qreal m_constantPressure = 0.65;
};
```

- [x] 11.4 **`src/ink/stroketessellator.cpp` - заменить целиком**

`setStyle`, `halfWidthAt` (шаг 1), `capDirection` и `isDot` (шаг 2) перенесены
дословно; `emitCap`, `emitJoint`, `tessellate`, `outline`, `isDegenerate` новые;
`toVertex` и `emitDot` удалены. `axisOf` решает «точка или нет» тем же `isDot`, что и
шаг 2: длина копится в том же порядке, что у мокрого штриха.

```cpp
#include "stroketessellator.h"

#include <QtGlobal>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

// Порог угла, за которым стык скругляется дугой, а не бисcектрисой.
constexpr qreal kRoundJoinThreshold = 0.35; // радиан, ~20 градусов
// Шаг дуги скругления.
constexpr qreal kRoundJoinStep = 0.28; // радиан, ~16 градусов
// Ниже этой длины биссектриса считается вырожденной (разворот почти на 180).
constexpr qreal kBisectorEpsilon = 0.2;

constexpr qreal kLegacyMinWidthFactor = 0.25;
constexpr qreal kTaperedMinWidthFactor = 0.08;

QPointF normalOf(const QPointF &direction)
{
    return QPointF(-direction.y(), direction.x());
}

QPointF normalized(const QPointF &v)
{
    const qreal length = std::hypot(v.x(), v.y());
    if (length < 1e-9)
        return QPointF();
    return v / length;
}

qreal cross(const QPointF &a, const QPointF &b)
{
    return a.x() * b.y() - a.y() * b.x();
}

constexpr qreal kNoLimit = std::numeric_limits<qreal>::infinity();

// Вершина кромки или внутренняя вершина стыка. Предел выноса кладётся в длину
// направления: |n| = 1 + ρ (AxisVertex).
AxisVertex axisVertex(const QPointF &center, const QPointF &direction, qreal halfWidth,
                      qreal side, qreal radius = kNoLimit)
{
    QPointF n = direction;
    if (std::isfinite(radius) && !n.isNull())
        n *= 1.0 + std::clamp(radius, AxisVertex::kMinRadius, AxisVertex::kMaxRadius);
    return {float(center.x()), float(center.y()), float(n.x()), float(n.y()), float(halfWidth),
            float(side)};
}

// Центр веера: d = 0, вынос нулевой.
AxisVertex axisCenter(const QPointF &center, qreal halfWidth)
{
    return axisVertex(center, QPointF(), halfWidth, 0.0);
}

QPointF rotated(const QPointF &v, qreal angle)
{
    const qreal c = std::cos(angle);
    const qreal s = std::sin(angle);
    return QPointF(v.x() * c - v.y() * s, v.x() * s + v.y() * c);
}

qreal turnAngle(const QPointF &incoming, const QPointF &outgoing)
{
    return std::atan2(cross(incoming, outgoing), QPointF::dotProduct(incoming, outgoing));
}

} // namespace

void StrokeTessellator::setWidth(qreal width)
{
    m_width = qMax(width, 0.05);
}

void StrokeTessellator::setMinWidthFactor(qreal factor)
{
    m_minWidthFactor = qBound(0.0, factor, 1.0);
}

void StrokeTessellator::setConstantPressure(qreal pressure)
{
    m_constantPressure = qBound(0.0, pressure, 1.0);
}

void StrokeTessellator::setStyle(StrokeStyle style)
{
    setMinWidthFactor(style == StrokeStyle::Tapered ? kTaperedMinWidthFactor
                                                    : kLegacyMinWidthFactor);
}

qreal StrokeTessellator::halfWidthAt(const InkPoint &point) const
{
    if (!point.hasPressure()) {
        const qreal factor =
            kLegacyMinWidthFactor + (1.0 - kLegacyMinWidthFactor) * m_constantPressure;
        return m_width * factor * 0.5;
    }
    const qreal pressure = qBound(0.0, point.pressure, 1.0);
    const qreal factor = m_minWidthFactor + (1.0 - m_minWidthFactor) * pressure;
    return m_width * factor * 0.5;
}


void StrokeTessellator::emitCap(QVector<Vertex> &strip, const QPointF &position, qreal halfWidth,
                                const QPointF &direction, CapEnd end) const
{
    // Полувеер вокруг точки (design.md, D7): у плоской пары нет бахромы вдоль
    // штриха, кромка торца осталась бы ступенчатой. Пары шага 2 (D4)
    // заменены, круглый торец остался.
    const QPointF normal = normalOf(direction);
    const Vertex center = axisCenter(position, halfWidth);
    const auto arc = [&](int step, qreal sign) {
        const qreal phi = M_PI * step / kCapFanSteps;
        const QPointF v = normal * (sign * std::cos(phi)) + direction * (sign * std::sin(phi));
        return axisVertex(position, normalized(v), halfWidth, 1.0);
    };

    if (end == CapEnd::Start) {
        // От правой стороны назад, за начало штриха, к левой:
        // v(φ) = −n·cosφ − t·sinφ.
        for (int step = 0; step <= kCapFanSteps; ++step) {
            strip.append(center);
            strip.append(arc(step, -1.0));
        }
        // Последняя вершина дуги - левая вершина первой пары; правая - следом.
        // Треугольник «центр, левая, правая» вырожден по площади.
        strip.append(axisVertex(position, -normal, halfWidth, -1.0));
        return;
    }

    strip.append(axisVertex(position, normal, halfWidth, 1.0));
    strip.append(axisVertex(position, -normal, halfWidth, -1.0));
    // От левой стороны вперёд, за конец штриха, к правой: v(φ) = n·cosφ + t·sinφ.
    for (int step = 0; step <= kCapFanSteps; ++step) {
        strip.append(center);
        strip.append(arc(step, 1.0));
    }
}

QPointF StrokeTessellator::capDirection(const QList<InkPoint> &points, CapEnd end, qreal radius)
{
    const int count = int(points.size());
    if (count < 2)
        return QPointF(1.0, 0.0);
    // k-я точка от того конца, у которого строится торец.
    const auto from = [&points, count, end](int k) {
        return end == CapEnd::Start ? points[k].position : points[count - 1 - k].position;
    };
    const QPointF tip = from(0);
    const int reach = qMin(count, kCapLookback);
    QPointF far = tip;
    for (int k = 1; k < reach; ++k) {
        far = from(k);
        if (std::hypot(far.x() - tip.x(), far.y() - tip.y()) >= radius)
            break;
    }
    // Направление движения пера: в начале - от касания вперёд, в конце - к отрыву.
    const QPointF direction = normalized(end == CapEnd::Start ? far - tip : tip - far);
    return direction.isNull() ? QPointF(1.0, 0.0) : direction;
}

bool StrokeTessellator::isDot(const QList<InkPoint> &points) const
{
    // Длина осевой копится в том же порядке, что и у мокрого штриха
    // (WetStroke::rebuildTail): решение «точка или нет» обязано совпасть.
    qreal arc = 0.0;
    for (int i = 1; i < points.size(); ++i) {
        const QPointF delta = points[i].position - points[i - 1].position;
        arc += std::hypot(delta.x(), delta.y());
        if (arc >= m_width)
            return false;
    }
    return true;
}

void StrokeTessellator::emitJoint(QVector<Vertex> &strip, const QPointF &position, qreal halfWidth,
                                  const QPointF &incoming, const QPointF &outgoing,
                                  qreal radius) const
{
    const qreal turn = turnAngle(incoming, outgoing);
    // Внутренняя сторона - та, куда поворачивает штрих: при turn > 0 это
    // сторона нормали normalOf(direction).
    const bool turnsLeft = turn > 0.0;
    const qreal cosHalf = std::cos(turn / 2.0);

    if (std::abs(turn) < kRoundJoinThreshold) {
        // Пологий стык - пара на биссектрисе. Вершина на биссектрисе стоит на
        // расстоянии вынос·cos(θ/2) от прямых обоих сегментов, отсюда s.
        QPointF bisector = normalized(normalOf(incoming) + normalOf(outgoing));
        if (bisector.isNull())
            bisector = normalOf(outgoing);
        strip.append(axisVertex(position, bisector, halfWidth, cosHalf,
                                turnsLeft ? radius : kNoLimit));
        strip.append(axisVertex(position, -bisector, halfWidth, -cosHalf,
                                turnsLeft ? kNoLimit : radius));
        return;
    }

    // Резкий стык (design.md, D7, «Стыки точные»). Треугольник
    // (внутренняя, O_0, центр) точен относительно прямой входящего сегмента,
    // веер (O_k, центр, O_k+1) вокруг центра стыка - радиально,
    // (центр, O_K, внутренняя) - относительно исходящего. Веер из внутренней
    // точки, как было, на «M» давал p99 ошибки 108/255 против 54.
    const QPointF startNormal = normalOf(incoming) * (turnsLeft ? -1.0 : 1.0);
    const QPointF endNormal = normalOf(outgoing) * (turnsLeft ? -1.0 : 1.0);
    const QPointF normalSum = normalOf(incoming) + normalOf(outgoing);
    const bool innerDegenerate = std::hypot(normalSum.x(), normalSum.y()) < kBisectorEpsilon;
    const QPointF innerDirection = normalized(normalSum) * (turnsLeft ? 1.0 : -1.0);
    const qreal innerSide = turnsLeft ? 1.0 : -1.0;

    // На развороте почти на 180° биссектрисы нет: внутренняя вершина -
    // центр стыка.
    const Vertex inner = innerDegenerate
        ? axisCenter(position, halfWidth)
        : axisVertex(position, innerDirection, halfWidth, innerSide * cosHalf, radius);
    const Vertex center = axisCenter(position, halfWidth);

    // Дуга идёт от нормали входящего сегмента к нормали исходящего по
    // короткой стороне: направление - из знака векторного произведения самих
    // нормалей, а не из знака поворота.
    const qreal arcAngle = turnAngle(startNormal, endNormal);
    const int steps = qMax(1, int(std::ceil(std::abs(arcAngle) / kRoundJoinStep)));
    const auto outer = [&](int step) {
        return axisVertex(position, rotated(startNormal, arcAngle * (qreal(step) / steps)),
                          halfWidth, -innerSide);
    };

    // Порядок в полосе фиксирован: на границах стыка сначала «левая» сторона,
    // потом «правая», иначе сегмент между стыками вывернется.
    if (turnsLeft) {
        strip.append(inner);
        strip.append(outer(0));
        for (int step = 1; step <= steps; ++step) {
            strip.append(center);
            strip.append(outer(step));
        }
        strip.append(inner);
        strip.append(outer(steps));
    } else {
        strip.append(outer(0));
        strip.append(inner);
        strip.append(outer(0));
        for (int step = 1; step <= steps; ++step) {
            strip.append(center);
            strip.append(outer(step));
        }
        strip.append(inner);
    }
}

qreal StrokeTessellator::curvatureRadius(const QPointF &previous, const QPointF &position,
                                         const QPointF &next, const QPointF &incoming,
                                         const QPointF &outgoing)
{
    // max, а не min: у конца штриха бывает отрезок короче шага нарезки, и min
    // схлопнул бы внутреннюю вершину в центр стыка - на кромке осталась бы
    // выемка (design.md, D7).
    const qreal halfSine = std::sin(std::abs(turnAngle(incoming, outgoing)) / 2.0);
    if (halfSine < 1e-6)
        return kNoLimit;
    const QPointF before = position - previous;
    const QPointF after = next - position;
    const qreal span = std::max(std::hypot(before.x(), before.y()), std::hypot(after.x(), after.y()));
    return span / (2.0 * halfSine);
}

StrokeTessellator::Axis StrokeTessellator::axisOf(const QList<InkPoint> &points) const
{
    Axis axis;
    if (points.size() < 2)
        return axis;

    if (isDot(points)) {
        // Касание (D5): капсула от первой до последней точки с наибольшей
        // полушириной; при совпадающих концах - диск из двух полувееров.
        qreal halfWidth = 0.0;
        for (const InkPoint &point : points)
            halfWidth = std::max(halfWidth, halfWidthAt(point));
        QPointF direction = normalized(points.last().position - points.first().position);
        if (direction.isNull())
            direction = QPointF(1.0, 0.0);
        axis.positions = {points.first().position, points.last().position};
        axis.halfWidths = {halfWidth, halfWidth};
        axis.directions = {direction};
        axis.startDirection = direction;
        axis.endDirection = direction;
        return axis;
    }

    axis.positions.reserve(points.size());
    axis.halfWidths.reserve(points.size());
    axis.directions.reserve(points.size() - 1);
    // Вырожденный сегмент (две точки в одном месте) наследует направление
    // предыдущего, иначе нормаль была бы неопределена.
    QPointF previous(1.0, 0.0);
    for (int i = 0; i < points.size(); ++i) {
        axis.positions.append(points[i].position);
        axis.halfWidths.append(halfWidthAt(points[i]));
        if (i + 1 < points.size()) {
            QPointF direction = normalized(points[i + 1].position - points[i].position);
            if (direction.isNull())
                direction = previous;
            axis.directions.append(direction);
            previous = direction;
        }
    }
    // Направление торца - хорда, а не последний сегмент (D4).
    axis.startDirection = capDirection(points, CapEnd::Start, axis.halfWidths.first());
    axis.endDirection = capDirection(points, CapEnd::End, axis.halfWidths.last());
    return axis;
}

QVector<StrokeTessellator::Vertex> StrokeTessellator::tessellate(const QList<InkPoint> &points) const
{
    QVector<Vertex> strip;
    const Axis axis = axisOf(points);
    if (axis.positions.size() < 2)
        return strip;

    strip.reserve(axis.positions.size() * 4 + 4 * (kCapFanSteps + 2));
    emitCap(strip, axis.positions.first(), axis.halfWidths.first(), axis.startDirection,
            CapEnd::Start);
    for (int i = 1; i + 1 < axis.positions.size(); ++i) {
        emitJoint(strip, axis.positions[i], axis.halfWidths[i], axis.directions[i - 1],
                  axis.directions[i],
                  curvatureRadius(axis.positions[i - 1], axis.positions[i], axis.positions[i + 1],
                                  axis.directions[i - 1], axis.directions[i]));
    }
    emitCap(strip, axis.positions.last(), axis.halfWidths.last(), axis.endDirection,
            CapEnd::End);
    return strip;
}

QPolygonF StrokeTessellator::outline(const QList<InkPoint> &points) const
{
    QPolygonF polygon;
    const Axis axis = axisOf(points);
    if (axis.positions.size() < 2)
        return polygon;

    // Точки контура лежат там, где у полосы на экране проходит кромка |d| = hw:
    // - вершина дуги (торец, внешняя сторона резкого стыка) - на полуширине;
    // - вершина на биссектрисе - на пересечении кромок соседних сегментов,
    //   hw / cos(θ/2), внутренняя - не дальше радиуса кривизны, как в полосе.
    QPolygonF left;
    QPolygonF right;
    for (int i = 1; i + 1 < axis.positions.size(); ++i) {
        const QPointF &c = axis.positions[i];
        const qreal hw = axis.halfWidths[i];
        const QPointF &incoming = axis.directions[i - 1];
        const QPointF &outgoing = axis.directions[i];
        const qreal radius = curvatureRadius(axis.positions[i - 1], c, axis.positions[i + 1],
                                             incoming, outgoing);
        const qreal turn = turnAngle(incoming, outgoing);
        const bool turnsLeft = turn > 0.0;
        const qreal miter = hw / std::max(std::cos(turn / 2.0), 1e-6);

        if (std::abs(turn) < kRoundJoinThreshold) {
            QPointF bisector = normalized(normalOf(incoming) + normalOf(outgoing));
            if (bisector.isNull())
                bisector = normalOf(outgoing);
            left.append(c + bisector * (turnsLeft ? std::min(miter, radius) : miter));
            right.append(c - bisector * (turnsLeft ? miter : std::min(miter, radius)));
            continue;
        }

        const QPointF normalSum = normalOf(incoming) + normalOf(outgoing);
        const bool innerDegenerate = std::hypot(normalSum.x(), normalSum.y()) < kBisectorEpsilon;
        const QPointF innerDirection = normalized(normalSum) * (turnsLeft ? 1.0 : -1.0);
        const QPointF inner = innerDegenerate ? c : c + innerDirection * std::min(miter, radius);
        const QPointF startNormal = normalOf(incoming) * (turnsLeft ? -1.0 : 1.0);
        const QPointF endNormal = normalOf(outgoing) * (turnsLeft ? -1.0 : 1.0);
        const qreal arcAngle = turnAngle(startNormal, endNormal);
        const int steps = qMax(1, int(std::ceil(std::abs(arcAngle) / kRoundJoinStep)));

        (turnsLeft ? left : right).append(inner);
        QPolygonF &outerSide = turnsLeft ? right : left;
        for (int step = 0; step <= steps; ++step)
            outerSide.append(c + rotated(startNormal, arcAngle * (qreal(step) / steps)) * hw);
    }

    // Дуга торца - те же углы, что у полувеера полосы.
    const auto capArc = [&polygon](const QPointF &c, qreal hw, const QPointF &direction,
                                   qreal sign) {
        const QPointF normal = normalOf(direction);
        for (int step = 0; step <= kCapFanSteps; ++step) {
            const qreal phi = M_PI * step / kCapFanSteps;
            const QPointF v = normal * (sign * std::cos(phi)) + direction * (sign * std::sin(phi));
            polygon.append(c + normalized(v) * hw);
        }
    };

    polygon.reserve(left.size() + right.size() + 2 * (kCapFanSteps + 1) + 1);
    // От правой стороны начала назад к левой, левая сторона вперёд, от левой
    // стороны конца вперёд к правой, правая сторона назад.
    capArc(axis.positions.first(), axis.halfWidths.first(), axis.startDirection, -1.0);
    polygon += left;
    capArc(axis.positions.last(), axis.halfWidths.last(), axis.endDirection, 1.0);
    for (int i = int(right.size()) - 1; i >= 0; --i)
        polygon.append(right[i]);
    polygon.append(polygon.first());
    return polygon;
}

bool StrokeTessellator::isDegenerate(const QVector<Vertex> &strip)
{
    if (strip.size() < 4)
        return true;
    for (const Vertex &v : strip) {
        if (!std::isfinite(v.cx) || !std::isfinite(v.cy) || !std::isfinite(v.nx)
            || !std::isfinite(v.ny) || !std::isfinite(v.hw) || !std::isfinite(v.s))
            return true;
    }
    return false;
}
```

- [x] 11.5 **Остальные тесты шага 2 в `tst_stroketessellator` - под ось**

Правки по месту (файл - после шага 2):

1. `checkNoArtifacts`: удалить строку `QVERIFY2(strip.size() % 2 == 0, label);` - у
   полувеера нечётное число вершин, отсечения граней в Qt Quick нет.
2. `straightStrokeHasRequestedWidth`: `QCOMPARE(strip.size(), (points.size() - 2) * 2 + 2 * kCapVertices);`
   → `QCOMPARE(strip.size(), (points.size() - 2) * 2 + kStartCapVertices + kEndCapVertices);`
3. `sharpTurnProducesRoundJoinVertices`: две строки с `2 * kCapVertices + 2` заменить:
```cpp
        QCOMPARE(tessellator.tessellate(gentle).size(), kStartCapVertices + kEndCapVertices + 2);
        QVERIFY2(tessellator.tessellate(sharp).size() > kStartCapVertices + kEndCapVertices + 2,
                 "стык не скруглён");
```
4. `capsAreHalfDiscsAroundStrokeEnds`: строки
   `QVERIFY(strip.size() >= 2 * kCapVertices);`, `QCOMPARE(strip.size() % 2, 0);` и
   заголовок первого цикла заменить:
```cpp
        QVERIFY(strip.size() >= kStartCapVertices + kEndCapVertices);

        // Центры вееров стоят в самой точке, остальные вершины торца - на дуге.
        for (int i = 0; i < kStartCapVertices; ++i) {
            if (strip[i].isCenter())
                continue;
```
   (тело цикла `const qreal d = QLineF(toPoint(strip[i]), first).length(); ...` - как
   было). Второй цикл: `for (int i = int(strip.size()) - kCapVertices; ...)` →
   `for (int i = int(strip.size()) - kEndCapVertices; i < strip.size(); ++i) {` и первой
   строкой тела `if (strip[i].isCenter()) continue;`. Проверки кончиков:
```cpp
        QVERIFY(QLineF(capTip(strip, StrokeTessellator::CapEnd::Start), first - QPointF(r0, 0.0)).length() < 1e-4);
        QVERIFY(QLineF(capTip(strip, StrokeTessellator::CapEnd::End), last + QPointF(r1, 0.0)).length() < 1e-4);
```
5. `capDirectionFollowsChordNotLastSegment`: `const QPointF tip = toPoint(strip.constLast());`
   → `const QPointF tip = capTip(strip, StrokeTessellator::CapEnd::End);`
6. `outlineIsClosedContourWithRoundEnds`: `QVERIFY(outline.size() > 2 * kCapVertices);` →
   `QVERIFY(outline.size() > 2 * (StrokeTessellator::kCapFanSteps + 1));`
7. `tapInOnePlaceIsDisc`: строку `QCOMPARE(strip.size(), 2 * kCapVertices);` и цикл за ней
   заменить:
```cpp
        QCOMPARE(strip.size(), kStartCapVertices + kEndCapVertices);
        for (const Vertex &vertex : strip) {
            if (!vertex.isCenter())
                QVERIFY(qAbs(QLineF(toPoint(vertex), center).length() - r) < 1e-4);
        }
```
8. `strokeShorterThanPenWidthIsCapsule`: в цикле по вершинам первой строкой тела
   `if (vertex.isCenter()) continue;`; последнюю проверку - на
   `QVERIFY(QLineF(capTip(longer, StrokeTessellator::CapEnd::Start), QPointF(10.0 - r0, 10.0)).length() < 1e-4);`
9. `degenerateInputIsHandled`: две проверки `vertex.x`/`vertex.y` заменить:
```cpp
            QVERIFY(std::isfinite(vertex.cx) && std::isfinite(vertex.cy));
            QVERIFY(std::isfinite(vertex.nx) && std::isfinite(vertex.ny));
            QVERIFY(std::isfinite(vertex.hw) && std::isfinite(vertex.s));
```

`widthFollowsPressureSmoothly` и `missingPressureGivesConstantWidth` правок не требуют:
`sideWidths` находит те же поперечные пары.

- [x] 11.6 **Прогон `tst_stroketessellator`**

```
cmake --build build -j$(nproc) --target tst_stroketessellator
```

Ожидаемо: в полном проекте `uqnotes_core` ещё не собирается на `documentcanvas.cpp`
(`uploadStrip` читает `.x/.y`, мост - 11.9); `wetstroke.cpp` собирается - у `emitJoint`
радиус по умолчанию. Прогон теста - в 11.10.

- [x] 11.7 **`src/ink/wetstroke.cpp`: радиус кривизны в стыках мокрой полосы**

В `rebuildTail()` (после шага 2) цикл окончательных стыков
`for (int i = m_jointedPoints; i + 1 < points.size(); ++i) { m_tessellator.emitJoint(...); }`
заменить:

```cpp
    // Радиус кривизны - по тем же трём точкам, что в полной тесселяции.
    for (int i = m_jointedPoints; i + 1 < points.size(); ++i) {
        m_tessellator.emitJoint(m_strip, points[i].position, m_tessellator.halfWidthAt(points[i]),
                                m_directions[i - 1], m_directions[i],
                                StrokeTessellator::curvatureRadius(
                                    points[i - 1].position, points[i].position,
                                    points[i + 1].position, m_directions[i - 1],
                                    m_directions[i]));
    }
```

Цикл провизорных стыков `for (int j = 0; j + 1 < tail.size(); ++j) { ... }` заменить:

```cpp
    for (int j = 0; j + 1 < tail.size(); ++j) {
        const QPointF outgoing =
            normalizedDirection(tail[j].position, tail[j + 1].position, incoming);
        // Предыдущая точка стыка: в хвосте - его же точка, у первого стыка
        // хвоста - предпоследняя окончательная (здесь их не меньше двух:
        // иначе штрих ещё строится целиком как точка, см. выше).
        const QPointF previous =
            j > 0 ? tail[j - 1].position : points[points.size() - 2].position;
        m_tessellator.emitJoint(m_strip, tail[j].position, m_tessellator.halfWidthAt(tail[j]),
                                incoming, outgoing,
                                StrokeTessellator::curvatureRadius(previous, tail[j].position,
                                                                   tail[j + 1].position,
                                                                   incoming, outgoing));
        incoming = outgoing;
    }
```

Вызовы `emitCap`, `startCapIsSettled`, `endCapWindow` шага 2 не трогать.

- [x] 11.8 **`tests/tst_wetstroke.cpp`: вершины целиком, торцы-веера, тугая петля**

Файл после шага 2 (шаг 2, 6.4, 7.3). В анонимном пространстве имён блок от
`// Вершин в одном торце: K + 1 пар (design.md, D4).` до конца `endCapCenter` заменить:

```cpp
// Вершин в торцах (design.md, D7): полувеер с центром.
constexpr int kStartCapVertices = StrokeTessellator::kStartCapVertices;
constexpr int kEndCapVertices = StrokeTessellator::kEndCapVertices;

// Вершина полосы в покое - на полуширине, без выноса бахромы.
QPointF toPoint(const WetStroke::Vertex &v)
{
    return v.at(v.hw);
}

// Центр дуги конечного торца - центр любой его вершины. «Конец линии» -
// центр дуги, а не кончик торца.
QPointF endCapCenter(const QVector<WetStroke::Vertex> &strip)
{
    return strip.constLast().center();
}

// Кончик конечного торца - вершина дуги на оси штриха (φ = π/2).
QPointF endCapTip(const QVector<WetStroke::Vertex> &strip)
{
    const int half = StrokeTessellator::kCapFanSteps / 2;
    return toPoint(strip[int(strip.size()) - 1 - 2 * (StrokeTessellator::kCapFanSteps - half)]);
}

// Вершины совпадают по всем полям: центр, направление (с пределом), hw, s.
bool sameVertex(const WetStroke::Vertex &a, const WetStroke::Vertex &b, float tolerance)
{
    return std::abs(a.cx - b.cx) <= tolerance && std::abs(a.cy - b.cy) <= tolerance
        && std::abs(a.nx - b.nx) <= tolerance && std::abs(a.ny - b.ny) <= tolerance
        && std::abs(a.hw - b.hw) <= tolerance && std::abs(a.s - b.s) <= tolerance;
}
```

Правки по месту:

1. `sameStrip`: условие `if (qAbs(a[i].x - b[i].x) >= 1e-3 || qAbs(a[i].y - b[i].y) >= 1e-3)` →
   `if (!sameVertex(a[i], b[i], 1e-3f))`.
2. `settledGeometryIsNeverRewritten`: условие `qFuzzyCompare(snapshot[v].x ...) && qFuzzyCompare(... .y ...)`
   в `QVERIFY2` → `sameVertex(snapshot[v], wet.strip()[v], 0.0f)`.
3. `incrementalResultMatchesFullTessellation`: условие с `.x`/`.y` в `QVERIFY2` →
   `sameVertex(wet.strip()[i], full[i], 1e-3f)`.
4. `tapWithoutMovementGivesDot`: `QCOMPARE(wet.strip().size(), 2 * kCapVertices);` и цикл за
   ним заменить:
```cpp
        QCOMPARE(wet.strip().size(), kStartCapVertices + kEndCapVertices);
        const qreal r = wet.tessellator().halfWidthAt({at, 0.5, 0.0});
        for (const WetStroke::Vertex &vertex : wet.strip()) {
            if (!vertex.isCenter())
                QVERIFY(qAbs(QLineF(toPoint(vertex), at).length() - r) < 1e-4);
        }
```
   (строку `const qreal r = ...` старого текста при этом убрать - она входит в замену).
5. `tinyMovementGivesCapsule`: в цикле по вершинам первой строкой тела
   `if (vertex.isCenter()) continue;`.
6. `stripEndFollowsThePenOnFastArc`: `if (wet.strip().size() < 2 * kCapVertices)` →
   `if (wet.strip().size() < kStartCapVertices + kEndCapVertices)`.
7. `wetCapsAreRoundAtEveryStep`: `if (wet.settledVertexCount() < kCapVertices)` →
   `if (wet.settledVertexCount() < kStartCapVertices)`; два цикла проверки дуг заменить:
```cpp
            for (int v = 0; v < kStartCapVertices; ++v) {
                if (!strip[v].isCenter())
                    QVERIFY(qAbs(QLineF(toPoint(strip[v]), first.position).length() - r0) <= 0.05 * r0);
            }
```
   и
```cpp
            for (int v = int(strip.size()) - kEndCapVertices; v < strip.size(); ++v) {
                if (!strip[v].isCenter())
                    QVERIFY(qAbs(QLineF(toPoint(strip[v]), tip.position).length() - r1) <= 0.05 * r1);
            }
```
8. `hookedEndMatchesDryAndFollowsChord`: `const QPointF tip = toPoint(wet.strip().constLast());` →
   `const QPointF tip = endCapTip(wet.strip());`.
9. `shiftedTapHasPeakWidth` (шаг 2, 7.3): в цикле `for (const WetStroke::Vertex &vertex : wet.strip())`
   после `wet.finish()` первой строкой тела `if (vertex.isCenter()) continue;` - центр
   полувеера лежит на оси капсулы, а не на её кромке.
10. `provisionalTailDoesNotOvershootReversal`: проверку внутри цикла по `v` заменить:
```cpp
                const qreal x = toPoint(wet.strip()[v]).x();
                QVERIFY2(x <= reachX + width / 2.0 + 1.5,
                         qPrintable(QStringLiteral("хвост торчит до x=%1 при пути до %2, отсчёт %3")
                                        .arg(x)
                                        .arg(reachX)
                                        .arg(i)));
```

Тест шага 1 `wetStrokeUsesPrototypeStyle` (шаг 1, 4.7) меряет полосу парами - заменить
целиком:

```cpp
    // ink-beauty-pass, D3 (тест шага 1): мокрый штрих рисуется законом
    // толщины своего прототипа; полоса проверяется по hw вершин.
    void wetStrokeUsesPrototypeStyle()
    {
        Stroke proto = prototype(4.0);
        proto.style = StrokeStyle::Tapered;
        WetStroke wet;
        wet.begin(0, proto);
        for (int i = 0; i < 40; ++i)
            wet.addPoint({QPointF(i * 3.0, 20.0), 0.0, i / 200.0});
        const Stroke stroke = wet.finish();
        QCOMPARE(stroke.style, StrokeStyle::Tapered);

        const QVector<WetStroke::Vertex> &strip = wet.strip();
        QVERIFY(strip.size() >= 4);
        for (const WetStroke::Vertex &v : strip)
            QVERIFY2(qAbs(2.0 * v.hw - 4.0 * 0.08) < 1e-3,
                     qPrintable(QStringLiteral("толщина %1").arg(2.0 * v.hw)));
    }
```

Перед `void geometryStaysFiniteThroughSharpTurns()` добавить:

```cpp
    // ink-beauty-pass, D7: предел внутреннего выноса по радиусу кривизны в
    // мокрой полосе считается так же, как в полной тесселяции.
    void incrementalMatchesFullTessellationOnTightLoop()
    {
        WetStroke wet;
        wet.begin(0, prototype(6.0));
        const QList<QPointF> path = reference::circle(2.5, 3, QPointF(50, 50), 240);
        for (int i = 0; i < path.size(); ++i)
            wet.addPoint({path[i], 0.9, i / 200.0});
        const Stroke finished = wet.finish();

        StrokeTessellator tessellator;
        tessellator.setWidth(6.0);
        tessellator.setStyle(finished.style);
        const QVector<StrokeTessellator::Vertex> full = tessellator.tessellate(finished.points);

        QCOMPARE(wet.strip().size(), full.size());
        int limited = 0;
        for (int i = 0; i < full.size(); ++i) {
            QVERIFY2(sameVertex(wet.strip()[i], full[i], 1e-3f),
                     qPrintable(QStringLiteral("расхождение в вершине %1").arg(i)));
            if (std::isfinite(full[i].radiusLimit()))
                ++limited;
        }
        QVERIFY2(limited > 0, "петля должна задействовать предел по кривизне");
    }
```

- [x] 11.9a **Тест шага 1 `dryGeometryFollowsStrokeStyle` - на ось**

У вершины полосы больше нет полей `x` и `y`. В `tests/tst_canvasink.cpp`, в
`dryGeometryFollowsStrokeStyle`, цикл по вершинам заменить на положение кромки
`AxisVertex::at(hw)` (центры вееров дают центр - на размах это не влияет):

```cpp
              for (const StrokeTessellator::Vertex &vertex : strip) {
                  const QPointF edge = vertex.at(vertex.hw);
                  top = qMin(top, edge.y());
                  bottom = qMax(bottom, edge.y());
              }
```

В `tapWithoutMovementLeavesDot` (шаг 7.4) константы `kCapPairs` больше нет: торцы
теперь полувеера. Заменить

```cpp
          QVERIFY2(m_canvas.wetStripVertexCount() >= 4 * (StrokeTessellator::kCapPairs + 1),
```

на

```cpp
          QVERIFY2(m_canvas.wetStripVertexCount()
                       >= StrokeTessellator::kStartCapVertices + StrokeTessellator::kEndCapVertices,
```

Проверяется прогоном в 11.10 вместе с остальными тестами холста.

- [x] 11.9 **Мост в `DocumentCanvas` и тест торца в `tst_canvasink`**

В `src/canvas/documentcanvas.cpp` тело `uploadStrip` заменить целиком (временно, до
группы 12):

```cpp
void uploadStrip(QSGGeometryNode *node, const StrokeTessellator::Vertex *source, int count)
{
    QSGGeometry *geometry = node->geometry();
    geometry->allocate(count);
    auto *target = geometry->vertexDataAsPoint2D();
    // Мост до материала чернил: плоский материал под MSAA получает полосу в
    // покое - вершины на полуширине, без выноса бахромы.
    for (int i = 0; i < count; ++i) {
        const QPointF p = source[i].at(source[i].hw);
        target[i].set(float(p.x()), float(p.y()));
    }
    node->markDirty(QSGNode::DirtyGeometry);
}
```

В `tests/tst_canvasink.cpp`, `finishedStrokeIsTheSameEverywhere()`, последний блок: три
строки шага 2 (`const int capStart = ...` и `const QPointF capCenter(...)`, шаг 6.5)
вместе с комментарием над ними заменить:

```cpp
        // У всех вершин конечного торца центр - точка отрыва (design.md, D7).
        const QPointF capCenter = strip.constLast().center();
```

- [x] 11.10 **Прогон групп тестов чернил**

```
cmake --build build -j$(nproc) && \
ctest --test-dir build -R 'tst_(stroketessellator|wetstroke|strokesampler|canvasink|export|pagescenegraph)' --output-on-failure
```

Ожидаемо: `100% tests passed`. На копии без D6: `tst_stroketessellator` - `Totals: 25
passed`, `tst_wetstroke` - `Totals: 23 passed` (вместе с тестами шагов 1-2),
`tst_strokesampler` - `Totals: 37 passed`. `tst_canvasink`, `tst_export`, `tst_pagescenegraph` на копии не собирались
(нет libqpdf).

- [x] 11.11 **Падающий тест зеркала покрытия: `tests/tst_inkcoverage.cpp`**

```cpp
// ink-beauty-pass, D7: покрытие кромки по формулам материала чернил на CPU -
// зеркало шейдеров src/canvas/shaders/ink.{vert,frag} (src/ink/inkcoverage.h).
// Пол толщины - параметр материала, а не геометрии: та же вершина даёт разную
// ширину при разном поле.

#include <QTest>

#include "ink/inkcoverage.h"

namespace {

// Масса чернил поперёк штриха - интеграл покрытия по d. Покрытие не нулевое
// до |d| = hw + 0.5, отсюда пределы.
qreal massAcross(qreal halfWidthPx)
{
    constexpr qreal step = 0.0005;
    const qreal limit = halfWidthPx + 1.0;
    qreal mass = 0.0;
    for (qreal d = -limit; d <= limit; d += step)
        mass += ink::coverage(d, halfWidthPx) * step;
    return mass;
}

AxisVertex vertex(const QPointF &c, const QPointF &n, float hw, float s)
{
    return {float(c.x()), float(c.y()), float(n.x()), float(n.y()), hw, s};
}

} // namespace

class TestInkCoverage : public QObject
{
    Q_OBJECT

private slots:
    // Покрытие пиксельным боксом: cov = clamp(min(|d|+hw, .5) − max(|d|−hw, −.5), 0, 1).
    void boxCoverageAcrossTheEdge()
    {
        QCOMPARE(ink::coverage(0.0, 1.0), 1.0);
        QCOMPARE(ink::coverage(1.0, 1.0), 0.5);
        QCOMPARE(ink::coverage(-1.0, 1.0), 0.5);
        QCOMPARE(ink::coverage(1.5, 1.0), 0.0);
        // Тонкая линия - трапеция высотой 2·hw.
        QVERIFY(qAbs(ink::coverage(0.0, 0.2) - 0.4) < 1e-12);
        QVERIFY(qAbs(ink::coverage(0.4, 0.2) - 0.3) < 1e-12);
    }

    // Масса чернил на единицу длины равна толщине при любой толщине - без
    // hairline-приёмов (D7).
    void massEqualsWidthAtAnyThickness()
    {
        for (const qreal hw : {0.05, 0.2, 0.5, 1.5, 4.0}) {
            QVERIFY2(qAbs(massAcross(hw) - 2.0 * hw) < 2e-3,
                     qPrintable(QStringLiteral("hw %1: масса %2").arg(hw).arg(massAcross(hw))));
        }
    }

    // Пол толщины: самое тонкое перо на отдалённом зуме не тоньше 0.3 px.
    void floorKeepsThinnestPenVisible()
    {
        const AxisVertex v = vertex(QPointF(10, 10), QPointF(0, 1), 0.01f, 1.0f);
        QCOMPARE(ink::toDevice(v, 1.0).halfWidth, ink::kDefaultMinHalfWidthPx);
        QVERIFY(qAbs(ink::toDevice(v, 1.0, 0.0).halfWidth - 0.01) < 1e-6);
        QVERIFY(qAbs(ink::toDevice(v, 1.0, 0.3).halfWidth - 0.3) < 1e-9);
        QVERIFY(qAbs(massAcross(ink::toDevice(v, 1.0).halfWidth) - 0.3) < 2e-3);
    }

    // Вершина кромки выносится на 1 device px за кромку: pos = c + n·(hwPx + 1).
    void edgeVertexReachesOnePixelPastTheEdge()
    {
        const AxisVertex v = vertex(QPointF(4, 6), QPointF(1, 0), 1.0f, 1.0f);
        const ink::DeviceVertex d = ink::toDevice(v, 2.0); // hwPx 2, вынос 3 px
        QVERIFY(qAbs(d.position.x() - 5.5) < 1e-9 && qAbs(d.position.y() - 6.0) < 1e-9);
        QVERIFY(qAbs(d.distance - 3.0) < 1e-9);
        QVERIFY(qAbs(d.halfWidth - 2.0) < 1e-9);
    }

    // Внутренняя вершина стыка выносится не дальше радиуса кривизны; d - её
    // расстояние s·вынос.
    void innerVertexStopsAtCurvatureRadius()
    {
        const AxisVertex v = vertex(QPointF(0, 0), QPointF(0, 1.5), 2.0f, 0.8f); // ρ = 0.5
        QVERIFY(qAbs(v.radiusLimit() - 0.5) < 1e-6);
        const ink::DeviceVertex d = ink::toDevice(v, 2.0); // вынос min(5, 1) = 1 px
        QVERIFY(qAbs(d.position.x()) < 1e-9 && qAbs(d.position.y() - 0.5) < 1e-6);
        QVERIFY(qAbs(d.distance - 0.8) < 1e-6);
    }

    void centerVertexStaysOnAxis()
    {
        const AxisVertex v = vertex(QPointF(3, 4), QPointF(0, 0), 1.0f, 0.0f);
        const ink::DeviceVertex d = ink::toDevice(v, 3.0);
        QCOMPARE(d.position, QPointF(3, 4));
        QCOMPARE(d.distance, 0.0);
    }
};

QTEST_APPLESS_MAIN(TestInkCoverage)
#include "tst_inkcoverage.moc"
```

В `tests/CMakeLists.txt` после `uq_add_test(tst_stroketessellator ...)` добавить:

```cmake
uq_add_test(tst_inkcoverage tst_inkcoverage.cpp)
```

Прогон: `cmake --build build --target tst_inkcoverage` - ожидаемо ошибка: нет
`ink/inkcoverage.h`.

- [x] 11.12 **Создать `src/ink/inkcoverage.h`**

```cpp
#pragma once

#include <QPointF>

#include <algorithm>
#include <cmath>

#include "ink/stroketessellator.h"

// Покрытие кромки чернил на CPU - зеркало шейдеров src/canvas/shaders/ink.vert
// и ink.frag (design.md ink-beauty-pass, D7). GPU в тестах нет: паритет
// экспорта, пол толщины и стенд (вариант app-ink) считают покрытие здесь.
// Любая правка шейдера повторяется здесь, и наоборот.
namespace ink {

// Вынос вершины за кромку, device px: бахрома, в которой живёт сглаживание.
inline constexpr qreal kFringePx = 1.0;
// Пол полуширины, device px: самое тонкое перо на отдалённом зуме не
// распадается на точки. Значение - Open Questions design.md.
inline constexpr qreal kDefaultMinHalfWidthPx = 0.15;

struct DeviceVertex
{
    QPointF position;      // точки страницы, уже с выносом
    qreal distance = 0.0;  // d - знаковое расстояние поперёк штриха, device px
    qreal halfWidth = 0.0; // полуширина с полом, device px
};

// То, что делает ink.vert. pxPerUnit - device px в точке страницы.
inline DeviceVertex toDevice(const AxisVertex &v, qreal pxPerUnit,
                             qreal minHalfWidthPx = kDefaultMinHalfWidthPx)
{
    const qreal hwPx = std::max(qreal(v.hw) * pxPerUnit, minHalfWidthPx);
    const qreal extension = hwPx + kFringePx;
    // У внутренней вершины стыка вынос не дальше радиуса кривизны.
    const qreal reachPx = std::min(extension, v.radiusLimit() * pxPerUnit);
    return {v.center() + v.direction() * (reachPx / pxPerUnit), qreal(v.s) * reachPx, hwPx};
}

// То, что делает ink.frag: длина пересечения [|d| − hw, |d| + hw] с пиксельным
// боксом [−0.5, 0.5]. При hw ≥ 0.5 - обычная кромка clamp(hw + 0.5 − |d|),
// у тонкой линии - трапеция с массой, равной толщине.
inline qreal coverage(qreal distancePx, qreal halfWidthPx)
{
    const qreal ad = std::abs(distancePx);
    return std::clamp(std::min(ad + halfWidthPx, 0.5) - std::max(ad - halfWidthPx, -0.5), 0.0,
                      1.0);
}

} // namespace ink
```

В `src/CMakeLists.txt` в SOURCES `qt_add_qml_module` после `ink/inkdocument.h` добавить
`ink/inkcoverage.h`.

Прогон:

```
cmake --build build -j$(nproc) --target tst_inkcoverage && \
ctest --test-dir build -R tst_inkcoverage --output-on-failure
```

Ожидаемо: `100% tests passed` (на копии - `Totals: 8 passed`: шесть слотов и init/cleanup).

- [x] 11.13 **Стенд: собираться с новой вершиной и показывать приложение (`app-ink`)**

Стенд компилирует `src/ink/*.cpp` приложения, поэтому `baseline` и `AppPipeline`
перестают собираться. Правка - полоса в покое под эмуляцией MSAA, как раньше:

```
V=tools/stroke-lab/variants
grep -l 'StrokeTessellator::Vertex' $V/*
sed -i 's/g\.vertices\.emplace_back(v\.x, v\.y);/g.vertices.push_back(v.at(v.hw));/' \
  $(grep -l 'StrokeTessellator::Vertex' $V/*)
# У прототипа aa-fringe своя структура AxisVertex в анонимном пространстве имён;
# рядом с ::AxisVertex из stroketessellator.h имя в fringeGpuStrip() становится
# неоднозначным (проверено: «reference to AxisVertex is ambiguous»).
sed -i 's/\bAxisVertex\b/FringeAxisVertex/g' $V/aa_fringe.cpp
```

Ожидаемо: первый `grep` печатает `baseline.cpp`, `app_pipeline.h` и, если шаг 2 оставил
в стенде исследовательский `d6_raw121_cusp.cpp`, его - только их и правит `sed`. В `outline.cpp`, `dry_refine.cpp`
тот же текст `v.x, v.y`, но со своими копиями тесселятора: их трогать нельзя, поэтому
`sed` идёт не по маске файлов.

Создать `tools/stroke-lab/variants/app_ink.cpp`:

```cpp
// app-ink - приложение после шага 3 ink-beauty-pass: полоса AxisVertex из
// StrokeTessellator и покрытие по формулам материала чернил
// (src/ink/inkcoverage.h - зеркало шейдеров), over на каждый фрагмент, без
// MSAA. Траектория - как у baseline (AppPipeline).
//
// Сверяется с aa-fringe-rho, прошедшим гейт D7: расхождение значит, что
// приложение ушло от прототипа. baseline после шага 3 - та же полоса в покое
// под эмуляцией MSAA 4x, то есть прежняя отрисовка.

#include "app_pipeline.h"

#include "aaraster.h"
#include "ink/inkcoverage.h"

#include <cmath>

namespace {

using namespace lab;

class AppInk : public AppPipeline
{
public:
    bool rasterize(const Geometry &, const RasterTarget &target) const override
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(m_final.width);
        tessellator.setStyle(m_final.style);
        const QVector<StrokeTessellator::Vertex> strip = tessellator.tessellate(m_final.points);
        if (StrokeTessellator::isDegenerate(strip) || !target.image)
            return false;

        const QTransform &T = target.pageToPixel;
        const qreal pxPerUnit = std::sqrt(std::abs(T.determinant()));
        std::vector<AttrVertex<2>> vertices;
        vertices.reserve(size_t(strip.size()));
        for (const AxisVertex &a : strip) {
            const ink::DeviceVertex d = ink::toDevice(a, pxPerUnit);
            AttrVertex<2> out;
            out.p = T.map(d.position);
            out.a = {float(d.distance), float(d.halfWidth)};
            vertices.push_back(out);
        }

        FragmentRaster raster(target.image->width(), target.image->height());
        AlphaBuffer buffer(target.image->width(), target.image->height());
        const float inkAlpha = float(target.color.alphaF());
        raster.strip(vertices, [&](int x, int y, const std::array<float, 2> &a) {
            const float cov = float(ink::coverage(a[0], a[1]));
            if (cov > 0.0f)
                buffer.over(x, y, cov * inkAlpha);
        });
        QColor opaque = target.color;
        opaque.setAlpha(255);
        buffer.compositeOnto(*target.image, opaque);
        return true;
    }
};

} // namespace

STROKE_LAB_VARIANT(AppInk, "app-ink",
                   "приложение после шага 3: полоса AxisVertex из StrokeTessellator, покрытие "
                   "по формулам InkMaterial (src/ink/inkcoverage.h), over, без MSAA");
```

- [x] 11.14 **Сверить приложение с прототипом на стенде**

```
cmake --build build -j$(nproc) --target stroke-lab
for w in 1 4.5; do
  ./build/tools/stroke-lab/stroke-lab --variants baseline,aa-fringe-rho,app-ink \
    --samples small_e_10,small_o_8,word_minimum_small,corner_M --scales 1,2.5 \
    --width $w --dpr 2 --out /tmp/app-ink-w$w > /dev/null
done
python3 - <<'EOF'
import glob, os
from PIL import Image, ImageChops
bad = 0
for w in ("1", "4.5"):
    for f in sorted(glob.glob(f"/tmp/app-ink-w{w}/app-ink/png/*.png")):
        if f.endswith("_ideal.png"):
            continue
        a = Image.open(f).convert("L")
        b = Image.open(f.replace("/app-ink/", "/aa-fringe-rho/")).convert("L")
        histogram = ImageChops.difference(a, b).histogram()
        differ = sum(histogram[9:])                      # пикселей с разницей > 8 уровней
        inked = sum(1 for p in a.get_flattened_data() if p < 250)
        flag = "" if differ <= max(1, 0.02 * inked) else "  <-- расхождение"
        bad += bool(flag)
        print(f"w{w} {os.path.basename(f)}: {differ} из {inked}{flag}")
print("совпадает с прототипом" if not bad else "РАСХОЖДЕНИЕ")
EOF
```

Ожидаемо: `совпадает с прототипом`. Сравнение идёт по картинкам, а не по
`raster_vs_ideal_p99`: на мелких сэмплах один пиксель сдвигает p99 (на копии с D6 так
разошлись 0.524 и 0.577). На копии без D6 различие - 0-2 пикселя на снимок (не больше
0.3% закрашенных), и все у торцов: направление торца у приложения - хорда
(`capDirection`), у прототипа - последний сегмент. Если отличий больше 2% закрашенных пикселей, вернуться к 11.4: стык
или предел в приложении разошёлся с `aa-fringe-rho`.

- [x] 11.15 **Полный прогон и коммит**

```
ctest --test-dir build --output-on-failure
```

Ожидаемо: `100% tests passed` (`tst_idleframes` - SKIP без сессии).

```
git add src/ink/stroketessellator.h src/ink/stroketessellator.cpp src/ink/wetstroke.cpp \
        src/ink/inkcoverage.h src/CMakeLists.txt src/canvas/documentcanvas.cpp \
        tests/tst_stroketessellator.cpp tests/tst_wetstroke.cpp tests/tst_canvasink.cpp \
        tests/tst_inkcoverage.cpp tests/CMakeLists.txt \
        tools/stroke-lab/variants
# в tools/stroke-lab/variants: baseline.cpp, app_pipeline.h (и d6_raw121_cusp.cpp, если есть) - sed,
# aa_fringe.cpp (переименование), новый app_ink.cpp
git commit -m "$(cat <<'EOF'
Оболочка чернил с осью стыка и пределом по кривизне

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
)"
```

---

## 12. InkMaterial: материал, шейдеры, переход DocumentCanvas

> **Изменение при исполнении (решение пользователя после группы 10, design.md
> D7):** предела внутреннего выноса по радиусу кривизны нет. Ограничение прошло
> гейт, но вырезало пиксели внутри чернил на вершинах арок «m» и «n». Поэтому
> в группах 11 и 12 всё, что относится к ρ, не реализуется: нет
> `curvatureRadius`, нет кодирования `|n| = 1 + ρ` и `AxisVertex::kRadiusTag`
> (направление вершины всегда единичное, кроме нулевого у центров вееров), нет
> теста `innerReachIsLimitedByCurvatureRadius`, в `ink.vert` вынос всегда
> `ext` без ветки по метке. Остальное - точные стыки, веер вокруг центра,
> полувеер торцов, пол толщины, зеркало покрытия - как написано. В 11.13-11.14
> вариант `app-ink` сверяется с `aa-fringe`, а не с `aa-fringe-rho`.


**Files:**
- Create: `src/canvas/inkmaterial.h`, `src/canvas/inkmaterial.cpp`
- Create: `src/canvas/shaders/ink.vert`, `src/canvas/shaders/ink.frag`
- Modify: `CMakeLists.txt` (ShaderTools), `src/CMakeLists.txt` (исходники, `qt_add_shaders`)
- Modify: `src/canvas/documentcanvas.h`, `src/canvas/documentcanvas.cpp`
- Modify: `src/main.cpp` (комментарий о MSAA)
- Modify: `README.md`, `packaging/PKGBUILD` (зависимость сборки `qt6-shadertools`)
- Create: `tests/tst_inkmaterial.cpp`, `tests/tst_inkmaterialgpu.cpp`; Modify: `tests/CMakeLists.txt`, `tests/tst_pagescenegraph.cpp`

`qt6-shadertools` 6.11.2 на машине установлен (`/usr/lib/cmake/Qt6ShaderToolsTools`,
`/usr/lib/qt6/bin/qsb`). Шейдеры собираются в `.qsb` при сборке и лежат в ресурсах,
поэтому пакету он нужен только как зависимость сборки.

- [ ] 12.1 **Падающий тест материала: `tests/tst_inkmaterial.cpp`**

```cpp
// ink-beauty-pass, D7: материал чернил. Тип мокрого слоя отличается от сухого
// (иначе мокрый узел сливается в батч с сухими, и каждое событие пера
// перезаливает VBO страницы), compare() группирует по цвету и полу, флаги
// оставляют масштаб в матрице, шейдеры собраны батчируемыми, а раскладка
// униформ и входов совпадает с тем, что пишет C++.

#include <QFile>
#include <QHash>
#include <QTest>

#include <rhi/qshader.h>

#include <memory>

#include "canvas/inkmaterial.h"
#include "ink/stroketessellator.h"

namespace {

QShader loadShader(const QString &name)
{
    QFile file(QStringLiteral(":/uqnotes/shaders/") + name + QStringLiteral(".qsb"));
    if (!file.open(QIODevice::ReadOnly))
        return QShader();
    return QShader::fromSerialized(file.readAll());
}

} // namespace

class TestInkMaterial : public QObject
{
    Q_OBJECT

private slots:
    void wetAndDryLayersHaveDistinctTypes()
    {
        InkMaterial dry;
        InkMaterial otherDry;
        InkMaterial wet(InkMaterial::Layer::Wet);
        QCOMPARE(dry.type(), otherDry.type());
        QVERIFY(dry.type() != wet.type());
        QCOMPARE(wet.layer(), InkMaterial::Layer::Wet);
    }

    void compareGroupsByColorFloorAndBatchKey()
    {
        InkMaterial a;
        InkMaterial b;
        a.setColor(QColor(0x1c, 0x2a, 0x5a));
        b.setColor(QColor(0x1c, 0x2a, 0x5a));
        QCOMPARE(a.compare(&b), 0);

        b.setColor(QColor(200, 20, 30));
        QVERIFY(a.compare(&b) != 0);
        QCOMPARE(a.compare(&b), -b.compare(&a));

        b.setColor(a.color());
        b.setMinHalfWidthPx(0.3f);
        QVERIFY(a.compare(&b) != 0);

        b.setMinHalfWidthPx(a.minHalfWidthPx());
        b.setBatchKey(1);
        QVERIFY2(a.compare(&b) != 0, "куски мокрого штриха обязаны не сливаться в один батч");
    }

    void materialBlendsAndKeepsScaleInMatrix()
    {
        InkMaterial material;
        const QSGMaterial::Flags flags = material.flags();
        QVERIFY(flags.testFlag(QSGMaterial::Blending));
        // Масштаб - в матрице, на CPU переносится только сдвиг центра.
        QVERIFY(flags.testFlags(QSGMaterial::RequiresFullMatrixExceptTranslate));
        QVERIFY(!flags.testFlags(QSGMaterial::RequiresFullMatrix));
        QVERIFY(!flags.testFlag(QSGMaterial::NoBatching));
    }

    // Пол толщины - состояние материала (униформ), а не геометрии.
    void floorIsMaterialState()
    {
        InkMaterial material;
        QCOMPARE(material.minHalfWidthPx(), 0.15f);
        material.setMinHalfWidthPx(0.3f);
        QCOMPARE(material.minHalfWidthPx(), 0.3f);
    }

    void attributesDescribeAxisVertex()
    {
        const QSGGeometry::AttributeSet &set = InkMaterial::attributes();
        QCOMPARE(set.count, 4);
        QCOMPARE(set.stride, int(sizeof(AxisVertex)));
        QCOMPARE(set.stride, 24);
        const int tuples[] = {2, 2, 1, 1};
        for (int i = 0; i < set.count; ++i) {
            QCOMPARE(set.attributes[i].position, i);
            QCOMPARE(set.attributes[i].tupleSize, tuples[i]);
            QCOMPARE(set.attributes[i].type, int(QSGGeometry::FloatType));
            // Позиция для батч-рендера - только центр: его он сдвигает на CPU.
            QCOMPARE(bool(set.attributes[i].isVertexCoordinate), i == 0);
        }
    }

    void shadersAreBuiltBatchable()
    {
        const QShader vertex = loadShader(QStringLiteral("ink.vert"));
        const QShader fragment = loadShader(QStringLiteral("ink.frag"));
        QVERIFY2(vertex.isValid(), "нет :/uqnotes/shaders/ink.vert.qsb");
        QVERIFY2(fragment.isValid(), "нет :/uqnotes/shaders/ink.frag.qsb");
        QCOMPARE(vertex.stage(), QShader::VertexStage);
        QCOMPARE(fragment.stage(), QShader::FragmentStage);
        bool batchable = false;
        for (const QShaderKey &key : vertex.availableShaders())
            batchable = batchable || key.sourceVariant() == QShader::BatchableVertexShader;
        QVERIFY2(batchable, "вершинный шейдер собран без BATCHABLE");
    }

    void vertexInputsMatchAttributes()
    {
        const QShader vertex = loadShader(QStringLiteral("ink.vert"));
        QHash<QByteArray, int> locations;
        for (const QShaderDescription::InOutVariable &input : vertex.description().inputVariables())
            locations.insert(input.name, input.location);
        QCOMPARE(locations.value("center", -1), 0);
        QCOMPARE(locations.value("dir", -1), 1);
        QCOMPARE(locations.value("hw", -1), 2);
        QCOMPARE(locations.value("s", -1), 3);
    }

    // Смещения в униформ-блоке - те, по которым пишет updateUniformData().
    void uniformLayoutMatchesMaterial()
    {
        for (const QString &name : {QStringLiteral("ink.vert"), QStringLiteral("ink.frag")}) {
            const QShader shader = loadShader(name);
            const QList<QShaderDescription::UniformBlock> blocks =
                shader.description().uniformBlocks();
            QCOMPARE(blocks.size(), 1);
            QCOMPARE(blocks.first().binding, 0);
            QVERIFY(blocks.first().size >= InkMaterial::Uniforms::kUsedSize);
            QHash<QByteArray, int> offsets;
            for (const QShaderDescription::BlockVariable &member : blocks.first().members)
                offsets.insert(member.name, member.offset);
            QCOMPARE(offsets.value("qt_Matrix", -1), InkMaterial::Uniforms::kMatrix);
            QCOMPARE(offsets.value("color", -1), InkMaterial::Uniforms::kColor);
            QCOMPARE(offsets.value("pxPerUnit", -1), InkMaterial::Uniforms::kPxPerUnit);
            QCOMPARE(offsets.value("minHwPx", -1), InkMaterial::Uniforms::kMinHalfWidthPx);
            QCOMPARE(offsets.value("fringePx", -1), InkMaterial::Uniforms::kFringePx);
        }
    }

    void createsShader()
    {
        InkMaterial material(InkMaterial::Layer::Wet);
        std::unique_ptr<QSGMaterialShader> shader(
            material.createShader(QSGRendererInterface::RenderMode2D));
        QVERIFY(shader);
    }
};

QTEST_MAIN(TestInkMaterial)
#include "tst_inkmaterial.moc"
```

В `tests/CMakeLists.txt` строку `find_package(Qt6 REQUIRED COMPONENTS Test)` заменить на
`find_package(Qt6 REQUIRED COMPONENTS Test GuiPrivate)` и после `uq_add_test(tst_inkcoverage ...)`
добавить:

```cmake
# QShader и его отражение - в приватных заголовках QtGui (rhi/qshader.h).
uq_add_test(tst_inkmaterial tst_inkmaterial.cpp)
target_link_libraries(tst_inkmaterial PRIVATE Qt6::GuiPrivate)
```

- [ ] 12.2 **Прогон - ожидаемый провал**

```
cmake -S . -B build && cmake --build build --target tst_inkmaterial
```

Ожидаемо: ошибка компиляции - нет `canvas/inkmaterial.h`.

- [ ] 12.3 **Зависимость сборки ShaderTools**

`CMakeLists.txt` (корень): строку `find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Pdf Sql Concurrent)`
заменить:

```cmake
# ShaderTools - только для сборки: шейдеры чернил собираются в .qsb и лежат в
# ресурсах (design.md ink-beauty-pass, D7).
find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Pdf Sql Concurrent ShaderTools)
```

`packaging/PKGBUILD`: `makedepends=('cmake' 'librsvg')` → `makedepends=('cmake' 'librsvg' 'qt6-shadertools')`.

`README.md`, раздел «Зависимости»: строку
`pacman -S qt6-base qt6-declarative qt6-wayland qt6-webengine qpdf abseil-cpp cmake` заменить на
`pacman -S qt6-base qt6-declarative qt6-wayland qt6-webengine qt6-shadertools qpdf abseil-cpp cmake`
и после абзаца про `qt6-webengine` добавить:

```
`qt6-shadertools` нужен только для сборки: им собираются шейдеры материала
чернил (`src/canvas/shaders/`).
```

- [ ] 12.4 **Шейдеры**

`src/canvas/shaders/ink.vert`:

```glsl
#version 440
// Материал чернил (design.md ink-beauty-pass, D7). Вершина AxisVertex
// (src/ink/stroketessellator.h) выносится на fringePx device px за кромку:
// pos = c + n̂·(hwPx + 1), varying vD = s·вынос - знаковое расстояние поперёк
// штриха в device px. У внутренней вершины стыка |dir| = 1 + ρ, вынос не
// дальше ρ. Пересчёт в device px - здесь, по pxPerUnit из матрицы и dpr,
// поэтому зум не пересобирает геометрию. CPU-зеркало - src/ink/inkcoverage.h.
// Собирается с BATCHABLE: location 7 занимает батч-рендер Qt Quick.

layout(location = 0) in vec2 center;
layout(location = 1) in vec2 dir;
layout(location = 2) in float hw;
layout(location = 3) in float s;

layout(location = 0) out float vD;
layout(location = 1) out float vHw;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    vec4 color;      // premultiplied, с прозрачностью узла
    float pxPerUnit; // device px в точке страницы
    float minHwPx;   // пол полуширины, device px
    float fringePx;  // вынос за кромку, device px
};

out gl_PerVertex { vec4 gl_Position; };

// Совпадает с AxisVertex::kRadiusTag.
const float kRadiusTag = 1.001;

void main()
{
    float k = max(pxPerUnit, 1e-6);
    float hwPx = max(hw * k, minHwPx);
    float ext = hwPx + fringePx;
    float len = length(dir);
    vec2 unitDir = len > 0.0 ? dir / len : vec2(0.0);
    float reach = len > kRadiusTag ? min(ext, (len - 1.0) * k) : ext;
    vD = s * reach;
    vHw = hwPx;
    gl_Position = qt_Matrix * vec4(center + unitDir * (reach / k), 0.0, 1.0);
}
```

`src/canvas/shaders/ink.frag`:

```glsl
#version 440
// Точное покрытие пиксельным боксом поперёк штриха (design.md, D7):
// cov = clamp(min(|d| + hw, 0.5) − max(|d| − hw, −0.5), 0, 1). При hw ≥ 0.5 -
// обычная кромка, у тонкой линии - трапеция с массой, равной толщине.
// Смешивание - premultiplied over (Blending в InkMaterial).

layout(location = 0) in float vD;
layout(location = 1) in float vHw;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    vec4 color;
    float pxPerUnit;
    float minHwPx;
    float fringePx;
};

void main()
{
    float ad = abs(vD);
    float cov = clamp(min(ad + vHw, 0.5) - max(ad - vHw, -0.5), 0.0, 1.0);
    fragColor = color * cov;
}
```

- [ ] 12.5 **`src/canvas/inkmaterial.h`**

```cpp
#pragma once

#include <QColor>
#include <QSGGeometry>
#include <QSGMaterial>

// Материал чернил с аналитическим сглаживанием кромки (design.md
// ink-beauty-pass, D7). Геометрия - полоса AxisVertex; кромку выносит и
// сглаживает шейдер, поэтому MSAA чернилам не нужен, а зум меняет только
// униформы.
//
// Сухие штрихи страницы одного цвета сливаются в батч, как раньше. Мокрый слой
// - отдельный тип: иначе мокрый узел сливается с сухими, и каждое событие пера
// перезаливает VBO всей страницы. Куски мокрого штриха различаются ключом
// батча: каждый кусок - свой батч, и на событие перезаливается только
// изменившийся кусок, а не весь штрих.
class InkMaterial : public QSGMaterial
{
public:
    enum class Layer {
        Dry,
        Wet,
    };

    // Смещения полей униформ-блока buf в шейдерах (std140). tst_inkmaterial
    // сверяет их с отражением собранных .qsb.
    struct Uniforms
    {
        static constexpr int kMatrix = 0;
        static constexpr int kColor = 64;
        static constexpr int kPxPerUnit = 80;
        static constexpr int kMinHalfWidthPx = 84;
        static constexpr int kFringePx = 88;
        static constexpr int kUsedSize = 92;
    };

    explicit InkMaterial(Layer layer = Layer::Dry);

    QSGMaterialType *type() const override;
    QSGMaterialShader *createShader(QSGRendererInterface::RenderMode renderMode) const override;
    int compare(const QSGMaterial *other) const override;

    Layer layer() const { return m_layer; }

    QColor color() const { return m_color; }
    void setColor(const QColor &color);

    // Пол полуширины, device px (design.md, D7, «Пол толщины»).
    float minHalfWidthPx() const { return m_minHalfWidthPx; }
    void setMinHalfWidthPx(float px);

    int batchKey() const { return m_batchKey; }
    void setBatchKey(int key);

    // Раскладка AxisVertex для QSGGeometry: центр (позиция для батч-рендера),
    // направление, полуширина, s - входы 0..3 шейдера.
    static const QSGGeometry::AttributeSet &attributes();

private:
    Layer m_layer;
    QColor m_color = QColor(Qt::black);
    float m_minHalfWidthPx;
    int m_batchKey = 0;
};
```

- [ ] 12.6 **`src/canvas/inkmaterial.cpp`**

```cpp
#include "inkmaterial.h"

#include <QMatrix4x4>
#include <QSGMaterialShader>
#include <QVector4D>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ink/inkcoverage.h"
#include "ink/stroketessellator.h"

namespace {

QSGMaterialType g_dryInkType;
QSGMaterialType g_wetInkType;

class InkMaterialShader : public QSGMaterialShader
{
public:
    InkMaterialShader()
    {
        setShaderFileName(VertexStage, QStringLiteral(":/uqnotes/shaders/ink.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/uqnotes/shaders/ink.frag.qsb"));
    }

    bool updateUniformData(RenderState &state, QSGMaterial *newMaterial,
                           QSGMaterial *oldMaterial) override
    {
        Q_ASSERT(!oldMaterial || newMaterial->type() == oldMaterial->type());
        const auto *material = static_cast<const InkMaterial *>(newMaterial);
        const auto *previous = static_cast<const InkMaterial *>(oldMaterial);
        QByteArray *buffer = state.uniformData();
        Q_ASSERT(buffer->size() >= InkMaterial::Uniforms::kUsedSize);
        char *data = buffer->data();
        bool changed = false;

        if (state.isMatrixDirty()) {
            const QMatrix4x4 matrix = state.combinedMatrix();
            std::memcpy(data + InkMaterial::Uniforms::kMatrix, matrix.constData(), 64);
            // Device px в точке страницы. Флаг RequiresFullMatrixExceptTranslate
            // оставляет масштаб зума в матрице и в слитом батче: на CPU
            // переносится только сдвиг центра, поэтому hw и n в точках
            // страницы верны.
            const QMatrix4x4 modelView = state.modelViewMatrix();
            const float determinant =
                modelView(0, 0) * modelView(1, 1) - modelView(0, 1) * modelView(1, 0);
            const float pxPerUnit = std::sqrt(std::abs(determinant)) * state.devicePixelRatio();
            std::memcpy(data + InkMaterial::Uniforms::kPxPerUnit, &pxPerUnit, sizeof(float));
            changed = true;
        }

        if (!previous || previous->color() != material->color() || state.isOpacityDirty()) {
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;
            float a = 0.0f;
            material->color().getRgbF(&r, &g, &b, &a);
            const float alpha = a * state.opacity();
            const QVector4D premultiplied(r * alpha, g * alpha, b * alpha, alpha);
            std::memcpy(data + InkMaterial::Uniforms::kColor, &premultiplied, 16);
            changed = true;
        }

        if (!previous || previous->minHalfWidthPx() != material->minHalfWidthPx()) {
            const float floor = material->minHalfWidthPx();
            const float fringe = float(ink::kFringePx);
            std::memcpy(data + InkMaterial::Uniforms::kMinHalfWidthPx, &floor, sizeof(float));
            std::memcpy(data + InkMaterial::Uniforms::kFringePx, &fringe, sizeof(float));
            changed = true;
        }

        return changed;
    }
};

template <typename T>
int order(const T &a, const T &b)
{
    return a < b ? -1 : (b < a ? 1 : 0);
}

} // namespace

InkMaterial::InkMaterial(Layer layer)
    : m_layer(layer)
    , m_minHalfWidthPx(float(ink::kDefaultMinHalfWidthPx))
{
    // Бахрома полупрозрачна по построению: смешивание нужно всегда, даже у
    // непрозрачного цвета.
    setFlag(Blending);
    setFlag(RequiresFullMatrixExceptTranslate);
}

QSGMaterialType *InkMaterial::type() const
{
    return m_layer == Layer::Wet ? &g_wetInkType : &g_dryInkType;
}

QSGMaterialShader *InkMaterial::createShader(QSGRendererInterface::RenderMode renderMode) const
{
    Q_UNUSED(renderMode)
    return new InkMaterialShader;
}

int InkMaterial::compare(const QSGMaterial *other) const
{
    // Тип у сравниваемых всегда один: батч-рендер сравнивает только узлы
    // одного type().
    const auto *ink = static_cast<const InkMaterial *>(other);
    if (const int byColor = order(m_color.rgba(), ink->m_color.rgba()))
        return byColor;
    if (const int byFloor = order(m_minHalfWidthPx, ink->m_minHalfWidthPx))
        return byFloor;
    return order(m_batchKey, ink->m_batchKey);
}

void InkMaterial::setColor(const QColor &color)
{
    m_color = color;
}

void InkMaterial::setMinHalfWidthPx(float px)
{
    m_minHalfWidthPx = std::max(px, 0.0f);
}

void InkMaterial::setBatchKey(int key)
{
    m_batchKey = key;
}

const QSGGeometry::AttributeSet &InkMaterial::attributes()
{
    static const QSGGeometry::Attribute data[] = {
        QSGGeometry::Attribute::create(0, 2, QSGGeometry::FloatType, true), // center
        QSGGeometry::Attribute::create(1, 2, QSGGeometry::FloatType),       // dir
        QSGGeometry::Attribute::create(2, 1, QSGGeometry::FloatType),       // hw
        QSGGeometry::Attribute::create(3, 1, QSGGeometry::FloatType),       // s
    };
    static const QSGGeometry::AttributeSet set = {4, int(sizeof(AxisVertex)), data};
    return set;
}
```

- [ ] 12.7 **`src/CMakeLists.txt`: исходники и шейдеры**

В SOURCES `qt_add_qml_module` после `canvas/insertlistmodel.h` добавить:

```
        canvas/inkmaterial.cpp
        canvas/inkmaterial.h
```

После блока `target_link_libraries(uqnotes_core PUBLIC ...)` добавить:

```cmake
# Шейдеры материала чернил (design.md ink-beauty-pass, D7). BATCHABLE
# обязателен для материалов Qt Quick: без батчируемого варианта вершинного
# шейдера узлы чернил не сливаются в батчи. Ресурсы: :/uqnotes/shaders/*.qsb.
qt_add_shaders(uqnotes_core "uqnotes_ink_shaders"
    BATCHABLE
    PREFIX "/uqnotes"
    BASE canvas
    FILES
        canvas/shaders/ink.vert
        canvas/shaders/ink.frag
)
```

- [ ] 12.8 **Прогон `tst_inkmaterial`**

```
cmake -S . -B build 2>&1 | grep 'exposed as' ; \
cmake --build build -j$(nproc) --target tst_inkmaterial && \
ctest --test-dir build -R tst_inkmaterial --output-on-failure
```

Ожидаемо: при конфигурации две строки
`canvas/shaders/ink.vert -> shaders/ink.vert.qsb exposed as :/uqnotes/shaders/ink.vert.qsb`
(и `ink.frag`), затем `100% tests passed`. Проверено отдельным проектом (статическая
библиотека с `InkMaterial` и `qt_add_shaders`, тест против неё): `Totals: 11 passed`,
ресурсы `.qsb` доходят до теста сами.

- [ ] 12.9 **Материал на настоящем RHI: `tests/tst_inkmaterialgpu.cpp`**

Граф сцены Qt Quick рисует полосы `InkMaterial` в текстуру через `QQuickRenderControl`
(окна на экране нет), профиль кромки сверяется с CPU-зеркалом. Под `offscreen` RHI нет,
и тест сообщает SKIP, поэтому `ctest` не краснеет без GPU. На машине с GPU:

```cpp
// ink-beauty-pass, D7: материал чернил на настоящем RHI. Граф сцены Qt Quick
// рисует полосу через QQuickRenderControl в текстуру - окна на экране нет, -
// и профиль покрытия поперёк штриха сверяется с CPU-зеркалом шейдеров
// (src/ink/inkcoverage.h). Зум меняет только матрицу узла: геометрия та же,
// а кромка обязана пересчитаться в шейдере. 80 узлов - чтобы узел зума стал
// корнем батча, и полосы прошли через слитый батч батч-рендера.
//
// Под QT_QPA_PLATFORM=offscreen RHI обычно нет (программный бэкенд) - тогда
// SKIP. Проверка на GPU: QT_QPA_PLATFORM=wayland ./tst_inkmaterialgpu.

#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGTransformNode>
#include <QTest>

#include <rhi/qrhi.h>

#include <cstring>
#include <memory>

#include "canvas/inkmaterial.h"
#include "ink/inkcoverage.h"
#include "ink/stroketessellator.h"

namespace {

constexpr int kStrokes = 80;
constexpr qreal kFirstAxisY = 30.3; // ось первого штриха, точки страницы
constexpr qreal kHalfWidth = 1.0;   // перо 2 pt, полный нажим

// Горизонтальные штрихи под одним узлом зума.
class InkItem : public QQuickItem
{
public:
    InkItem() { setFlag(ItemHasContents); }
    qreal zoom = 1.0;

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override
    {
        QMatrix4x4 matrix;
        matrix.scale(float(zoom));
        if (old) {
            static_cast<QSGTransformNode *>(old)->setMatrix(matrix);
            return old;
        }
        auto *root = new QSGTransformNode;
        root->setMatrix(matrix);
        StrokeTessellator tessellator;
        tessellator.setWidth(2.0 * kHalfWidth);
        tessellator.setMinWidthFactor(0.0);
        for (int k = 0; k < kStrokes; ++k) {
            QList<InkPoint> points;
            for (int i = 0; i <= 40; ++i)
                points.append({QPointF(20 + i * 4.0, kFirstAxisY + k * 5.0), 1.0, i / 200.0});
            const QVector<AxisVertex> strip = tessellator.tessellate(points);
            auto *node = new QSGGeometryNode;
            auto *geometry = new QSGGeometry(InkMaterial::attributes(), int(strip.size()));
            geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
            std::memcpy(geometry->vertexData(), strip.constData(),
                        size_t(strip.size()) * sizeof(AxisVertex));
            node->setGeometry(geometry);
            node->setFlag(QSGNode::OwnsGeometry);
            auto *material = new InkMaterial;
            material->setColor(Qt::black);
            node->setMaterial(material);
            node->setFlag(QSGNode::OwnsMaterial);
            root->appendChildNode(node);
        }
        return root;
    }
};

} // namespace

class TestInkMaterialGpu : public QObject
{
    Q_OBJECT

private slots:
    void edgeCoverageMatchesMirrorAtEveryZoom()
    {
        const QSize size(400, 200);
        QQuickRenderControl control;
        QQuickWindow window(&control);
        window.setColor(Qt::white);
        window.resize(size);
        auto *item = new InkItem;
        item->setParentItem(window.contentItem());
        item->setSize(QSizeF(size));

        if (!control.initialize() || !control.rhi())
            QSKIP("RHI недоступен на этой платформе (программный бэкенд)");
        QRhi *rhi = control.rhi();
        qInfo("RHI %s, %s", rhi->backendName(), rhi->driverInfo().deviceName.constData());

        std::unique_ptr<QRhiTexture> texture(rhi->newTexture(
            QRhiTexture::RGBA8, size, 1,
            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        QVERIFY(texture->create());
        std::unique_ptr<QRhiRenderBuffer> depth(
            rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size, 1));
        QVERIFY(depth->create());
        QRhiTextureRenderTargetDescription description{QRhiColorAttachment(texture.get())};
        description.setDepthStencilBuffer(depth.get());
        std::unique_ptr<QRhiTextureRenderTarget> target(
            rhi->newTextureRenderTarget(description));
        std::unique_ptr<QRhiRenderPassDescriptor> pass(
            target->newCompatibleRenderPassDescriptor());
        target->setRenderPassDescriptor(pass.get());
        QVERIFY(target->create());
        window.setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(target.get()));

        const auto frame = [&] {
            control.polishItems();
            control.beginFrame();
            control.sync();
            control.render();
            QRhiReadbackResult readback;
            QRhiResourceUpdateBatch *batch = rhi->nextResourceUpdateBatch();
            batch->readBackTexture({texture.get()}, &readback);
            control.commandBuffer()->resourceUpdate(batch);
            control.endFrame();
            QImage image(reinterpret_cast<const uchar *>(readback.data.constData()),
                         readback.pixelSize.width(), readback.pixelSize.height(),
                         QImage::Format_RGBA8888_Premultiplied);
            QImage copy = image.convertToFormat(QImage::Format_RGB32);
            return rhi->isYUpInFramebuffer() ? copy.flipped(Qt::Vertical) : copy;
        };

        for (const qreal zoom : {1.0, 2.0, 1.5, 3.0}) {
            item->zoom = zoom;
            item->update();
            const QImage image = frame();
            // Device px в точке страницы: зум и dpr цели (у текстуры - 1).
            const qreal k = zoom;
            const int x = int(100 * k);
            const qreal axis = kFirstAxisY * k;
            qreal worst = 0.0;
            qreal massGpu = 0.0;
            qreal massMirror = 0.0;
            for (int y = int(axis - 2 * k); y <= int(axis + 2 * k); ++y) {
                const qreal gpu = 1.0 - qRed(image.pixel(x, y)) / 255.0;
                const qreal mirror = ink::coverage(y + 0.5 - axis, kHalfWidth * k);
                worst = qMax(worst, qAbs(gpu - mirror));
                massGpu += gpu;
                massMirror += mirror;
            }
            QVERIFY2(worst <= 2.0 / 255.0,
                     qPrintable(QStringLiteral("зум %1: ошибка покрытия %2, масса %3 против %4")
                                    .arg(zoom)
                                    .arg(worst)
                                    .arg(massGpu)
                                    .arg(massMirror)));
        }
    }
};

QTEST_MAIN(TestInkMaterialGpu)
#include "tst_inkmaterialgpu.moc"
```

В `tests/CMakeLists.txt` после `tst_inkmaterial`:

```cmake
# На GPU: QT_QPA_PLATFORM=wayland ./build/tests/tst_inkmaterialgpu. Под offscreen - SKIP.
uq_add_test(tst_inkmaterialgpu tst_inkmaterialgpu.cpp)
target_link_libraries(tst_inkmaterialgpu PRIVATE Qt6::GuiPrivate)
```

Прогон:

```
cmake --build build -j$(nproc) --target tst_inkmaterialgpu
ctest --test-dir build -R tst_inkmaterialgpu --output-on-failure
QT_QPA_PLATFORM=wayland ./build/tests/tst_inkmaterialgpu
```

Ожидаемо (проверено): под `ctest` - `SKIP ... RHI недоступен`; под `wayland` -
`QINFO ... RHI OpenGL, Intel ... Iris(R) Xe` и `Totals: 3 passed`. Если подменить
`pxPerUnit` на `dpr` без матрицы, тест падает на зуме 2 («ошибка покрытия 0.05»):
он ловит потерю зума в униформе.

- [ ] 12.10 **Падающие тесты графа сцены: `tests/tst_pagescenegraph.cpp`**

В `private slots:` добавить:

```cpp
    // ink-beauty-pass, D7: сухие и мокрые чернила нарисованы материалом
    // чернил, у мокрого слоя свой тип (иначе он сливается в батч с сухими),
    // куски мокрого штриха не сливаются друг с другом.
    void inkNodesUseInkMaterial()
    {
        Canvas fixture;
        QVERIFY(fixture.open(fixtures::large()));
        DocumentCanvas *canvas = fixture.canvas;

        canvas->goToPage(0);
        QVERIFY(renderUntil(&fixture.window,
                            [canvas] { return !canvas->textureSizeOf(0).isEmpty(); }));
        drawLine(canvas, QPointF(350, 300), QPointF(550, 300));
        renderFrames(&fixture.window, 3);

        // Длинный мокрый штрих - больше одного куска по 512 вершин.
        qreal time = 3.0;
        QCOMPARE(canvas->pageAtView(QPointF(300, 420)), 0);
        QVERIFY(canvas->beginInkAt(QPointF(300, 420), 0.6, false, time));
        for (int i = 1; i <= 400; ++i) {
            time += 1.0 / 240.0;
            canvas->continueInkAt(QPointF(300 + i * 0.9, 420 + 30.0 * std::sin(i / 5.0)), 0.6,
                                  time);
        }
        renderFrames(&fixture.window, 2);
        QVERIFY2(canvas->wetStripVertexCount() > 512,
                 qPrintable(QStringLiteral("полоса %1 не дошла до второго куска")
                                .arg(canvas->wetStripVertexCount())));

        const DocumentCanvas::InkMaterialStats stats = canvas->inkMaterialStats(0);
        QVERIFY(stats.dryNodes > 0);
        QVERIFY(stats.wetNodes > 1);
        QCOMPARE(stats.foreignNodes, 0);
        QVERIFY2(stats.dryAndWetTypesDiffer, "мокрый слой с типом сухого сольётся с ним в батч");
        QVERIFY2(stats.wetChunksSeparate, "куски мокрого штриха сливаются в один батч");

        canvas->endInk();
        renderFrames(&fixture.window, 2);
        QCOMPARE(canvas->inkMaterialStats(0).wetNodes, 0);
        QCOMPARE(canvas->inkMaterialStats(0).foreignNodes, 0);
    }

    // Сценарий «Изменение масштаба» (ink-input, «Сглаживание кромки чернил»):
    // кромка пересчитывается в шейдере, геометрия штрихов не пересобирается.
    void zoomDoesNotRebuildInkGeometry()
    {
        Canvas fixture;
        QVERIFY(fixture.open(fixtures::large()));
        DocumentCanvas *canvas = fixture.canvas;

        canvas->goToPage(0);
        QVERIFY(renderUntil(&fixture.window,
                            [canvas] { return !canvas->textureSizeOf(0).isEmpty(); }));
        drawLine(canvas, QPointF(350, 300), QPointF(550, 320));
        renderFrames(&fixture.window, 3);
        QVERIFY(canvas->inkMaterialStats(0).dryNodes > 0);

        const int rebuilds = canvas->dryInkNodeRebuildCount();
        for (const qreal zoom : {2.0, 0.6, 1.4, 3.0}) {
            canvas->setZoom(zoom);
            renderFrames(&fixture.window, 1);
        }
        QCOMPARE(canvas->dryInkNodeRebuildCount(), rebuilds);
        QVERIFY(canvas->inkMaterialStats(0).dryNodes > 0);
    }
```

Прогон: `cmake --build build --target tst_pagescenegraph` - ожидаемо ошибка: нет
`InkMaterialStats`, `inkMaterialStats`, `dryInkNodeRebuildCount`.

- [ ] 12.11 **`src/canvas/documentcanvas.h`: диагностика материала**

После объявления `int wetStripVertexCount() const ...` добавить:

```cpp
    // Чем нарисованы чернила страницы (design.md ink-beauty-pass, D7). Узел
    // «чужой», если он не на InkMaterial своего слоя или вершина не AxisVertex.
    // Типы сухого и мокрого слоёв обязаны различаться, а куски мокрого штриха -
    // не совпадать по compare(), иначе батч-рендер их сольёт.
    struct InkMaterialStats
    {
        int dryNodes = 0;
        int wetNodes = 0;
        int foreignNodes = 0;
        bool dryAndWetTypesDiffer = true;
        bool wetChunksSeparate = true;
    };
    InkMaterialStats inkMaterialStats(int page) const;
    // Сколько раз узлы сухих чернил пересобирались (страницы и вкладыши). Зум
    // и прокрутка этого числа не меняют.
    int dryInkNodeRebuildCount() const { return m_dryInkNodeRebuilds; }
```

В секцию `private:` рядом с `m_insertNodes` добавить:

```cpp
    // Счётчик пересборок узлов сухих чернил; пишется из updatePaintNode.
    int m_dryInkNodeRebuilds = 0;
```

- [ ] 12.12 **`src/canvas/documentcanvas.cpp`: переход с `QSGFlatColorMaterial` на `InkMaterial`**

Подключения: строку `#include <QSGFlatColorMaterial>` удалить; добавить `#include <cstring>`
и, после `#include "canvas/insertlistmodel.h"`, `#include "canvas/inkmaterial.h"`.

Блок в анонимном пространстве имён от комментария «Размер куска мокрого штриха...» до
конца `uploadStrip` (включая мост из 11.9) заменить:

```cpp
// Размер куска мокрого штриха в вершинах и перекрытие между кусками.
// Перекрытие в две вершины воспроизводит на границе куска все треугольники
// полосы - и сегменты, и веера вокруг центров стыков (повтор центральной
// вершины): треугольник i - это вершины i, i+1, i+2.
constexpr int kWetChunkVertices = 512;
constexpr int kWetChunkOverlap = 2;

static_assert(sizeof(StrokeTessellator::Vertex) == 24, "раскладка InkMaterial::attributes()");

// Узел полосы чернил. batchKey различает куски мокрого штриха: каждый - свой
// батч, на событие пера перезаливается только изменившийся.
QSGGeometryNode *makeStripNode(const QColor &color, InkMaterial::Layer layer, int batchKey = 0)
{
    auto *node = new QSGGeometryNode;

    auto *geometry = new QSGGeometry(InkMaterial::attributes(), 0);
    geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry, true);

    auto *material = new InkMaterial(layer);
    material->setColor(color);
    material->setBatchKey(batchKey);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial, true);

    return node;
}

void uploadStrip(QSGGeometryNode *node, const StrokeTessellator::Vertex *source, int count)
{
    QSGGeometry *geometry = node->geometry();
    geometry->allocate(count);
    std::memcpy(geometry->vertexData(), source, size_t(count) * sizeof(StrokeTessellator::Vertex));
    node->markDirty(QSGNode::DirtyGeometry);
}
```

В `updateDryInkNodes` строку `layer.dryInkIds = ids;` заменить на:

```cpp
    layer.dryInkIds = ids;
    ++m_dryInkNodeRebuilds;
```

и `QSGGeometryNode *strokeNode = makeStripNode(entry.color);` - на
`QSGGeometryNode *strokeNode = makeStripNode(entry.color, InkMaterial::Layer::Dry);`.

В `updateWetInkNodes` строку `QSGGeometryNode *chunk = makeStripNode(m_penColor);` заменить:

```cpp
        // Номер куска - ключ батча: куски не сливаются в один батч.
        QSGGeometryNode *chunk = makeStripNode(m_penColor, InkMaterial::Layer::Wet,
                                               int(layer.wetChunks.size()));
```

После функции `DocumentCanvas::inkLayering` добавить:

```cpp
DocumentCanvas::InkMaterialStats DocumentCanvas::inkMaterialStats(int page) const
{
    InkMaterialStats stats;
    PageSceneNode *node = m_nodes.value(page);
    if (!node)
        return stats;

    QList<QSGMaterialType *> dryTypes;
    const InkMaterial *previousWet = nullptr;
    const auto visit = [&](QSGNode *layer, InkMaterial::Layer expected) {
        if (!layer)
            return;
        const bool wet = expected == InkMaterial::Layer::Wet;
        for (QSGNode *child = layer->firstChild(); child; child = child->nextSibling()) {
            ++(wet ? stats.wetNodes : stats.dryNodes);
            if (child->type() != QSGNode::GeometryNodeType) {
                ++stats.foreignNodes;
                continue;
            }
            auto *geometryNode = static_cast<QSGGeometryNode *>(child);
            const auto *material = dynamic_cast<const InkMaterial *>(geometryNode->material());
            if (!material || material->layer() != expected
                || geometryNode->geometry()->sizeOfVertex() != int(sizeof(AxisVertex))) {
                ++stats.foreignNodes;
                continue;
            }
            if (!wet) {
                dryTypes.append(material->type());
                continue;
            }
            if (dryTypes.contains(material->type()))
                stats.dryAndWetTypesDiffer = false;
            if (previousWet && previousWet->compare(material) == 0)
                stats.wetChunksSeparate = false;
            previousWet = material;
        }
    };
    visit(node->ink.dryInk, InkMaterial::Layer::Dry);
    visit(node->ink.wetInk, InkMaterial::Layer::Wet);
    return stats;
}
```

- [ ] 12.13 **`src/main.cpp`: комментарий о MSAA**

Комментарий над `QSurfaceFormat format = ...` заменить:

```cpp
    // Кромку чернил сглаживает их материал (InkMaterial, design.md
    // ink-beauty-pass, D7), а не мультисэмплинг. MSAA 4x пока остаётся ради
    // остальной сцены - рамок и значков вкладышей, панелей; выключается он
    // отдельным шагом после аудита этих элементов (D8).
```

- [ ] 12.14 **Прогон графа сцены и холста**

```
cmake --build build -j$(nproc) && \
ctest --test-dir build -R 'tst_(pagescenegraph|documentcanvas|canvasink|insertoverlay|inkmaterial)' --output-on-failure
```

Ожидаемо: `100% tests passed`. `tst_pagescenegraph` работает на программном бэкенде: он
узлы с материалом не растеризует, а проверяется структура графа - как и раньше с
`QSGFlatColorMaterial`.

- [ ] 12.15 **Полный прогон и коммит**

```
ctest --test-dir build --output-on-failure
git add CMakeLists.txt src/CMakeLists.txt src/canvas/inkmaterial.h src/canvas/inkmaterial.cpp \
        src/canvas/shaders/ink.vert src/canvas/shaders/ink.frag \
        src/canvas/documentcanvas.h src/canvas/documentcanvas.cpp src/main.cpp \
        README.md packaging/PKGBUILD tests/CMakeLists.txt tests/tst_inkmaterial.cpp \
        tests/tst_inkmaterialgpu.cpp tests/tst_pagescenegraph.cpp
git commit -m "$(cat <<'EOF'
Материал чернил с аналитическим сглаживанием кромки

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
)"
```

Ожидаемо: `100% tests passed` до коммита.

---

## 13. Экспорт: паритет контура `outline()` и полосы на экране

Контур из общих примитивов написан в 11.4: без него группа 11 не собиралась. Здесь -
тесты, которые закрепляют D7 «Экспорт»: растр заливки контура и растр полосы совпадают
везде, кроме полосы ±0.5 px вдоль кромки. Сначала на CPU-растре по формулам материала,
затем через настоящий экспорт и сторонний растеризатор (poppler).

**Files:**
- Create: `tests/support/inkraster.h` (CPU-растеризация полосы по формулам InkMaterial)
- Create: `tests/tst_inkparity.cpp`; Modify: `tests/CMakeLists.txt`
- Modify: `tests/tst_export.cpp` (экспорт против растра полосы)
- Modify: `src/export/noteexporter.cpp` (комментарий)

- [ ] 13.1 **`tests/support/inkraster.h`**

```cpp
#pragma once

// Растеризация полосы AxisVertex на CPU по формулам материала чернил: одна
// точка выборки в центре пикселя, правило top-left на общих рёбрах, атрибуты
// интерполируются аффинно, смешивание over на каждый фрагмент - как GPU без
// MSAA. Тот же приём, что FragmentRaster стенда (tools/stroke-lab/aaraster.h).

#include <QTransform>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <vector>

#include "ink/inkcoverage.h"
#include "ink/stroketessellator.h"

namespace inkraster {

struct Coverage
{
    int width = 0;
    int height = 0;
    std::vector<float> alpha;

    float at(int x, int y) const { return alpha[size_t(y) * size_t(width) + size_t(x)]; }
};

// pageToDevice - подобие без отражения: масштаб и сдвиг.
inline Coverage rasterize(const QVector<AxisVertex> &strip, const QTransform &pageToDevice,
                          int width, int height,
                          qreal minHalfWidthPx = ink::kDefaultMinHalfWidthPx)
{
    Coverage out{width, height, std::vector<float>(size_t(width) * size_t(height), 0.0f)};
    const qreal pxPerUnit = std::sqrt(std::abs(pageToDevice.determinant()));

    struct DeviceVertex
    {
        QPointF p;
        qreal d;
        qreal hw;
    };
    std::vector<DeviceVertex> v;
    v.reserve(size_t(strip.size()));
    for (const AxisVertex &a : strip) {
        const ink::DeviceVertex dv = ink::toDevice(a, pxPerUnit, minHalfWidthPx);
        v.push_back({pageToDevice.map(dv.position), dv.distance, dv.halfWidth});
    }

    const auto edge = [](const QPointF &p, const QPointF &q, double x, double y) {
        return (q.x() - p.x()) * (y - p.y()) - (q.y() - p.y()) * (x - p.x());
    };
    // Общее ребро двух согласованно ориентированных треугольников проходится
    // в противоположные стороны и достаётся одному из них.
    const auto owns = [](const QPointF &p, const QPointF &q) {
        const double dy = q.y() - p.y();
        const double dx = q.x() - p.x();
        return dy < 0 || (dy == 0 && dx > 0);
    };

    for (size_t i = 0; i + 2 < v.size(); ++i) {
        const DeviceVertex *a = &v[i];
        const DeviceVertex *b = &v[i + 1];
        const DeviceVertex *c = &v[i + 2];
        double area = edge(a->p, b->p, c->p.x(), c->p.y());
        if (!std::isfinite(area) || std::abs(area) < 1e-12)
            continue; // вырожденный треугольник GPU не рисует
        if (area < 0) {
            std::swap(b, c);
            area = -area;
        }
        const int x0 = std::max(0, int(std::floor(std::min({a->p.x(), b->p.x(), c->p.x()}) - 0.5)));
        const int x1 = std::min(width - 1, int(std::ceil(std::max({a->p.x(), b->p.x(), c->p.x()}) - 0.5)));
        const int y0 = std::max(0, int(std::floor(std::min({a->p.y(), b->p.y(), c->p.y()}) - 0.5)));
        const int y1 = std::min(height - 1, int(std::ceil(std::max({a->p.y(), b->p.y(), c->p.y()}) - 0.5)));
        const bool o0 = owns(b->p, c->p);
        const bool o1 = owns(c->p, a->p);
        const bool o2 = owns(a->p, b->p);
        for (int y = y0; y <= y1; ++y) {
            const double sy = y + 0.5;
            for (int x = x0; x <= x1; ++x) {
                const double sx = x + 0.5;
                const double w0 = edge(b->p, c->p, sx, sy);
                const double w1 = edge(c->p, a->p, sx, sy);
                const double w2 = edge(a->p, b->p, sx, sy);
                if (w0 < 0 || w1 < 0 || w2 < 0)
                    continue;
                if ((w0 == 0 && !o0) || (w1 == 0 && !o1) || (w2 == 0 && !o2))
                    continue;
                const double d = (w0 * a->d + w1 * b->d + w2 * c->d) / area;
                const double hw = (w0 * a->hw + w1 * b->hw + w2 * c->hw) / area;
                const float cov = float(ink::coverage(d, hw));
                float &dst = out.alpha[size_t(y) * size_t(width) + size_t(x)];
                dst = dst + cov * (1.0f - dst);
            }
        }
    }
    return out;
}

} // namespace inkraster
```

- [ ] 13.2 **Тест паритета: `tests/tst_inkparity.cpp`**

```cpp
// ink-beauty-pass, D7, «Экспорт»: контур outline() и полоса на экране - одна
// фигура. Растр заливки контура (выборка в центре пикселя, ненулевой обход) и
// растр полосы по формулам материала чернил совпадают везде, кроме полосы
// ±0.5 px вдоль кромки контура.

#include <QPainterPath>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <vector>

#include "ink/stroketessellator.h"
#include "support/inkraster.h"
#include "support/referencestrokes.h"

namespace {

constexpr qreal kBandPx = 0.5;
constexpr float kTolerance = 0.02f;

qreal distanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const qreal lengthSquared = QPointF::dotProduct(ab, ab);
    const qreal t = lengthSquared < 1e-18
        ? 0.0
        : std::clamp(QPointF::dotProduct(p - a, ab) / lengthSquared, 0.0, 1.0);
    const QPointF q = a + ab * t;
    return std::hypot(p.x() - q.x(), p.y() - q.y());
}

struct Parity
{
    int compared = 0;
    int mismatches = 0;
    QString first;
};

Parity compareAt(const QList<InkPoint> &points, const StrokeTessellator &tessellator,
                 qreal pxPerUnit)
{
    Parity result;
    const QPolygonF outline = tessellator.outline(points);
    const QVector<AxisVertex> strip = tessellator.tessellate(points);
    if (outline.isEmpty() || StrokeTessellator::isDegenerate(strip))
        return result;

    QPainterPath path;
    path.addPolygon(outline);
    path.closeSubpath();
    path.setFillRule(Qt::WindingFill);

    // Поле вокруг фигуры: бахрома 1 px и запас.
    const qreal margin = 3.0 / pxPerUnit;
    const QRectF bounds = path.boundingRect().adjusted(-margin, -margin, margin, margin);
    const QTransform toDevice =
        QTransform::fromScale(pxPerUnit, pxPerUnit).translate(-bounds.left(), -bounds.top());
    const int width = int(std::ceil(bounds.width() * pxPerUnit));
    const int height = int(std::ceil(bounds.height() * pxPerUnit));
    const inkraster::Coverage coverage = inkraster::rasterize(strip, toDevice, width, height);

    // Рёбра контура в device px, разложенные по строкам с запасом на полосу.
    const QPolygonF device = toDevice.map(outline);
    std::vector<std::vector<int>> rows(static_cast<size_t>(height));
    for (int e = 0; e + 1 < device.size(); ++e) {
        const qreal top = std::min(device[e].y(), device[e + 1].y()) - kBandPx - 1.0;
        const qreal bottom = std::max(device[e].y(), device[e + 1].y()) + kBandPx + 1.0;
        for (int y = std::max(0, int(std::floor(top))); y <= std::min(height - 1, int(bottom)); ++y)
            rows[size_t(y)].push_back(e);
    }

    const QTransform toPage = toDevice.inverted();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const QPointF center(x + 0.5, y + 0.5);
            bool inBand = false;
            for (const int e : rows[size_t(y)]) {
                if (distanceToSegment(center, device[e], device[e + 1]) <= kBandPx) {
                    inBand = true;
                    break;
                }
            }
            if (inBand)
                continue;
            ++result.compared;
            const float fill = path.contains(toPage.map(center)) ? 1.0f : 0.0f;
            const float strip = coverage.at(x, y);
            if (std::abs(strip - fill) > kTolerance) {
                if (result.mismatches == 0)
                    result.first = QStringLiteral("пиксель (%1, %2): полоса %3, контур %4")
                                       .arg(x)
                                       .arg(y)
                                       .arg(strip)
                                       .arg(fill);
                ++result.mismatches;
            }
        }
    }
    return result;
}

QList<InkPoint> withPressure(const QList<QPointF> &path, qreal pressure)
{
    QList<InkPoint> points;
    for (int i = 0; i < path.size(); ++i)
        points.append({path[i], pressure, i / 200.0});
    return points;
}

QList<InkPoint> wave()
{
    QList<InkPoint> points;
    for (int i = 0; i <= 80; ++i) {
        const qreal t = qreal(i) / 80.0;
        points.append({QPointF(20 + t * 200, 60 + std::sin(t * 4.0 * M_PI) * 20),
                       0.15 + 0.85 * std::sin(t * M_PI), i / 200.0});
    }
    return points;
}

} // namespace

class TestInkParity : public QObject
{
    Q_OBJECT

private:
    void check(const char *label, const QList<InkPoint> &points, qreal width)
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(width);
        for (const qreal pxPerUnit : {1.0, 2.5}) {
            const Parity parity = compareAt(points, tessellator, pxPerUnit);
            QVERIFY2(parity.compared > 100,
                     qPrintable(QStringLiteral("%1: сравнивать нечего").arg(QLatin1String(label))));
            QVERIFY2(parity.mismatches == 0,
                     qPrintable(QStringLiteral("%1 x%2: %3 расхождений из %4, первое - %5")
                                    .arg(QLatin1String(label))
                                    .arg(pxPerUnit)
                                    .arg(parity.mismatches)
                                    .arg(parity.compared)
                                    .arg(parity.first)));
        }
    }

private slots:
    // Переменная толщина от давления, пологие стыки, оба торца.
    void waveWithPressure() { check("wave", wave(), 6.0); }

    void loop() { check("loop", withPressure(reference::loop(), 0.7), 4.0); }

    void letterE() { check("letter e", withPressure(reference::letterE(), 0.8), 3.0); }

    // Петля радиусом меньше полуширины: работает предел по кривизне.
    void tightLoopUnderCurvatureLimit()
    {
        check("tight loop",
              withPressure(reference::circle(2.0, 2, QPointF(30, 30), 360), 1.0), 6.0);
    }

    // Касание без сдвига - диск из двух полувееров (D5).
    void tapDisk()
    {
        check("tap", {{QPointF(40, 40), 0.8, 0.0}, {QPointF(40, 40), 0.8, 0.01}}, 5.0);
    }
};

QTEST_MAIN(TestInkParity)
#include "tst_inkparity.moc"
```

В `tests/CMakeLists.txt` после `uq_add_test(tst_inkmaterial ...)` и его
`target_link_libraries` добавить:

```cmake
uq_add_test(tst_inkparity tst_inkparity.cpp)
```

- [ ] 13.3 **Прогон паритета**

```
cmake -S . -B build && cmake --build build -j$(nproc) --target tst_inkparity && \
ctest --test-dir build -R tst_inkparity --output-on-failure
```

Ожидаемо: `100% tests passed` (на копии - `Totals: 7 passed`, 0.2 с; если в одной
дуге торца контура сдвинуть вершину на оси на полрадиуса, падают `waveWithPressure`,
`letterE`, `tapDisk`). Если расхождение есть, первый пиксель в сообщении
показывает, какой примитив контура не совпал с полосой: торец, сторона,
биссектриса или дуга. Чинится `outline()` (11.4), а не тест: эталон - полоса, её
проверил гейт и стенд (11.14). Острые углы (`sharpZigzag`, hairpin) в паритет не
входят намеренно. Там вершина на биссектрисе в полосе выносится на `hw + 1 px`, а
внутренний угол контура лежит на `hw / cos(θ/2)`; область между ними на экране
закрывают соседние сегменты, поэтому расхождение доходит до пикселя только при
повороте больше примерно 140° у тонкой линии.

- [ ] 13.4 **Тест экспорта против растра полосы (закрепляющий): `tests/tst_export.cpp`**

К подключениям добавить `#include "ink/stroketessellator.h"` и `#include "support/inkraster.h"`.
В `private slots:` добавить:

```cpp
    // ink-beauty-pass, D7, «Экспорт»: чернила в PDF - та же фигура, что на
    // экране. Сторонний растеризатор (pdftoppm, 144 dpi = 2 px на точку) против
    // растра полосы по формулам материала: внутри и снаружи штриха, вдали от
    // кромки, ответы совпадают.
    void exportedInkMatchesScreenStrip()
    {
        PdfDocumentModel document;
        QVERIFY(document.open(fixtures::noOutline()));
        const Prepared prepared = prepare(document);
        QCOMPARE(prepared.rotations.first(), 0);

        Stroke stroke = wave(0, 300, 8.0);
        stroke.color = QColor(220, 20, 30);
        InkDocument ink;
        ink.addStroke(stroke);

        const QString output = m_dir.filePath(QStringLiteral("parity.pdf"));
        QString error;
        QVERIFY2(NoteExporter::exportNow(fixtures::noOutline(), prepared.sizes,
                                         prepared.rotations, ink.allStrokes(), output, &error),
                 qPrintable(error));

        const QString png = m_dir.filePath(QStringLiteral("parity"));
        QProcess render;
        render.start(QStringLiteral("pdftoppm"),
                     {QStringLiteral("-f"), QStringLiteral("1"), QStringLiteral("-l"),
                      QStringLiteral("1"), QStringLiteral("-r"), QStringLiteral("144"),
                      QStringLiteral("-png"), output, png});
        QVERIFY(render.waitForFinished(60000));
        const QFileInfo root(png);
        const QStringList produced = QDir(root.absolutePath())
                                         .entryList({root.fileName() + QStringLiteral("-*.png")},
                                                    QDir::Files);
        QVERIFY(!produced.isEmpty());
        const QImage image(QDir(root.absolutePath()).filePath(produced.first()));
        QVERIFY(!image.isNull());

        StrokeTessellator tessellator;
        tessellator.setWidth(stroke.width);
        tessellator.setStyle(stroke.style);
        const inkraster::Coverage strip =
            inkraster::rasterize(tessellator.tessellate(stroke.points),
                                 QTransform::fromScale(2.0, 2.0), image.width(), image.height());

        // Сравниваются только пиксели, у которых вся окрестность 3×3 у полосы
        // либо сплошь залита, либо пуста: сглаживание poppler и кромка не в счёт.
        const auto solid = [&strip](int x, int y, bool inked) {
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const float a = strip.at(x + dx, y + dy);
                    if (inked ? a < 0.98f : a > 0.02f)
                        return false;
                }
            return true;
        };
        int inside = 0;
        int outside = 0;
        int mismatches = 0;
        QString first;
        const QRect area = QRect(QPoint(2 * 40, 2 * 220), QPoint(2 * 540, 2 * 380))
                               .intersected(image.rect().adjusted(1, 1, -1, -1));
        for (int y = area.top(); y <= area.bottom(); ++y) {
            for (int x = area.left(); x <= area.right(); ++x) {
                const bool inked = solid(x, y, true);
                if (!inked && !solid(x, y, false))
                    continue;
                const QColor pixel = image.pixelColor(x, y);
                const bool red = pixel.red() > 150 && pixel.green() < 110 && pixel.blue() < 110;
                ++(inked ? inside : outside);
                if (red != inked) {
                    if (mismatches++ == 0)
                        first = QStringLiteral("(%1, %2): полоса %3, PDF %4")
                                    .arg(x)
                                    .arg(y)
                                    .arg(inked ? QStringLiteral("чернила") : QStringLiteral("фон"))
                                    .arg(red ? QStringLiteral("чернила") : QStringLiteral("фон"));
                }
            }
        }
        QVERIFY(inside > 500 && outside > 500);
        QVERIFY2(mismatches == 0, qPrintable(QStringLiteral("%1 расхождений, первое %2")
                                                 .arg(mismatches)
                                                 .arg(first)));
    }
```

Рамка `area` - область волны `wave(0, 300)`: x 60…520, y 250…350 в точках страницы, с
запасом. Тест проходит и на текущем контуре (11.4): он закрепляет паритет, а не
вводит его. Красный провал до реализации здесь невозможен. Проверить, что тест что-то
ловит: временно вернуть в `outline()` торцы плоской парой (заменить вызовы `capArc` на
одну точку `c ± n·hw`), убедиться в провале и вернуть.

- [ ] 13.5 **Прогон экспорта**

```
cmake --build build -j$(nproc) --target tst_export && \
ctest --test-dir build -R 'tst_(export|inkparity)' --output-on-failure
```

Ожидаемо: `100% tests passed` (нужны `pdftoppm` и `qpdf` - как и остальным тестам экспорта).

- [ ] 13.6 **`src/export/noteexporter.cpp`: комментарий о контуре**

Комментарий над `path.setFillRule(Qt::WindingFill);` заменить:

```cpp
                    // Контур - те же примитивы, что полоса на экране: дуги
                    // торцов, стороны, внешние дуги стыков (design.md
                    // ink-beauty-pass, D7). Заливка по ненулевому обходу: на
                    // петлях контур сам себя перекрывает, и even-odd оставил бы
                    // дырки.
```

Перед `outline()` в цикле должен стоять `tessellator.setStyle(record->stroke.style);` -
его ставит шаг 1 (контракт). Проверить: `grep -n setStyle src/export/noteexporter.cpp`;
ожидаемо одна строка.

- [ ] 13.7 **Полный прогон и коммит**

```
ctest --test-dir build --output-on-failure
git add tests/support/inkraster.h tests/tst_inkparity.cpp tests/tst_export.cpp \
        tests/CMakeLists.txt src/export/noteexporter.cpp
git commit -m "$(cat <<'EOF'
Паритет контура экспорта и полосы на экране

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
)"
```

---

## 14. Проверки на устройстве, версия 0.10.0

Устройство - Samsung Galaxy Book, Mesa Intel LNL, 120 Гц, KDE/kwin_wayland (PERFORMANCE.md).
Документ - тот же, что в PERFORMANCE.md: 200+ страниц с чернилами на каждой странице
(`kb.pdf` с его заметкой). Сборка на устройстве - `RelWithDebInfo`, как в контракте.

**Files:**
- Modify: `CMakeLists.txt` (версия), `packaging/PKGBUILD` (`pkgver`)
- Modify: `PERFORMANCE.md` (раздел о материале чернил)
- Modify: `tools/stroke-lab/README.md` (app-ink, baseline после шага 3)

- [ ] 14.1 **Собрать на устройстве обе версии для сравнения**

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUQ_BUILD_STROKE_LAB=ON
cmake --build build -j$(nproc)
git worktree add /tmp/uq-0.9 $(git log --format=%H -1 --grep='версия 0.9.0')
cmake -S /tmp/uq-0.9 -B /tmp/uq-0.9/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUQ_BUILD_TESTS=OFF
cmake --build /tmp/uq-0.9/build -j$(nproc)
```

Ожидаемо: два бинарника, `./build/src/uq-notes` и `/tmp/uq-0.9/build/src/uq-notes`.

- [ ] 14.2 **Батчинг: мокрый штрих не перезаливает страницу**

```
# Без QT_FORCE_STDERR_LOGGING Qt пишет отладку в журнал, когда stderr не терминал,
# и файл остаётся пустым (проверено).
QT_FORCE_STDERR_LOGGING=1 QSG_RENDERER_DEBUG=upload UQ_DEMO_INK=20 ./build/src/uq-notes ~/kb.pdf 2> /tmp/upload.log
awk '
/Uploading Opaque Batches:/ { alpha = 0; next }
/Uploading Alpha Batches:/  { if (frames++) print batches, bytes; alpha = 1; batches = 0; bytes = 0; next }
alpha && / - batch .* vbo:/ { n = split($0, f, ":"); batches++; bytes += f[n] + 0 }
END { print batches, bytes }
' /tmp/upload.log > /tmp/upload-frames.txt
wc -l < /tmp/upload-frames.txt
for part in head tail; do
  $part -n 1200 /tmp/upload-frames.txt | awk '{print $2}' | sort -n |
    awk -v p=$part '{a[NR]=$1} END {print p, "median", a[int(NR/2)+1], "p95", a[int(NR*0.95)]}'
done
```

`/tmp/upload-frames.txt` - на кадр «число перезалитых батчей чернил и вершинных байт».
Строка батча в журнале выглядит так: ` - batch 0x... first: ... root: ... merged: 1
positionAttribute 0  vbo: 0x0 : 297920`; разбор берёт число после последнего `:`.
Ожидаемо:
- медиана байт на кадр ≤ 16384: перезаливается один кусок мокрого штриха (512 × 24 =
  12288 байт), батчи сухих чернил печатают `already uploaded`;
- p95 первых и последних 1200 кадров (10 с) одного порядка: перезаливка не растёт с
  числом штрихов на странице.

Всплески на отрыве пера (новый сухой штрих перестраивает батч страницы) и на
добавлении куска - ожидаемы, они реже одного кадра из ста.

При провале (медиана - сотни КБ или МБ, т.е. батч страницы на каждом кадре) проверить
`type()` и `compare()` мокрого материала, `BATCHABLE` в `qt_add_shaders` и
`tst_pagescenegraph::inkNodesUseInkMaterial`. Дальше не идти.

- [ ] 14.3 **Бюджет кадра: минута письма на 200 страницах**

Четыре прогона подряд, чередуя сборки, на батарее с регулятором `powersave`
(PERFORMANCE.md):

```
for build in ./build /tmp/uq-0.9/build ./build /tmp/uq-0.9/build; do
  UQ_DEMO_INK=60 UQ_FRAME_LOG=1 $build/src/uq-notes ~/kb.pdf | tail -n 2 | tee -a /tmp/ink-frames.txt
done
```

Последние две строки `[frames] refresh ... | median ... | p99 ... | long frames ...` каждого
прогона - его итог. Критерии для 0.10.0: в каждом прогоне `p99 ≤ 12.6` мс, медиана
8.3x мс, длинных кадров после первых 2 с не больше, чем у 0.9.0 в соседнем прогоне,
плюс два. При провале - вернуться к пользователю с `/tmp/ink-frames.txt` и
`/tmp/upload-frames.txt`. Выключение MSAA (D8) - отдельный шаг 4, в этом шаге его не
делать.

- [ ] 14.4 **Зум и прокрутка: геометрия не пересобирается, кадр в бюджете**

```
UQ_DEMO_NAV=30 UQ_FRAME_LOG=1 ./build/src/uq-notes ~/kb.pdf | tail -n 2
```

Ожидаемо: p99 и число длинных кадров не хуже записанных в PERFORMANCE.md для
`UQ_DEMO_NAV` (раздел о зоне удержания); зум меняет только униформы
(`tst_pagescenegraph::zoomDoesNotRebuildInkGeometry`).

- [ ] 14.5 **Память VBO**

Во время прогона 14.4 снять пик RSS для обеих сборок:

```
UQ_DEMO_NAV=30 ./build/src/uq-notes ~/kb.pdf & pid=$!; peak=0
while kill -0 $pid 2>/dev/null; do r=$(awk '/VmHWM/{print $2}' /proc/$pid/status 2>/dev/null); [ -n "$r" ] && peak=$r; sleep 1; done
echo "пик RSS ${peak} КБ"
```

(то же с `/tmp/uq-0.9/build/src/uq-notes`). Ожидаемо: рост не больше 100 МБ (D7: VBO
плотной страницы 8.7 → 25 МБ, удерживается только зона удержания). Больше - вернуться
к пользователю: упаковка вершины в half (16 байт) в D7 вынесена за это изменение.

- [ ] 14.6 **Сценарии спеки «Сглаживание кромки чернил» глазами**

На устройстве, в собранной 0.10.0:
0. `QT_QPA_PLATFORM=wayland ./build/tests/tst_inkmaterialgpu` - `Totals: 3 passed` на GPU
   устройства (12.9).
1. Перо 1 pt, масштаб 100%: слово «minimum» и «привет» - линия сплошная, без тёмных
   точек на стыках и бледных провалов.
2. Самое тонкое перо (0.4 pt), масштаб 50%: линия не распадается на точки.
3. Медленная прокрутка страницы с чернилами: кромка не мерцает.
4. Зум 25% → 800% по странице с чернилами: кромка гладкая на каждом масштабе.
5. Красное и зелёное перо поверх тёмного содержимого PDF: чернила своего цвета, не
   гаснут до фона.
6. Мокрый и нанесённый штрих: в момент отрыва линия не меняется.

Снимок для записи (масштаб 100% и 250%):

```
UQ_GOTO_PAGE=0 UQ_SCREENSHOT=/tmp/ink-0.10.png UQ_SCREENSHOT_DELAY_MS=2500 ./build/src/uq-notes ~/kb.uqn
```

Если какой-то сценарий не выполнен - вернуться к пользователю со снимком. Значение пола
(0.15 → 0.3 px) - Open Question design.md, решение пользователя.

- [ ] 14.7 **Сравнение со стендом: эффект D7 воспроизводится**

```
./build/tools/stroke-lab/stroke-lab --variants baseline,app-ink --dpr 1.75 --out /tmp/lab-0.10 \
  > /tmp/lab-0.10.md
grep -E 'baseline|app-ink' /tmp/lab-0.10/summary.md
```

Ожидаемо: у `app-ink` против `baseline` (полоса под MSAA 4x) ошибка кромки
(`aa_rmse_shape`, если метрика есть в стенде шага 0, иначе `raster_vs_ideal_rms`)
заметно ниже. В D7 при dpr 1: 27.6 → 11.3 из 255 на масштабе 1, мерцание 18.8 → 5.4. Числа записать
в PERFORMANCE.md (14.8). Если `app-ink` не лучше `baseline` - вернуться к пользователю:
в приложении и прототипе расходится шейдер или стык.

- [ ] 14.8 **PERFORMANCE.md, README стенда, версия**

`CMakeLists.txt`: `project(uq-notes VERSION 0.9.0 LANGUAGES CXX)` → `project(uq-notes VERSION 0.10.0 LANGUAGES CXX)`.
`packaging/PKGBUILD`: `pkgver=0.9.0` → `pkgver=0.10.0`.

`PERFORMANCE.md`, в конец, раздел (‹…› - числа из 14.2-14.5 и 14.7):

```
## Материал чернил (`ink-beauty-pass`, D7)

Кромку чернил сглаживает материал (`src/canvas/inkmaterial.*`): вершина несёт
ось стыка, шейдер выносит её на 1 device px за кромку и считает точное покрытие
пиксельным боксом. Вершина 24 байта вместо 8, вершин на 18% больше. MSAA 4x пока
остаётся ради остальной сцены (D8).

**Письмо, 200 страниц.** `UQ_DEMO_INK=60 UQ_FRAME_LOG=1`, `kb.pdf` с заметкой,
четыре прогона, сборки чередуются:

| Показатель | 0.10.0 | 0.9.0 |
| --- | --- | --- |
| медиана интервала | ‹› мс | ‹› мс |
| p99 | ‹› мс | ‹› мс |
| длинных кадров после 2 с | ‹› | ‹› |
| латентность, максимум | ‹› мс | ‹› мс |

**Перезаливка во время письма.** `QSG_RENDERER_DEBUG=upload`, 20 секунд: медиана
‹› байт вершин на кадр (один кусок мокрого штриха), p95 ‹› - одинаково в начале и
в конце прогона. Сухие батчи страницы не перезаливаются: у мокрого слоя свой тип
материала, у кусков мокрого штриха свой ключ батча.

**Память.** Пик RSS при `UQ_DEMO_NAV=30`: ‹› МБ против ‹› МБ у 0.9.0.

**Стенд.** `stroke-lab --variants baseline,app-ink --dpr 1.75`: ошибка кромки ‹› →
‹›, мерцание ‹› → ‹›, «бусины» ‹› → ‹›.
```

`tools/stroke-lab/README.md`, к описанию `baseline` добавить:

```
После шага 3 (`ink-beauty-pass`, D7) приложение на стенде - вариант `app-ink`:
полоса `AxisVertex` из `StrokeTessellator` и покрытие по формулам `InkMaterial`
(`src/ink/inkcoverage.h`). `baseline` остаётся той же полосой в покое под
эмуляцией MSAA 4x - прежней отрисовкой, для сравнения.
```

- [ ] 14.9 **Полный прогон и финальный коммит шага**

```
cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure
git worktree remove /tmp/uq-0.9
git add CMakeLists.txt packaging/PKGBUILD PERFORMANCE.md tools/stroke-lab/README.md
git commit -m "$(cat <<'EOF'
Аналитическое сглаживание кромки чернил, версия 0.10.0

Кромка линии больше не зависит от мультисэмплинга. Раньше полосу штриха
сглаживал MSAA 4x на всё окно: пять уровней покрытия на пиксель, пятна и
пунктир у тонкого пера, мерцание кромки при медленной прокрутке. Теперь
у чернил свой материал: вершина несёт ось стыка - центр, направление
выноса, полуширину и сторону, - шейдер выносит её на один device px за
кромку и считает точную долю пикселя, которую покрывает линия. Ошибка
кромки на стенде ‹› → ‹› из 255, мерцание при субпиксельном сдвиге ‹› →
‹›. Тонкое перо больше не распадается на бусины: масса чернил поперёк
линии равна её толщине при любой толщине, а пол в 0.15 device px держит
самое тонкое перо видимым на отдалённом зуме.

Стыки переписаны. Внутренняя вершина знает своё настоящее расстояние до
обоих сегментов, внешняя дуга - веер вокруг центра стыка, а не из
внутренней точки: перенос прежнего стыка под бахрому давал на «M» вдвое
большую ошибку. Торцы - полувеер вокруг точки вместо пар шага 2. На тугих
петлях внутренний вынос ограничен радиусом кривизны осевой: без предела
бахрома складывалась, и петля «e» при dpr 2 бледнела. Предел сначала прошёл
гейт на стенде (tools/stroke-lab/gate-rho.sh) и только потом попал в
приложение. Радиус хранится длиной направления, поэтому вершина осталась
в 24 байтах.

Батчинг сохранён. Сухие штрихи страницы одного цвета сливаются в батч,
как раньше. У мокрого слоя свой тип материала, у каждого куска мокрого
штриха свой ключ батча: на событие пера перезаливается один кусок в 12 КБ,
а не VBO страницы. На устройстве ‹› байт на кадр в медиане, минута письма
по документу в 200 страниц - p99 кадра ‹› мс при пороге 12.6.

Экспорт рисует ту же фигуру. Контур строится из тех же примитивов, что
полоса: дуги торцов, стороны, внешние дуги стыков. Тест паритета сверяет
заливку контура с растром полосы по формулам материала, второй - сам PDF
через poppler. Зум меняет только униформы материала, геометрия штрихов не
пересобирается - это тоже закреплено тестом графа сцены.

MSAA 4x пока остаётся: он сглаживает рамки и значки вкладышей и панели.
Его выключение - следующий шаг, после аудита этих элементов. Для сборки
теперь нужен qt6-shadertools, он добавлен в README и PKGBUILD. Версия
поднята до 0.10.0 в CMakeLists.txt и PKGBUILD.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
)"
```

Числа ‹…› в сообщении - из PERFORMANCE.md (14.8), до коммита вписать их на место.

---

## Что раздел НЕ покрывает

- Выключение MSAA и аудит элементов окна (D8, шаг 4): в этом шаге `format.setSamples(4)`
  остаётся.
- Упаковку вершины в half до 16 байт (D7, «Цена»): только если 14.5 покажет, что память
  стала ограничением, и отдельным изменением.
- Запасной путь при провале гейта 10 (`outline-sdf`, слой «один раз на штрих»): решение
  пользователя.
- Маркер и полупрозрачные чернила: over кладёт альфу на каждый фрагмент, на
  самоперекрытиях штрих темнее (Non-Goals).
- Пиксельную проверку шейдера в `ctest` без GPU: `tst_inkmaterialgpu` под `offscreen`
  сообщает SKIP, на GPU его запускают руками (12.9, 14.6). Формулы проверяются
  CPU-зеркалом (`tst_inkcoverage`, `tst_inkparity`), раскладка `.qsb` - отражением
  (`tst_inkmaterial`). `aa-gpu-bench` стенда гоняет шейдеры прототипа, а не
  `src/canvas/shaders/`.
- Разделение типов сухого и мокрого слоя и ключи кусков проверены только структурно
  (`tst_pagescenegraph`, программный бэкенд) и по журналу на устройстве (14.2). На
  настоящем батч-рендере проверено слияние полос `InkMaterial` и неперезаливка при
  зуме (12.9), но не поведение мокрого слоя.
- Программный бэкенд Qt Quick (`QT_QUICK_BACKEND=software`): узлы с материалом он не
  рисует - так было и с `QSGFlatColorMaterial`.
- Границы узлов для проверки перекрытий в батч-рендере: Qt берёт их по атрибуту-позиции,
  то есть по центрам стыков, без полуширины и бахромы. У разноцветных штрихов, которые
  почти касаются, это может поменять порядок наложения частично покрытых пикселей
  кромки. Это незаметно, и это не чинится.
- Острые углы в тесте паритета (13.3): у тонкой линии при повороте больше ~140°
  внутренний угол контура экспорта может быть на пиксель глубже кромки на экране.


# Шаг 4: MSAA

## 15. Шаг 4: MSAA выключается (D8)

После группы 14 кромку чернил сглаживает `InkMaterial`, и MSAA чернилам больше
не нужен. Но он сглаживает всю сцену, поэтому сначала выбор числа сэмплов
выносится в проверяемую функцию с переключателем `UQ_MSAA`, затем проходит
аудит остальных элементов окна и замер GPU, и только потом значение по
умолчанию меняется на 0.

**Files:**
- Create: `src/canvas/surfaceformat.h`, `src/canvas/surfaceformat.cpp`
- Create: `tests/tst_surfaceformat.cpp`
- Modify: `src/CMakeLists.txt` (список SOURCES модуля)
- Modify: `tests/CMakeLists.txt`
- Modify: `src/main.cpp:17-22`
- Modify: `PERFORMANCE.md`, `README.md`
- Modify: `CMakeLists.txt:2`, `packaging/PKGBUILD` (версия 0.11.0)

- [ ] 15.1 **Написать падающий тест выбора числа сэмплов**

Создать `tests/tst_surfaceformat.cpp`:

```cpp
#include <QtTest>

#include "canvas/surfaceformat.h"

// Число сэмплов мультисэмплинга окна (design.md, D8). Кромку чернил
// сглаживает InkMaterial, поэтому по умолчанию MSAA выключен; UQ_MSAA
// возвращает его для сравнения на устройстве.
class TestSurfaceFormat : public QObject
{
    Q_OBJECT

private slots:
    void defaultIsNoMultisampling()
    {
        QCOMPARE(surfaceSampleCount(QByteArray()), 0);
    }

    void environmentRestoresMultisampling_data()
    {
        QTest::addColumn<QByteArray>("value");
        QTest::addColumn<int>("expected");
        QTest::newRow("4") << QByteArray("4") << 4;
        QTest::newRow("8") << QByteArray("8") << 8;
        QTest::newRow("0") << QByteArray("0") << 0;
        QTest::newRow("пробелы") << QByteArray(" 4 ") << 4;
    }

    void environmentRestoresMultisampling()
    {
        QFETCH(QByteArray, value);
        QFETCH(int, expected);
        QCOMPARE(surfaceSampleCount(value), expected);
    }

    // Мусор в переменной не должен ни ронять запуск, ни включать странное
    // число сэмплов: тогда действует значение по умолчанию.
    void invalidValueFallsBackToDefault_data()
    {
        QTest::addColumn<QByteArray>("value");
        QTest::newRow("текст") << QByteArray("yes");
        QTest::newRow("отрицательное") << QByteArray("-4");
        QTest::newRow("не степень двойки") << QByteArray("3");
        QTest::newRow("слишком много") << QByteArray("32");
    }

    void invalidValueFallsBackToDefault()
    {
        QFETCH(QByteArray, value);
        QCOMPARE(surfaceSampleCount(value), 0);
    }
};

QTEST_GUILESS_MAIN(TestSurfaceFormat)
#include "tst_surfaceformat.moc"
```

В `tests/CMakeLists.txt` после строки `uq_add_test(tst_documentcanvas tst_documentcanvas.cpp)` добавить:

```cmake
uq_add_test(tst_surfaceformat tst_surfaceformat.cpp)
```

- [ ] 15.2 **Убедиться, что тест не собирается**

Run: `cmake --build build -j$(nproc) --target tst_surfaceformat`
Expected: ошибка компиляции `canvas/surfaceformat.h: No such file or directory`.

- [ ] 15.3 **Написать функцию выбора числа сэмплов**

Создать `src/canvas/surfaceformat.h`:

```cpp
#pragma once

#include <QByteArray>

// Число сэмплов мультисэмплинга окна. Кромку чернил сглаживает InkMaterial по
// точной доле покрытия пикселя, поэтому MSAA по умолчанию выключен: на зуме он
// стоил половину времени GPU плотной страницы (design.md, D8). Значение
// переменной UQ_MSAA (0, 2, 4, 8 или 16) возвращает его для сравнения на
// устройстве; всё остальное означает значение по умолчанию.
int surfaceSampleCount(const QByteArray &environmentValue);
```

Создать `src/canvas/surfaceformat.cpp`:

```cpp
#include "surfaceformat.h"

namespace {

constexpr int kDefaultSamples = 0;

} // namespace

int surfaceSampleCount(const QByteArray &environmentValue)
{
    bool ok = false;
    const int samples = environmentValue.trimmed().toInt(&ok);
    if (!ok)
        return kDefaultSamples;
    switch (samples) {
    case 0:
    case 2:
    case 4:
    case 8:
    case 16:
        return samples;
    default:
        return kDefaultSamples;
    }
}
```

В `src/CMakeLists.txt` в список `SOURCES` модуля после строки
`canvas/insertlistmodel.h` добавить:

```cmake
        canvas/surfaceformat.cpp
        canvas/surfaceformat.h
```

- [ ] 15.4 **Прогнать тест**

Run: `cmake --build build -j$(nproc) --target tst_surfaceformat && ctest --test-dir build -R tst_surfaceformat --output-on-failure`
Expected: `100% tests passed`.

- [ ] 15.5 **Подключить функцию в main.cpp, пока со значением по умолчанию 4**

Аудит в 15.7 сравнивает оба режима одной сборкой, поэтому сначала значение по
умолчанию временно остаётся прежним. В `src/canvas/surfaceformat.cpp` заменить
`constexpr int kDefaultSamples = 0;` на `constexpr int kDefaultSamples = 4;`.
В `src/main.cpp` добавить `#include "canvas/surfaceformat.h"` к остальным
включениям и заменить строки 17-22:

```cpp
    // Чернила рисуются залитой геометрией, поэтому сглаживание берётся от
    // мультисэмплинга: без него кромка линии видна ступеньками.
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setSamples(4);
    QSurfaceFormat::setDefaultFormat(format);
```

на

```cpp
    // Кромку чернил сглаживает InkMaterial; мультисэмплинг окна нужен только
    // для сравнения на устройстве (design.md, D8).
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setSamples(surfaceSampleCount(qgetenv("UQ_MSAA")));
    QSurfaceFormat::setDefaultFormat(format);
```

Тест 15.1 `defaultIsNoMultisampling` на этом шаге падает - так и должно быть до
15.9; остальные строки теста проходят.

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -E "tst_surfaceformat|tst_idleframes" --output-on-failure`
Expected: `100% tests passed`.

- [ ] 15.6 **Собрать сборку для аудита**

Run: `cmake -S . -B build -DUQ_DIAGNOSTICS=ON && cmake --build build -j$(nproc)`
Expected: сборка без ошибок.

- [ ] 15.7 **Аудит элементов окна без MSAA (на устройстве)**

Нужен PDF с оглавлением и заметка с раскрытым и свёрнутым вкладышем и чернилами
(подойдёт `build/tests/fixtures/textbook.pdf`, вкладыш создаётся инструментом
«Вкладыш»). Снять два снимка одного и того же состояния:

```bash
UQ_MSAA=4 UQ_SCREENSHOT=/tmp/uq-msaa4.png UQ_SCREENSHOT_DELAY_MS=1500 ./build/src/uq-notes build/tests/fixtures/textbook.pdf
UQ_MSAA=0 UQ_SCREENSHOT=/tmp/uq-msaa0.png UQ_SCREENSHOT_DELAY_MS=1500 ./build/src/uq-notes build/tests/fixtures/textbook.pdf
```

Сравнить снимки при увеличении (например, `magick compare /tmp/uq-msaa4.png /tmp/uq-msaa0.png /tmp/uq-diff.png`) по списку:

1. Рамка вкладыша (`InsertSceneNode`, ширина `kInsertBorderPixels / m_zoom`). Без
   MSAA прямоугольник шириной 1 логический px при dpr 1.75 и дробном зуме
   ложится то на 1, то на 2 device px.
2. Значок свёрнутого вкладыша, кнопки и ползунки панели инструментов,
   пункты оглавления, полоса экспорта. У `Rectangle` с `radius` сглаживание
   включено по умолчанию и от MSAA не зависит.
3. Текст интерфейса: рисуется distance field и от MSAA не зависит.
4. Чернила: идентичны в пределах шума, их кромку рисует `InkMaterial`.

Результат записать в `PERFORMANCE.md` (раздел «MSAA», 15.10).

- [ ] 15.8 **Если по пункту 1 аудита рамка вкладыша неровная - выровнять её по device px**

Делать только при неровной рамке на снимке `uq-msaa0.png`. Метод `updateInsertNodes`
и `m_zoom` уже есть, `window()->effectiveDevicePixelRatio()` - метод
`QQuickWindow`. В `src/canvas/documentcanvas.cpp` заменить строку 1231

```cpp
    const qreal borderWidth = qFuzzyIsNull(m_zoom) ? 1.0 : kInsertBorderPixels / m_zoom;
```

на

```cpp
    // Без мультисэмплинга (design.md, D8) рамка шириной в дробное число device px
    // ложится то на один, то на два пикселя. Ширина округляется до целого
    // числа device px, не меньше одного.
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    const qreal deviceBorder = qMax(1.0, std::round(kInsertBorderPixels * dpr));
    const qreal borderWidth = qFuzzyIsNull(m_zoom) ? 1.0 : deviceBorder / (dpr * m_zoom);
```

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R "tst_insertoverlay|tst_pagescenegraph" --output-on-failure`
Expected: `100% tests passed`. Повторить снимок `uq-msaa0.png` из 15.7, рамка ровная.

- [ ] 15.9 **Замер GPU и переключение значения по умолчанию**

На устройстве, документ на 200 страниц с чернилами (как в `PERFORMANCE.md`), в
соседнем терминале `sudo intel_gpu_top -o /tmp/gpu-msaaN.txt -s 1000`:

```bash
UQ_MSAA=4 UQ_DEMO_NAV=30 UQ_FRAME_LOG=1 ./build/src/uq-notes большой.pdf 2>&1 | tee /tmp/nav-msaa4.log
UQ_MSAA=0 UQ_DEMO_NAV=30 UQ_FRAME_LOG=1 ./build/src/uq-notes большой.pdf 2>&1 | tee /tmp/nav-msaa0.log
```

Гейт: занятость рендер-движка (`RCS %`) при `UQ_MSAA=0` не выше, чем при
`UQ_MSAA=4`; p99 интервала между кадрами в журнале не хуже. Если стенд GPU из
группы 1 перенесён, дополнительно его прогон на масштабах 1 и 2.3 (ожидание из
design.md, D7: 1.77 и 1.78 мс против 1.58 и 3.36 мс с MSAA).

Если гейт пройден, в `src/canvas/surfaceformat.cpp` вернуть
`constexpr int kDefaultSamples = 0;`.

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -E tst_idleframes --output-on-failure`
Expected: `100% tests passed`, включая `tst_surfaceformat::defaultIsNoMultisampling`.

Если гейт не пройден: значение по умолчанию остаётся 4, тест
`defaultIsNoMultisampling` меняется на `QCOMPARE(surfaceSampleCount(QByteArray()), 4)`
с комментарием со ссылкой на замер, в design.md D8 дописывается результат.

- [ ] 15.10 **Записать результаты и сменить версию**

В `PERFORMANCE.md` добавить раздел:

```markdown
## MSAA (ink-beauty-pass, D8)

Кромку чернил сглаживает InkMaterial, поэтому мультисэмплинг окна по
умолчанию выключен. `UQ_MSAA=4` возвращает его для сравнения.

| Показатель | UQ_MSAA=4 | UQ_MSAA=0 |
| --- | --- | --- |
| RCS, навигация 30 с | <из /tmp/gpu-msaa4.txt> | <из /tmp/gpu-msaa0.txt> |
| p99 интервала кадров | <из /tmp/nav-msaa4.log> | <из /tmp/nav-msaa0.log> |

Аудит элементов окна без MSAA: <итог пунктов 1-4 из 15.7 и была ли нужна 15.8>.
```

Угловые скобки заменить фактическими числами из файлов замера; раздел без чисел
не коммитится. В `README.md` в разделе «Сборка и запуск» после команды запуска
добавить абзац:

```markdown
Мультисэмплинг окна по умолчанию выключен: кромку чернил сглаживает шейдер.
Для сравнения его можно вернуть: `UQ_MSAA=4 ./build/src/uq-notes ...`.
```

Версия: `CMakeLists.txt` строка 2 `project(uq-notes VERSION 0.11.0 LANGUAGES CXX)`,
`packaging/PKGBUILD` `pkgver=0.11.0`.

- [ ] 15.11 **Финальный коммит шага 4**

```bash
git add src/canvas/surfaceformat.h src/canvas/surfaceformat.cpp src/CMakeLists.txt \
        src/main.cpp src/canvas/documentcanvas.cpp tests/tst_surfaceformat.cpp \
        tests/CMakeLists.txt PERFORMANCE.md README.md CMakeLists.txt packaging/PKGBUILD
git commit -F - <<'EOF'
Мультисэмплинг окна выключен, версия 0.11.0

Кромку чернил теперь сглаживает шейдер по точной доле покрытия пикселя, и
мультисэмплинг окна чернилам больше ничего не даёт. Зато он стоил половину
времени GPU плотной страницы на увеличенном масштабе: каждый пиксель кадра
обрабатывался четыре раза ради кромки, которую уже нарисовали точно.

Остальная сцена от него не зависела: у скруглённых прямоугольников интерфейса
своё сглаживание, текст рисуется distance field. Это проверено снимками окна в
обоих режимах; итог и замер GPU записаны в PERFORMANCE.md. Переменная UQ_MSAA
возвращает мультисэмплинг для сравнения на устройстве.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

Если шаг 15.8 не выполнялся, `src/canvas/documentcanvas.cpp` из `git add` убрать.

## 16. Завершение изменения

- [ ] 16.1 **Полный прогон тестов**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure`
Expected: `100% tests passed` (`tst_idleframes` без графической сессии сообщает SKIP).

- [ ] 16.2 **Сверка со спекой**

Пройти сценарии `openspec/changes/ink-beauty-pass/specs/ink-input/spec.md` и
`specs/note-storage/spec.md` и для каждого назвать тест или ручную проверку на
устройстве, которая его закрывает. Сценарии без проверки дописать тестами до
архивации. Слияние дельт в `openspec/specs/` - при архивации изменения
(`/opsx:archive`), не в этом плане.
