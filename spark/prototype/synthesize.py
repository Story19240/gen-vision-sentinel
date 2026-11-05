#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""确定性火花合成场景，用于离线算法验证。"""

from __future__ import annotations

import math
from typing import Any, Sequence

import numpy as np


SYNTHETIC_WIDTH = 640
SYNTHETIC_HEIGHT = 480
SYNTHETIC_CONTACT_ROI = (100, 200, 540, 232)
SYNTHETIC_REFERENCE_ROI = (100, 80, 540, 112)
SYNTHETIC_FALLING_ROI = (240, 232, 400, 420)

SCENARIOS = (
    "normal",
    "normal_global_drift",
    "blue_white_spark",
    "orange_spark_falling",
    "short_flash",
    "steady_lamp",
    "spark_outside_contact",
)

_SPARK_COLORS = {
    # Blue-white is pale enough to satisfy the configured H=100..130, S<=120
    # signature; orange-yellow is a saturated R/G/BGR vector near H=5..25.
    "blue_white": np.array([255, 200, 190], dtype=np.float32),
    "orange_yellow": np.array([15, 105, 255], dtype=np.float32),
}


def make_background(w: int, h: int, seed: int) -> np.ndarray:
    """生成带深灰渐变、轻微噪声和横向接触带的确定性背景。"""

    width = max(1, int(w))
    height = max(1, int(h))
    rng = np.random.default_rng(int(seed))

    y_gradient = np.linspace(26.0, 50.0, height, dtype=np.float32)
    x_gradient = np.linspace(-5.0, 5.0, width, dtype=np.float32)
    background = y_gradient[:, None] + x_gradient[None, :]

    band_y1 = int(round(height * 0.40))
    band_y2 = max(band_y1 + 1, int(round(height * 0.50)))
    band = background[band_y1:band_y2]
    texture = rng.uniform(-2.5, 2.5, size=(band.shape[0], width)).astype(
        np.float32
    )
    band += 7.0 + texture

    noise = rng.normal(0.0, 0.9, size=(height, width)).astype(np.float32)
    background += noise
    return np.clip(np.rint(background), 0, 255).astype(np.uint8)


def _phase_modulation(phase: float) -> float:
    """把任意 phase 映射为 0.85~1.0 的稳定闪烁调制。"""

    wave = 0.5 + 0.5 * math.sin(2.0 * math.pi * float(phase))
    return 0.85 + 0.15 * wave


def add_spark(
    frame: np.ndarray,
    center: Sequence[int],
    radius: float,
    color_type: str,
    intensity: float,
    phase: float,
) -> np.ndarray:
    """以逐通道加色方式叠加一个径向衰减的火花亮核。"""

    if frame is None or frame.ndim != 3 or frame.shape[2] < 3:
        raise ValueError("frame must be a BGR image")
    if color_type not in _SPARK_COLORS:
        raise ValueError(f"unsupported color_type: {color_type}")

    output = frame.copy()
    height, width = output.shape[:2]
    center_x = int(round(float(center[0])))
    center_y = int(round(float(center[1])))
    spark_radius = max(1.0, float(radius))

    x1 = max(0, center_x - int(math.ceil(spark_radius * 1.6)))
    x2 = min(width, center_x + int(math.ceil(spark_radius * 1.6)) + 1)
    y1 = max(0, center_y - int(math.ceil(spark_radius * 1.6)))
    y2 = min(height, center_y + int(math.ceil(spark_radius * 1.6)) + 1)
    if x1 >= x2 or y1 >= y2:
        return output

    yy, xx = np.mgrid[y1:y2, x1:x2]
    distance_sq = (xx - center_x) ** 2 + (yy - center_y) ** 2
    sigma = spark_radius * 0.55
    radial = np.exp(-0.5 * distance_sq / max(sigma * sigma, 1e-6))
    modulation = _phase_modulation(phase)
    alpha = radial * max(0.0, float(intensity)) * modulation

    patch = output[y1:y2, x1:x2, :3].astype(np.float32)
    color = _SPARK_COLORS[color_type]
    patch += alpha[:, :, None] * color[None, None, :]
    output[y1:y2, x1:x2, :3] = np.clip(np.rint(patch), 0, 255).astype(np.uint8)
    return output


def add_falling_sparks(
    frame: np.ndarray,
    falling_roi: Sequence[int],
    count: int = 6,
    seed: int = 0,
    frame_index: int = 0,
) -> np.ndarray:
    """在下落区内生成并叠加若干向下移动的小亮点。"""

    if frame is None or frame.ndim != 3 or frame.shape[2] < 3:
        raise ValueError("frame must be a BGR image")
    if len(falling_roi) != 4:
        raise ValueError("falling_roi must contain [x1, y1, x2, y2]")

    output = frame.copy()
    x1, y1, x2, y2 = (int(value) for value in falling_roi)
    width = max(0, x2 - x1)
    height = max(0, y2 - y1)
    if width <= 0 or height <= 0 or int(count) <= 0:
        return output

    rng = np.random.default_rng(int(seed))
    particle_count = int(count)
    base_x = rng.uniform(0.08, 0.92, size=particle_count)
    lane = np.linspace(0.08, 0.92, particle_count, dtype=np.float64)
    lane += rng.uniform(-0.025, 0.025, size=particle_count)
    speed = rng.uniform(0.018, 0.035, size=particle_count)
    offsets = rng.uniform(0.0, 1.0, size=particle_count)
    radii = rng.uniform(2.0, 3.1, size=particle_count)

    for index in range(particle_count):
        progress = (offsets[index] + float(frame_index) * speed[index]) % 1.0
        particle_x = int(round(x1 + lane[index] * width))
        particle_y = int(round(y1 + progress * (height - 1)))
        particle = add_spark(
            output,
            (particle_x, particle_y),
            float(radii[index]),
            "orange_yellow",
            1.0,
            0.0,
        )
        output = particle
    return output


