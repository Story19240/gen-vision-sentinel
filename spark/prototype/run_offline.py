#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""火花检测离线原型命令行工具。"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from dataclasses import asdict, fields
from pathlib import Path
from typing import Any

import cv2
import numpy as np

from spark_detector import (
    DetectionResult,
    SparkDetector,
    clip_roi,
    select_roi_profile,
)
from synthesize import SCENARIOS, make_sequence


PROTOTYPE_DIR = Path(__file__).resolve().parent
DEFAULT_CONFIG = PROTOTYPE_DIR.parent / "spark_config.example.json"
DEFAULT_OUTPUT = PROTOTYPE_DIR / "output"
CSV_FIELD_NAMES = [
    field.name
    for field in fields(DetectionResult)
    if field.name != "timestamp_ms"
]
NUMERIC_METRIC_NAMES = [
    field.name
    for field in fields(DetectionResult)
    if field.name not in {"timestamp_ms", "color_type", "level"}
]


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the offline spark detector on an image, video, or scenario"
    )
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--input", help="Input image or video file")
    source.add_argument(
        "--scenario",
        choices=SCENARIOS,
        help="Deterministic synthetic scenario name",
    )
    parser.add_argument(
        "--config",
        default=str(DEFAULT_CONFIG),
        help=f"Detector config JSON (default: {DEFAULT_CONFIG})",
    )
    parser.add_argument(
        "--output",
        default=str(DEFAULT_OUTPUT),
        help=f"Output directory (default: {DEFAULT_OUTPUT})",
    )
    parser.add_argument(
        "--baseline",
        action="store_true",
        help="Write normal-baseline statistics and mean+5sigma suggestions",
    )
    parser.add_argument(
        "--fps",
        type=float,
        default=None,
        help="Input FPS override; video FPS is read from the file when omitted",
    )
    parser.add_argument(
        "--frames",
        type=int,
        default=60,
        help="Number of frames for a synthetic scenario (default: 60)",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=20261105,
        help="Synthetic scenario seed (default: 20261105)",
    )
    return parser.parse_args()


