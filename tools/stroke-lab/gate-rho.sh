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
