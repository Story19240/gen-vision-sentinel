#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""基于四路物理信号的火花检测器（离线算法原型）。"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass
from typing import Any, Deque, Mapping, Sequence

import cv2
import numpy as np


@dataclass(frozen=True)
class DetectionResult:
    """单帧检测结果。"""

    timestamp_ms: int
    rel_saturation: float
    contact_sat_ratio: float
    ref_sat_ratio: float
    rel_flicker: float
    contact_flicker: float
    ref_flicker: float
    color_match_ratio: float
    color_hit: bool
    color_type: str
    falling_bright_pixels: int
    falling_components: int
    candidate: bool
    event_triggered: bool
    level: str
    contact_v_mean: float
    reference_v_mean: float
    candidate_duration_ms: int


def _as_roi(value: Any) -> list[int] | None:
    if not isinstance(value, (list, tuple)) or len(value) != 4:
        return None
    try:
        x1, y1, x2, y2 = (int(item) for item in value)
    except (TypeError, ValueError):
        return None
    if x2 <= x1 or y2 <= y1:
        return None
    return [x1, y1, x2, y2]


def select_roi_profile(
    roi_config: Any, frame_shape: Sequence[int]
) -> list[int] | None:
    """按画面尺寸选择 ROI 配置，返回未裁剪的 [x1, y1, x2, y2]。"""

    if not isinstance(roi_config, Mapping):
        return _as_roi(roi_config)

    sample = _as_roi(roi_config.get("sample"))
    synthetic = _as_roi(roi_config.get("synthetic"))
    if len(frame_shape) < 2:
        return sample or synthetic

    frame_h, frame_w = int(frame_shape[0]), int(frame_shape[1])
    if frame_w == 751 and frame_h == 1024 and sample is not None:
        return sample
    if frame_w == 640 and frame_h == 480 and synthetic is not None:
        return synthetic

    preferred = synthetic if frame_w >= frame_h else sample
    fallback = sample if preferred is synthetic else synthetic
    return preferred or fallback


def clip_roi(
    roi: Sequence[int] | None, frame_shape: Sequence[int]
) -> tuple[int, int, int, int] | None:
    """将 ROI 裁剪到画面范围内；空或非法 ROI 返回 None。"""

    if roi is None or len(roi) != 4 or len(frame_shape) < 2:
        return None
    frame_h, frame_w = int(frame_shape[0]), int(frame_shape[1])
    x1 = max(0, min(int(roi[0]), frame_w))
    y1 = max(0, min(int(roi[1]), frame_h))
    x2 = max(0, min(int(roi[2]), frame_w))
    y2 = max(0, min(int(roi[3]), frame_h))
    if x2 <= x1 or y2 <= y1:
        return None
    return x1, y1, x2, y2


def _hue_in_range(hue: np.ndarray, bounds: Sequence[float]) -> np.ndarray:
    if len(bounds) < 2:
        return np.zeros(hue.shape, dtype=bool)
    low = float(bounds[0]) % 180.0
    high = float(bounds[1]) % 180.0
    if low <= high:
        return (hue >= low) & (hue <= high)
    return (hue >= low) | (hue <= high)


def _mean_frame_delta(values: Deque[float]) -> float:
    if len(values) < 2:
        return 0.0
    sequence = list(values)
    return float(
        np.mean(np.abs(np.diff(np.asarray(sequence, dtype=np.float64))))
    )


