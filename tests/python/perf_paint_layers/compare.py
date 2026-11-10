# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Table builder for the results of `run.py`.

* With `--a` and `--b`: A/B table, delta and verdict per scenario and row count.
  Counters are compared exactly; time (`wall_ms`, `regen_ms`) with a +-10 % threshold.
* With `--b` only: B against the expectations of TZ section 7 (absolute check, used when no
  baseline build exists).

Prints a Markdown table to stdout.

Usage:
  python compare.py --b <dir>/results_B.json [--a <dir>/results_A.json]
"""

import argparse
import json
import sys

TIME_THRESHOLD = 0.10
COUNTERS = ["full_rebuild", "kept", "compile", "extract", "pass_hit", "pass_miss",
            "queued_fallback", "queued_default", "prune_mix"]
TIMES = ["wall_ms", "regen_ms"]

# TZ section 7, as absolute expectations for B (median over reps unless noted).
ZERO_COMPILE_ZERO_REBUILD = {"S1", "S2", "S3", "S4", "S6", "S7", "S8", "S10", "S12"}


def load(path):
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    index = {}
    for entry in data["results"]:
        index[(entry["scenario"], entry["rows"])] = entry
    return data, index


def median(entry, key):
    summary = entry.get("summary") or {}
    if key not in summary:
        return None
    return summary[key]["median"]


def p95(entry, key):
    summary = entry.get("summary") or {}
    if key not in summary:
        return None
    return summary[key]["p95"]


def fmt(value):
    if value is None:
        return "-"
    if isinstance(value, float) and not value.is_integer():
        return f"{value:.2f}"
    return f"{int(value)}"


def verdict_counter(a, b):
    if a is None or b is None:
        return "нет данных"
    if b < a:
        return "улучшение"
    if b > a:
        return "регресс"
    return "без изменений"


def verdict_time(a, b):
    if a is None or b is None or a == 0:
        return "нет данных"
    delta = (b - a) / a
    if delta > TIME_THRESHOLD:
        return "регресс"
    if delta < -TIME_THRESHOLD:
        return "улучшение"
    return "без изменений"


def expectation(scenario, entry):
    """Verdict of B against TZ section 7 (no A needed)."""
    status = entry.get("status")
    if status == "NOT_RUN_BACKGROUND":
        return "NOT_RUN (нужен GUI)"
    if status != "OK":
        return f"ОШИБКА ({status})"
    full = p95(entry, "full_rebuild")
    comp = p95(entry, "compile")
    if scenario in ZERO_COMPILE_ZERO_REBUILD:
        ok = (full or 0) == 0 and (comp or 0) == 0
        return "соответствует" if ok else f"НЕ соответствует (full p95={fmt(full)}, compile p95={fmt(comp)})"
    if scenario == "S5":
        comp_med = median(entry, "compile") or 0
        return "соответствует (<=2 компиляций на 10 шагов)" if comp_med <= 2 else \
            f"НЕ соответствует (compile med={fmt(comp_med)})"
    if scenario == "S9":
        return "допустимо (форма меняется)"
    if scenario == "S11":
        return "контроль (FULL REBUILD допустим)"
    return "см. таблицу"


def build_ab(a_index, b_index, keys):
    lines = ["| Сценарий | N | Метрика | A med (p95) | B med (p95) | Дельта | Вердикт |",
             "|---|---|---|---|---|---|---|"]
    for key in keys:
        scenario, rows = key
        a = a_index.get(key)
        b = b_index.get(key)
        if b is None:
            continue
        if b.get("status") == "NOT_RUN_BACKGROUND":
            lines.append(f"| {scenario} | {rows} | — | — | — | — | NOT_RUN (нужен GUI) |")
            continue
        for metric in COUNTERS:
            av, bv = median(a, metric) if a else None, median(b, metric)
            if av is None and bv is None:
                continue
            delta = None if av is None or bv is None else bv - av
            lines.append(
                f"| {scenario} | {rows} | {metric} | {fmt(av)} ({fmt(p95(a, metric) if a else None)}) | "
                f"{fmt(bv)} ({fmt(p95(b, metric))}) | {fmt(delta)} | {verdict_counter(av, bv)} |")
        for metric in TIMES:
            av, bv = median(a, metric) if a else None, median(b, metric)
            if av is None and bv is None:
                continue
            delta = None if av is None or bv is None or av == 0 else f"{(bv - av) / av * 100:+.1f} %"
            lines.append(
                f"| {scenario} | {rows} | {metric}, мс | {fmt(av)} | {fmt(bv)} | {delta or '-'} | "
                f"{verdict_time(av, bv)} |")
    return "\n".join(lines)


def layered_note(scenario, entry):
    """Flag S1 runs where the layered material got no tracked value map."""
    layered = entry.get("layered") or {}
    tracked_1 = layered.get("tracked_1_total", 0)
    tracked_0 = layered.get("tracked_0_total", 0)
    refused = layered.get("refused") or {}
    top = sorted(refused.items(), key=lambda kv: -kv[1])[:2]
    refused_text = "; ".join(f"{reason} x{count}" for reason, count in top) or "нет"
    if scenario == "S1" and tracked_1 == 0:
        if tracked_0 > 0:
            return f"карта не построилась, см. refused ({refused_text})"
        return "нет строк extract_time (сборка без новых строк?)"
    if render_skipped(entry):
        return "рендер пропущен"
    return "-"


def render_skipped(entry):
    return bool(entry.get("render_skipped"))


def build_layered(a_index, b_index, keys):
    """Layered-material extract counters: extract_layered, build time, tracked share (B only)."""
    lines = ["| Сценарий | N | extract_layered A med | extract_layered B med | Дельта | Вердикт | "
             "extract_build_ms_median A | extract_build_ms_median B | tracked доля B | Примечание |",
             "|---|---|---|---|---|---|---|---|---|---|"]
    for key in keys:
        scenario, rows = key
        b = b_index.get(key)
        if b is None or b.get("status") != "OK":
            continue
        a = a_index.get(key) if a_index else None
        av = median(a, "extract_layered") if a else None
        bv = median(b, "extract_layered")
        if a_index is None:
            verdict = "нет A (B-only)"
        elif a is None or av is None or bv is None:
            verdict = "нет данных"
        else:
            verdict = verdict_counter(av, bv)
        delta = None if av is None or bv is None else bv - av
        share = median(b, "tracked_share")
        share_text = "-" if share is None else f"{share * 100:.0f} %"
        lines.append(
            f"| {scenario} | {rows} | {fmt(av)} | {fmt(bv)} | {fmt(delta)} | {verdict} | "
            f"{fmt(median(a, 'extract_build_ms_median') if a else None)} | "
            f"{fmt(median(b, 'extract_build_ms_median'))} | {share_text} | "
            f"{layered_note(scenario, b)} |")
    return "\n".join(lines)


def build_expect(b_index, keys):
    lines = ["| Сценарий | N | full_rebuild med (p95) | compile med (p95) | pass_miss med | "
             "regen_ms med | wall_ms med | Ожидание TZ | Вердикт |",
             "|---|---|---|---|---|---|---|---|---|"]
    for key in keys:
        scenario, rows = key
        b = b_index.get(key)
        if b is None:
            continue
        lines.append(
            f"| {scenario} | {rows} | {fmt(median(b, 'full_rebuild'))} ({fmt(p95(b, 'full_rebuild'))}) | "
            f"{fmt(median(b, 'compile'))} ({fmt(p95(b, 'compile'))}) | {fmt(median(b, 'pass_miss'))} | "
            f"{fmt(median(b, 'regen_ms'))} | {fmt(median(b, 'wall_ms'))} | "
            f"{'см. раздел 7' if scenario in ZERO_COMPILE_ZERO_REBUILD or scenario in ('S5', 'S9', 'S11') else '-'} | "
            f"{expectation(scenario, b)} |")
    return "\n".join(lines)


def checks_table(b_index):
    lines = ["| Сценарий | N | mut vs fresh (макс. разница /255) | reload vs mut | state == fresh | "
             "digest mut == fresh |", "|---|---|---|---|---|---|"]
    for (scenario, rows), entry in sorted(b_index.items()):
        for check in entry.get("checks", []):
            mvf = check.get("mut_vs_fresh", {}).get("max_diff_255")
            rvm = check.get("reload_vs_mut", {}).get("max_diff_255")
            digest_eq = check.get("digest_mut") == check.get("digest_fresh")
            lines.append(f"| {scenario} | {rows} | {fmt(mvf)} | {fmt(rvm)} | "
                         f"{check.get('state_equal_fresh')} | {digest_eq} |")
    return "\n".join(lines)


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--b", required=True)
    parser.add_argument("--a")
    args = parser.parse_args(argv)

    _, b_index = load(args.b)
    keys = sorted(b_index.keys(), key=lambda k: (k[0], k[1]))
    print("## Проверка корректности рендера (раздел 5)\n")
    print(checks_table(b_index))
    print()
    if args.a:
        _, a_index = load(args.a)
        print("## A vs B\n")
        print(build_ab(a_index, b_index, keys))
        print()
        print("## Слоистый материал (extract)\n")
        print(build_layered(a_index, b_index, keys))
    else:
        print("## B против ожиданий раздела 7 (baseline A не предоставлен)\n")
        print(build_expect(b_index, keys))
        print()
        print("## Слоистый материал (extract), B only\n")
        print(build_layered(None, b_index, keys))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
