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


# Сверка копии сэмплера с приложением: без D6 осевая та же до округления
# метрик (3 знака).
keys = ['centerline_dev_mean_px', 'centerline_dev_max_px', 'facet_count_2deg',
        'curvature_tv_excess_per100px', 'corner_dist_max_px', 'settled_mutations',
        'false_cusp_count', 'centerline_points']
diff = [(k, key) for k in base for key in keys if base[k].get(key) != per['d6-copy'][k].get(key)]
check('d6-copy совпадает с baseline', not diff,
      f'расхождений {len(diff)}' + (f', первое {diff[0]}' if diff else ''))

# Вершина острия в максимуме - по корпусу без острий без стоянки и без зубца:
# у тех свои критерии ниже (у зубца вершины эталонные только ради ложных
# острий, его срез меряет tooth_peak_ratio).
def own_criterion(sample):
    return 'nostop' in sample or sample == 'tooth_240'


for scale in scales:
    b = max(s['corner_dist_max_px'] for k, s in base.items()
            if k[1] == scale and s.get('corner_dist_max_px') is not None and not own_criterion(k[0]))
    d = max(s['corner_dist_max_px'] for k, s in cand.items()
            if k[1] == scale and s.get('corner_dist_max_px') is not None and not own_criterion(k[0]))
    check(f'x{scale:g}: вершина острия в максимуме <= 0.6 px', d <= 0.6, f'{b} -> {d}')

# Ключи сводки по масштабам (s1, s2.5, ...) - из самой сводки.
for tag in sorted(summary['baseline'], key=lambda t: float(t[1:])):
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