def _load_config(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as config_file:
        config = json.load(config_file)
    if not isinstance(config, dict):
        raise ValueError("config root must be a JSON object")
    return config


def _read_image(path: Path) -> tuple[list[np.ndarray], float]:
    frame = cv2.imread(str(path), cv2.IMREAD_COLOR)
    if frame is None:
        raise ValueError(f"could not read image: {path}")
    return [frame], 25.0


def _read_video(path: Path, fps_override: float | None) -> tuple[list[np.ndarray], float]:
    capture = cv2.VideoCapture(str(path))
    if not capture.isOpened():
        raise ValueError(f"could not open video: {path}")
    fps = float(fps_override or capture.get(cv2.CAP_PROP_FPS) or 25.0)
    if fps <= 0.0:
        fps = 25.0
    frames: list[np.ndarray] = []
    while True:
        ok, frame = capture.read()
        if not ok:
            break
        frames.append(frame)
    capture.release()
    if not frames:
        raise ValueError(f"video contains no readable frames: {path}")
    return frames, fps


def _load_source(args: argparse.Namespace) -> tuple[list[np.ndarray], float]:
    if args.scenario:
        fps = float(args.fps or 25.0)
        frames, _ = make_sequence(
            args.scenario,
            num_frames=max(1, args.frames),
            fps=fps,
            seed=args.seed,
        )
        return frames, fps

    input_path = Path(args.input).expanduser()
    if not input_path.is_file():
        raise ValueError(f"input file does not exist: {input_path}")
    if input_path.suffix.lower() in {
        ".bmp",
        ".jpeg",
        ".jpg",
        ".png",
        ".tif",
        ".tiff",
        ".webp",
    }:
        return _read_image(input_path)
    return _read_video(input_path, args.fps)


def _run_detector(
    frames: list[np.ndarray], fps: float, config: dict[str, Any]
) -> list[DetectionResult]:
    detector = SparkDetector(config, fps)
    results: list[DetectionResult] = []
    for frame_index, frame in enumerate(frames):
        timestamp_ms = int(round(frame_index * 1000.0 / fps))
        results.append(detector.process_frame(frame, timestamp_ms=timestamp_ms))
    return results


def _write_metrics(path: Path, results: list[DetectionResult]) -> None:
    fieldnames = ["frame_index", "timestamp_ms"] + CSV_FIELD_NAMES
    with path.open("w", encoding="utf-8", newline="") as metrics_file:
        writer = csv.DictWriter(metrics_file, fieldnames=fieldnames)
        writer.writeheader()
        for frame_index, result in enumerate(results):
            row = asdict(result)
            row["frame_index"] = frame_index
            writer.writerow(row)


def _summary(results: list[DetectionResult]) -> dict[str, Any]:
    events = []
    for frame_index, result in enumerate(results):
        if result.event_triggered:
            events.append(
                {
                    "frame_index": frame_index,
                    "frame_number": frame_index + 1,
                    "timestamp_ms": result.timestamp_ms,
                    "level": result.level,
                    "color_type": result.color_type,
                }
            )

    max_metrics: dict[str, float | int] = {}
    for metric_name in NUMERIC_METRIC_NAMES:
        values = [getattr(result, metric_name) for result in results]
        if not values:
            continue
        if all(isinstance(value, bool) for value in values):
            max_metrics[metric_name] = int(max(values))
        elif all(isinstance(value, int) for value in values):
            max_metrics[metric_name] = int(max(values))
        else:
            max_metrics[metric_name] = float(max(values))

    return {
        "total_frames": len(results),
        "event_count": len(events),
        "events": events,
        "max_metrics": max_metrics,
    }


def _draw_roi(
    image: np.ndarray,
    roi: list[int] | None,
    label: str,
    color: tuple[int, int, int],
) -> None:
    clipped = clip_roi(roi, image.shape)
    if clipped is None:
        return
    x1, y1, x2, y2 = clipped
    cv2.rectangle(image, (x1, y1), (x2, y2), color, 2)
    cv2.putText(
        image,
        label,
        (x1, max(16, y1 - 6)),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.55,
        color,
        2,
        cv2.LINE_AA,
    )


def _write_annotated(
    path: Path,
    frame: np.ndarray,
    result: DetectionResult,
    config: dict[str, Any],
) -> None:
    annotated = frame.copy()
    _draw_roi(
        annotated,
        select_roi_profile(config.get("contact_roi"), frame.shape),
        "CONTACT",
        (0, 220, 0),
    )
    _draw_roi(
        annotated,
        select_roi_profile(config.get("reference_roi"), frame.shape),
        "REFERENCE",
        (255, 180, 0),
    )
    _draw_roi(
        annotated,
        select_roi_profile(config.get("falling_roi"), frame.shape),
        "FALLING",
        (0, 165, 255),
    )
    if result.event_triggered:
        event_text = f"EVENT {result.level.upper()} {result.color_type}"
        cv2.rectangle(annotated, (0, 0), (annotated.shape[1], 38), (0, 0, 180), -1)
        cv2.putText(
            annotated,
            event_text,
            (12, 27),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.75,
            (255, 255, 255),
            2,
            cv2.LINE_AA,
        )
    if not cv2.imwrite(str(path), annotated):
        raise ValueError(f"could not write annotated image: {path}")


def _write_baseline(path: Path, results: list[DetectionResult]) -> None:
    baseline_metrics = (
        "rel_saturation",
        "rel_flicker",
        "falling_bright_pixels",
    )
    baseline: dict[str, Any] = {}
    for metric_name in baseline_metrics:
        values = np.asarray(
            [getattr(result, metric_name) for result in results],
            dtype=np.float64,
        )
        mean = float(np.mean(values))
        std = float(np.std(values))
        p99_9 = float(np.percentile(values, 99.9))
        baseline[metric_name] = {
            "mean": mean,
            "std": std,
            "p99_9": p99_9,
            "suggested_threshold_mean_plus_5_sigma": mean + 5.0 * std,
        }
    with path.open("w", encoding="utf-8") as baseline_file:
        json.dump(baseline, baseline_file, indent=2, ensure_ascii=False)
        baseline_file.write("\n")


def main() -> int:
    args = _parse_args()
    try:
        config = _load_config(Path(args.config).expanduser())
        frames, fps = _load_source(args)
        results = _run_detector(frames, fps, config)
        output_dir = Path(args.output).expanduser()
        output_dir.mkdir(parents=True, exist_ok=True)

        _write_metrics(output_dir / "metrics.csv", results)
        summary = _summary(results)
        with (output_dir / "summary.json").open("w", encoding="utf-8") as summary_file:
            json.dump(summary, summary_file, indent=2, ensure_ascii=False)
            summary_file.write("\n")

        if args.scenario or Path(args.input).suffix.lower() in {
            ".bmp",
            ".jpeg",
            ".jpg",
            ".png",
            ".tif",
            ".tiff",
            ".webp",
        }:
            event_index = next(
                (
                    index
                    for index, result in enumerate(results)
                    if result.event_triggered
                ),
                len(results) - 1,
            )
            _write_annotated(
                output_dir / "annotated.png",
                frames[event_index],
                results[event_index],
                config,
            )

        if args.baseline:
            _write_baseline(output_dir / "baseline.json", results)

        print(f"processed {len(results)} frame(s) at {fps:.3f} FPS")
        print(f"events: {summary['event_count']}")
        print(f"output: {output_dir}")
        return 0
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
