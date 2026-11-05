#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""火花检测离线原型的七场景自验证。"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Callable

from spark_detector import DetectionResult, SparkDetector
from synthesize import make_sequence


FPS = 25
NUM_FRAMES = 60
SEED = 20261105
PROJECT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_CONFIG = PROJECT_DIR / "spark_config.example.json"


def _load_config() -> dict:
    with DEFAULT_CONFIG.open("r", encoding="utf-8") as config_file:
        return json.load(config_file)


def _run_scenario(
    scenario: str, config: dict
) -> tuple[list[DetectionResult], dict]:
    frames, truth = make_sequence(
        scenario,
        num_frames=NUM_FRAMES,
        fps=FPS,
        seed=SEED,
    )
    detector = SparkDetector(config, FPS)
    results = [
        detector.process_frame(frame, timestamp_ms=index * 1000 // FPS)
        for index, frame in enumerate(frames)
    ]
    return results, truth


def _events(results: list[DetectionResult]) -> list[DetectionResult]:
    return [result for result in results if result.event_triggered]


def _check_normal(scenario: str, results: list[DetectionResult], _: dict) -> bool:
    return len(_events(results)) == 0


def _check_blue_white(
    scenario: str, results: list[DetectionResult], _: dict
) -> bool:
    events = _events(results)
    return bool(events) and any(event.level == "light" for event in events)


def _check_orange(
    scenario: str, results: list[DetectionResult], _: dict
) -> bool:
    events = _events(results)
    return bool(events) and any(event.level == "heavy" for event in events)


CHECKS: dict[str, Callable[[str, list[DetectionResult], dict], bool]] = {
    "normal": _check_normal,
    "normal_global_drift": _check_normal,
    "blue_white_spark": _check_blue_white,
    "orange_spark_falling": _check_orange,
    "short_flash": _check_normal,
    "steady_lamp": _check_normal,
    "spark_outside_contact": _check_normal,
}


def main() -> int:
    config = _load_config()
    failures = 0
    print("火花检测离线原型自验证")
    print(f"config: {DEFAULT_CONFIG}")
    print(f"fps: {FPS}, frames/scenario: {NUM_FRAMES}, seed: {SEED}")

    for scenario, check in CHECKS.items():
        results, truth = _run_scenario(scenario, config)
        events = _events(results)
        passed = check(scenario, results, truth)
        event_summary = ", ".join(
            f"level={event.level}, color={event.color_type}" for event in events
        )
        if not event_summary:
            event_summary = "none"
        status = "PASS" if passed else "FAIL"
        print(
            f"{status} {scenario}: events={len(events)} "
            f"({event_summary})"
        )
        if not passed:
            failures += 1

    if failures:
        print(f"FAILED: {failures} scenario(s) did not meet expectations")
        return 1
    print("PASSED: 7/7 scenarios met expectations")
    return 0


if __name__ == "__main__":
    sys.exit(main())
