#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""火花检测离线原型的七场景自验证。"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable

from spark_detector import DetectionResult, SparkDetector
from synthesize import make_sequence


FPS = 25
NUM_FRAMES = 60
SEED = 20261105
PROJECT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_CONFIG = PROJECT_DIR / "spark_config.example.json"
RUN_OFFLINE = Path(__file__).resolve().parent / "run_offline.py"


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


def _check_missing_contact_roi_rejected() -> bool:
    with tempfile.TemporaryDirectory(prefix="spark-bad-config-") as temp_dir:
        temp_path = Path(temp_dir)
        bad_config = json.loads(DEFAULT_CONFIG.read_text(encoding="utf-8"))
        bad_config.pop("contact_roi")
        bad_config_path = temp_path / "bad_config.json"
        bad_config_path.write_text(
            json.dumps(bad_config, indent=2, ensure_ascii=False),
            encoding="utf-8",
        )
        output_dir = temp_path / "output"
        completed = subprocess.run(
            [
                sys.executable,
                str(RUN_OFFLINE),
                "--scenario",
                "normal",
                "--config",
                str(bad_config_path),
                "--output",
                str(output_dir),
                "--frames",
                "1",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        return (
            completed.returncode != 0
            and "contact_roi" in completed.stderr
            and not (output_dir / "metrics.csv").exists()
            and not (output_dir / "summary.json").exists()
        )


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

    config_check_passed = _check_missing_contact_roi_rejected()
    config_status = "PASS" if config_check_passed else "FAIL"
    print(
        f"{config_status} missing_contact_roi_rejected: "
        "runner rejected invalid config without metrics/summary"
    )
    if not config_check_passed:
        failures += 1

    if failures:
        print(f"FAILED: {failures} check(s) did not meet expectations")
        return 1
    print("PASSED: 8/8 checks met expectations")
    return 0


if __name__ == "__main__":
    sys.exit(main())