def _drift_frame(frame: np.ndarray, frame_index: int, fps: float) -> np.ndarray:
    phase = float(frame_index) / max(float(fps), 1.0)
    offset = int(round(4.0 + 3.0 * math.sin(2.0 * math.pi * phase / 2.5)))
    drifted = frame.astype(np.int16) + offset
    return np.clip(drifted, 0, 255).astype(np.uint8)


def make_sequence(
    scenario: str,
    num_frames: int,
    fps: float,
    seed: int,
) -> tuple[list[np.ndarray], dict[str, Any]]:
    """生成完整帧序列以及场景真值。"""

    if scenario not in SCENARIOS:
        raise ValueError(
            f"unknown scenario {scenario!r}; expected one of {', '.join(SCENARIOS)}"
        )

    frame_count = max(1, int(num_frames))
    frame_rate = max(1.0, float(fps))
    start_frame = max(1, int(round(frame_rate * 0.2)))
    spark_frames = max(2, int(round(frame_rate * 0.6)))
    end_frame = min(frame_count - 1, start_frame + spark_frames - 1)

    truth: dict[str, Any] = {
        "scenario": scenario,
        "has_spark": False,
        "in_contact_roi": False,
        "start_frame": None,
        "end_frame": None,
        "color_type": "none",
        "level": "none",
        "expected_event_count": 0,
    }

    if scenario == "blue_white_spark":
        truth.update(
            has_spark=True,
            in_contact_roi=True,
            start_frame=start_frame,
            end_frame=end_frame,
            color_type="blue_white",
            level="light",
            expected_event_count=1,
        )
    elif scenario == "orange_spark_falling":
        truth.update(
            has_spark=True,
            in_contact_roi=True,
            start_frame=start_frame,
            end_frame=end_frame,
            color_type="orange_yellow",
            level="heavy",
            expected_event_count=1,
        )
    elif scenario == "short_flash":
        truth.update(
            has_spark=True,
            in_contact_roi=True,
            start_frame=start_frame,
            end_frame=min(start_frame + 1, frame_count - 1),
            color_type="blue_white",
            level="none",
            expected_event_count=0,
        )
    elif scenario == "spark_outside_contact":
        truth.update(
            has_spark=True,
            in_contact_roi=False,
            start_frame=start_frame,
            end_frame=end_frame,
            color_type="blue_white",
            level="none",
            expected_event_count=0,
        )

    frames: list[np.ndarray] = []
    contact_center = (
        (SYNTHETIC_CONTACT_ROI[0] + SYNTHETIC_CONTACT_ROI[2]) // 2,
        (SYNTHETIC_CONTACT_ROI[1] + SYNTHETIC_CONTACT_ROI[3]) // 2,
    )
    reference_center = (
        (SYNTHETIC_REFERENCE_ROI[0] + SYNTHETIC_REFERENCE_ROI[2]) // 2,
        (SYNTHETIC_REFERENCE_ROI[1] + SYNTHETIC_REFERENCE_ROI[3]) // 2,
    )

    for frame_index in range(frame_count):
        background_seed = (
            int(seed)
            if scenario == "steady_lamp"
            else int(seed) + frame_index * 1009
        )
        frame = make_background(
            SYNTHETIC_WIDTH,
            SYNTHETIC_HEIGHT,
            background_seed,
        )

        if scenario == "normal_global_drift":
            frame = _drift_frame(frame, frame_index, frame_rate)

        if scenario == "steady_lamp":
            frame = add_spark(
                frame,
                contact_center,
                22.0,
                "blue_white",
                1.0,
                0.0,
            )

        if scenario in (
            "blue_white_spark",
            "orange_spark_falling",
            "short_flash",
        ) and start_frame <= frame_index <= truth["end_frame"]:
            phase = (frame_index - start_frame) * 0.25
            color_type = (
                "orange_yellow"
                if scenario == "orange_spark_falling"
                else "blue_white"
            )
            frame = add_spark(
                frame,
                contact_center,
                34.0,
                color_type,
                1.0,
                phase,
            )
            if scenario == "orange_spark_falling":
                frame = add_falling_sparks(
                    frame,
                    SYNTHETIC_FALLING_ROI,
                    count=6,
                    seed=int(seed) + 17,
                    frame_index=frame_index - start_frame,
                )

        if scenario == "spark_outside_contact" and start_frame <= frame_index <= end_frame:
            phase = (frame_index - start_frame) * 0.25
            frame = add_spark(
                frame,
                reference_center,
                34.0,
                "blue_white",
                1.0,
                phase,
            )

        frames.append(frame)

    return frames, truth


__all__ = [
    "SCENARIOS",
    "SYNTHETIC_CONTACT_ROI",
    "SYNTHETIC_FALLING_ROI",
    "SYNTHETIC_REFERENCE_ROI",
    "add_falling_sparks",
    "add_spark",
    "make_background",
    "make_sequence",
]