class SparkDetector:
    """四路物理信号检测器。"""

    def __init__(self, config: Mapping[str, Any], fps: float) -> None:
        self.config = dict(config)
        self.fps = float(fps) if float(fps) > 0.0 else 25.0
        self.v_sat_threshold = int(self.config.get("v_sat_threshold", 240))
        self.use_relative = bool(self.config.get("use_relative", True))

        thresholds = self.config.get("thresholds", {})
        if not isinstance(thresholds, Mapping):
            thresholds = {}
        self.rel_saturation_threshold = float(
            thresholds.get("rel_saturation", 0.01)
        )
        self.flicker_threshold = float(thresholds.get("flicker", 0.5))
        self.color_match_threshold = float(
            thresholds.get("color_match_ratio", 0.5)
        )

        level_thresholds = self.config.get("level_thresholds", {})
        if not isinstance(level_thresholds, Mapping):
            level_thresholds = {}
        self.heavy_bright_pixels = int(
            level_thresholds.get("heavy_bright_pixels", 20)
        )
        self.heavy_components = int(
            level_thresholds.get("heavy_components", 2)
        )

        confirm_duration_ms = int(self.config.get("confirm_duration_ms", 200))
        self.confirm_duration_ms = max(0, confirm_duration_ms)
        window_frames = max(2, int(round(self.fps * 0.5)))
        self._contact_v_history: Deque[float] = deque(maxlen=window_frames)
        self._reference_v_history: Deque[float] = deque(maxlen=window_frames)
        self._candidate_start_ms: int | None = None
        self._candidate_latched = False
        self._last_timestamp_ms: int | None = None
        self._frame_index = 0

    def reset(self) -> None:
        """清空闪烁历史和时序确认状态。"""

        self._contact_v_history.clear()
        self._reference_v_history.clear()
        self._candidate_start_ms = None
        self._candidate_latched = False
        self._last_timestamp_ms = None
        self._frame_index = 0

    def _empty_result(self, timestamp_ms: int) -> DetectionResult:
        return DetectionResult(
            timestamp_ms=int(timestamp_ms),
            rel_saturation=0.0,
            contact_sat_ratio=0.0,
            ref_sat_ratio=0.0,
            rel_flicker=0.0,
            contact_flicker=0.0,
            ref_flicker=0.0,
            color_match_ratio=0.0,
            color_hit=False,
            color_type="none",
            falling_bright_pixels=0,
            falling_components=0,
            candidate=False,
            event_triggered=False,
            level="none",
            contact_v_mean=0.0,
            reference_v_mean=0.0,
            candidate_duration_ms=0,
        )

    def _normalize_frame(self, frame_bgr: Any) -> np.ndarray | None:
        if frame_bgr is None:
            return None
        frame = np.asarray(frame_bgr)
        if frame.ndim != 3 or frame.shape[0] <= 0 or frame.shape[1] <= 0:
            return None
        if frame.shape[2] < 3:
            return None
        frame = frame[:, :, :3]
        if frame.dtype != np.uint8:
            if np.issubdtype(frame.dtype, np.floating) and frame.size:
                max_value = float(np.nanmax(frame))
                if max_value <= 1.0:
                    frame = frame * 255.0
            frame = np.nan_to_num(frame, nan=0.0, posinf=255.0, neginf=0.0)
            frame = np.clip(frame, 0, 255).astype(np.uint8)
        return np.ascontiguousarray(frame)

    def process_frame(
        self, frame_bgr: Any, timestamp_ms: int | float | None = None
    ) -> DetectionResult:
        """处理一帧 BGR 图像并返回四路信号与判决结果。"""

        if timestamp_ms is None:
            timestamp = int(round(self._frame_index * 1000.0 / self.fps))
        else:
            timestamp = int(round(float(timestamp_ms)))
        self._frame_index += 1

        if (
            self._last_timestamp_ms is not None
            and timestamp < self._last_timestamp_ms
        ):
            self._candidate_start_ms = None
            self._candidate_latched = False
        self._last_timestamp_ms = timestamp

        frame = self._normalize_frame(frame_bgr)
        if frame is None:
            self._candidate_start_ms = None
            self._candidate_latched = False
            return self._empty_result(timestamp)

        frame_h, frame_w = frame.shape[:2]
        contact_roi = clip_roi(
            select_roi_profile(self.config.get("contact_roi"), frame.shape),
            frame.shape,
        )
        reference_roi = clip_roi(
            select_roi_profile(self.config.get("reference_roi"), frame.shape),
            frame.shape,
        )
        falling_roi = clip_roi(
            select_roi_profile(self.config.get("falling_roi"), frame.shape),
            frame.shape,
        )
        if contact_roi is None or reference_roi is None or falling_roi is None:
            self._candidate_start_ms = None
            self._candidate_latched = False
            return self._empty_result(timestamp)

        hsv = cv2.cvtColor(frame, cv2.COLOR_BGR2HSV)
        v_channel = hsv[:, :, 2]

        contact_v = v_channel[
            contact_roi[1] : contact_roi[3], contact_roi[0] : contact_roi[2]
        ]
        reference_v = v_channel[
            reference_roi[1] : reference_roi[3],
            reference_roi[0] : reference_roi[2],
        ]
        falling_v = v_channel[
            falling_roi[1] : falling_roi[3], falling_roi[0] : falling_roi[2]
        ]

        contact_area = int(contact_v.size)
        reference_area = int(reference_v.size)
        contact_sat_pixels = int(
            np.count_nonzero(contact_v >= self.v_sat_threshold)
        )
        reference_sat_pixels = int(
            np.count_nonzero(reference_v >= self.v_sat_threshold)
        )
        contact_sat_ratio = (
            float(contact_sat_pixels) / float(contact_area)
            if contact_area
            else 0.0
        )
        ref_sat_ratio = (
            float(reference_sat_pixels) / float(reference_area)
            if reference_area
            else 0.0
        )
        rel_saturation = (
            contact_sat_ratio - ref_sat_ratio
            if self.use_relative
            else contact_sat_ratio
        )

        contact_v_mean = float(np.mean(contact_v)) if contact_v.size else 0.0
        reference_v_mean = (
            float(np.mean(reference_v)) if reference_v.size else 0.0
        )
        self._contact_v_history.append(contact_v_mean)
        self._reference_v_history.append(reference_v_mean)
        contact_flicker = _mean_frame_delta(self._contact_v_history)
        ref_flicker = _mean_frame_delta(self._reference_v_history)
        rel_flicker = contact_flicker - ref_flicker

        contact_hsv = hsv[
            contact_roi[1] : contact_roi[3], contact_roi[0] : contact_roi[2]
        ]
        saturated_mask = contact_v >= self.v_sat_threshold
        saturated_count = int(np.count_nonzero(saturated_mask))
        color_match_ratio = 0.0
        color_type = "none"
        blue_count = 0
        orange_count = 0
        if saturated_count:
            hue = contact_hsv[:, :, 0]
            saturation = contact_hsv[:, :, 1]
            blue_mask = (
                saturated_mask
                & _hue_in_range(hue, self.config.get("hue_blue_white", [100, 130]))
                & (saturation <= 120)
            )
            orange_mask = (
                saturated_mask
                & _hue_in_range(
                    hue, self.config.get("hue_orange_yellow", [5, 25])
                )
                & (saturation >= 80)
            )
            blue_count = int(np.count_nonzero(blue_mask))
            orange_count = int(np.count_nonzero(orange_mask))
            color_match_ratio = float(blue_count + orange_count) / float(
                saturated_count
            )
            if blue_count and orange_count:
                color_type = (
                    "blue_white" if blue_count >= orange_count else "orange_yellow"
                )
            elif blue_count:
                color_type = "blue_white"
            elif orange_count:
                color_type = "orange_yellow"
        color_hit = color_match_ratio >= self.color_match_threshold

        falling_bright = (falling_v >= self.v_sat_threshold).astype(np.uint8)
        falling_bright_pixels = int(np.count_nonzero(falling_bright))
        if falling_bright_pixels:
            falling_components = max(
                0, int(cv2.connectedComponents(falling_bright, connectivity=8)[0]) - 1
            )
        else:
            falling_components = 0

        candidate = (
            rel_saturation >= self.rel_saturation_threshold
            and rel_flicker >= self.flicker_threshold
            and color_hit
        )

        candidate_duration_ms = 0
        event_triggered = False
        if candidate:
            if self._candidate_start_ms is None:
                self._candidate_start_ms = timestamp
            candidate_duration_ms = max(
                0, timestamp - self._candidate_start_ms
            )
            if (
                candidate_duration_ms >= self.confirm_duration_ms
                and not self._candidate_latched
            ):
                event_triggered = True
                self._candidate_latched = True
        else:
            self._candidate_start_ms = None
            self._candidate_latched = False

        heavy_level = (
            falling_bright_pixels >= self.heavy_bright_pixels
            or falling_components >= self.heavy_components
        )
        if event_triggered:
            level = "heavy" if heavy_level else "light"
        else:
            level = "none"

        return DetectionResult(
            timestamp_ms=timestamp,
            rel_saturation=float(rel_saturation),
            contact_sat_ratio=float(contact_sat_ratio),
            ref_sat_ratio=float(ref_sat_ratio),
            rel_flicker=float(rel_flicker),
            contact_flicker=float(contact_flicker),
            ref_flicker=float(ref_flicker),
            color_match_ratio=float(color_match_ratio),
            color_hit=bool(color_hit),
            color_type=color_type,
            falling_bright_pixels=falling_bright_pixels,
            falling_components=falling_components,
            candidate=bool(candidate),
            event_triggered=event_triggered,
            level=level,
            contact_v_mean=contact_v_mean,
            reference_v_mean=reference_v_mean,
            candidate_duration_ms=candidate_duration_ms,
        )


__all__ = [
    "DetectionResult",
    "SparkDetector",
    "clip_roi",
    "select_roi_profile",
]
