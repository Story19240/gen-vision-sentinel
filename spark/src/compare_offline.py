#!/usr/bin/env python3
"""Compare two spark offline result directories for regression review."""

import argparse
import csv
import json
import math
import sys


FLOAT_FIELDS = {
    "rel_saturation",
    "contact_sat_ratio",
    "ref_sat_ratio",
    "rel_flicker",
    "contact_flicker",
    "ref_flicker",
    "color_match_ratio",
    "contact_v_mean",
    "reference_v_mean",
}
INTEGER_FIELDS = {
    "falling_bright_pixels",
    "falling_components",
    "candidate_duration_ms",
}
BOOLEAN_FIELDS = {"color_hit", "candidate", "event_triggered"}
STRING_FIELDS = {"color_type", "level"}
MAX_FLOAT_FIELDS = FLOAT_FIELDS.copy()
MAX_INTEGER_FIELDS = INTEGER_FIELDS.copy()


def parse_bool(value):
    lowered = value.strip().lower()
    if lowered == "true":
        return True
    if lowered == "false":
        return False
    raise ValueError(f"invalid boolean in metrics CSV: {value!r}")


def numbers_close(expected, actual, tolerance, relative_tolerance):
    if math.isnan(expected) and math.isnan(actual):
        return True
    return math.isclose(
        expected,
        actual,
        rel_tol=relative_tolerance,
        abs_tol=tolerance,
    )


def compare_metrics(expected_path, actual_path, tolerance, relative_tolerance):
    errors = []
    with open(expected_path, "r", encoding="utf-8", newline="") as expected_file:
        expected_rows = list(csv.DictReader(expected_file))
    with open(actual_path, "r", encoding="utf-8", newline="") as actual_file:
        actual_rows = list(csv.DictReader(actual_file))

    if not expected_rows:
        errors.append("expected metrics.csv has no data rows")
        return errors
    if len(expected_rows) != len(actual_rows):
        errors.append(
            f"frame count differs: expected {len(expected_rows)}, actual {len(actual_rows)}"
        )
        return errors

    for row_index, (expected, actual) in enumerate(zip(expected_rows, actual_rows)):
        if expected.get("frame_index") != actual.get("frame_index"):
            errors.append(
                f"frame {row_index}: frame_index differs: "
                f"{expected.get('frame_index')!r} != {actual.get('frame_index')!r}"
            )
        if expected.get("timestamp_ms") != actual.get("timestamp_ms"):
            errors.append(
                f"frame {row_index}: timestamp_ms differs: "
                f"{expected.get('timestamp_ms')!r} != {actual.get('timestamp_ms')!r}"
            )
        for field in sorted(FLOAT_FIELDS):
            try:
                expected_value = float(expected[field])
                actual_value = float(actual[field])
            except (KeyError, TypeError, ValueError) as error:
                errors.append(f"frame {row_index}: {field}: {error}")
                continue
            if not numbers_close(
                expected_value,
                actual_value,
                tolerance,
                relative_tolerance,
            ):
                errors.append(
                    f"frame {row_index}: {field} differs: "
                    f"{expected_value!r} != {actual_value!r}"
                )
        for field in sorted(INTEGER_FIELDS):
            try:
                if int(expected[field]) != int(actual[field]):
                    errors.append(
                        f"frame {row_index}: {field} differs: "
                        f"{expected[field]!r} != {actual[field]!r}"
                    )
            except (KeyError, TypeError, ValueError) as error:
                errors.append(f"frame {row_index}: {field}: {error}")
        for field in sorted(BOOLEAN_FIELDS):
            try:
                if parse_bool(expected[field]) != parse_bool(actual[field]):
                    errors.append(
                        f"frame {row_index}: {field} differs: "
                        f"{expected[field]!r} != {actual[field]!r}"
                    )
            except (KeyError, ValueError) as error:
                errors.append(f"frame {row_index}: {field}: {error}")
        for field in sorted(STRING_FIELDS):
            if expected.get(field) != actual.get(field):
                errors.append(
                    f"frame {row_index}: {field} differs: "
                    f"{expected.get(field)!r} != {actual.get(field)!r}"
                )
    return errors


def compare_events(expected, actual):
    errors = []
    expected_events = expected.get("events")
    actual_events = actual.get("events")
    if expected_events != actual_events:
        errors.append(
            f"event list differs: expected {expected_events!r}, actual {actual_events!r}"
        )
    return errors


def compare_max_metrics(expected, actual, tolerance, relative_tolerance):
    errors = []
    expected_metrics = expected.get("max_metrics", {})
    actual_metrics = actual.get("max_metrics", {})
    if set(expected_metrics) != set(actual_metrics):
        errors.append(
            "max_metrics keys differ: "
            f"expected {sorted(expected_metrics)}, actual {sorted(actual_metrics)}"
        )
        return errors
    for field in sorted(expected_metrics):
        expected_value = expected_metrics[field]
        actual_value = actual_metrics[field]
        if field in MAX_FLOAT_FIELDS:
            try:
                if not numbers_close(
                    float(expected_value),
                    float(actual_value),
                    tolerance,
                    relative_tolerance,
                ):
                    errors.append(
                        f"max_metrics.{field} differs: "
                        f"{expected_value!r} != {actual_value!r}"
                    )
            except (TypeError, ValueError) as error:
                errors.append(f"max_metrics.{field}: {error}")
        elif field in MAX_INTEGER_FIELDS:
            if int(expected_value) != int(actual_value):
                errors.append(
                    f"max_metrics.{field} differs: "
                    f"{expected_value!r} != {actual_value!r}"
                )
        elif expected_value != actual_value:
            errors.append(
                f"max_metrics.{field} differs: "
                f"{expected_value!r} != {actual_value!r}"
            )
    return errors


def main():
    parser = argparse.ArgumentParser(
        description="Compare Python and C++ spark offline outputs"
    )
    parser.add_argument("expected", help="reference result directory")
    parser.add_argument("actual", help="actual result directory")
    parser.add_argument(
        "--tolerance",
        type=float,
        default=1e-9,
        help="absolute floating-point tolerance (default: 1e-9)",
    )
    parser.add_argument(
        "--relative-tolerance",
        type=float,
        default=1e-9,
        help="relative floating-point tolerance (default: 1e-9)",
    )
    arguments = parser.parse_args()

    try:
        errors = compare_metrics(
            f"{arguments.expected}/metrics.csv",
            f"{arguments.actual}/metrics.csv",
            arguments.tolerance,
            arguments.relative_tolerance,
        )
        with open(f"{arguments.expected}/summary.json", "r", encoding="utf-8") as file:
            expected_summary = json.load(file)
        with open(f"{arguments.actual}/summary.json", "r", encoding="utf-8") as file:
            actual_summary = json.load(file)
        if expected_summary.get("total_frames") != actual_summary.get("total_frames"):
            errors.append(
                "total_frames differs: "
                f"{expected_summary.get('total_frames')!r} != "
                f"{actual_summary.get('total_frames')!r}"
            )
        if expected_summary.get("event_count") != actual_summary.get("event_count"):
            errors.append(
                "event_count differs: "
                f"{expected_summary.get('event_count')!r} != "
                f"{actual_summary.get('event_count')!r}"
            )
        errors.extend(compare_events(expected_summary, actual_summary))
        errors.extend(
            compare_max_metrics(
                expected_summary,
                actual_summary,
                arguments.tolerance,
                arguments.relative_tolerance,
            )
        )
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print("COMPARE_OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
