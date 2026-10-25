"""Channel layouts and BS.1770 channel weights (spec §3.11).

A layout is an ordered list of channel roles, one per input channel. The LFE has weight 0:
it is neither filtered nor measured (loudness and true peak). Multichannel sources without
a channel mask are never guessed silently: `resolve_layout` raises and names the most
plausible layout, which the caller applies only after the user passed or confirmed it.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

SURROUND_WEIGHT = 1.41  # +1.5 dB, BS.1770
ROLE_WEIGHTS = {"L": 1.0, "R": 1.0, "C": 1.0, "LFE": 0.0,
                "Ls": SURROUND_WEIGHT, "Rs": SURROUND_WEIGHT,
                "Lb": SURROUND_WEIGHT, "Rb": SURROUND_WEIGHT}

STANDARD_ROLES = {
    "mono": ("C",),
    "stereo": ("L", "R"),
    "5.0": ("L", "R", "C", "Ls", "Rs"),
    "5.1": ("L", "R", "C", "LFE", "Ls", "Rs"),
    "7.1": ("L", "R", "C", "LFE", "Lb", "Rb", "Ls", "Rs"),
}
SUGGESTIONS = {5: "5.0", 6: "5.1", 8: "7.1"}

# WAVE_FORMAT_EXTENSIBLE dwChannelMask bits -> speaker (FFmpeg names)
_MASK_SPEAKERS = {0x1: "FL", 0x2: "FR", 0x4: "FC", 0x8: "LFE", 0x10: "BL", 0x20: "BR",
                  0x200: "SL", 0x400: "SR"}


@dataclass(frozen=True)
class ChannelLayout:
    roles: tuple[str, ...]
    name: str = "custom"

    def __post_init__(self):
        unknown = [r for r in self.roles if r not in ROLE_WEIGHTS]
        if unknown:
            raise ValueError(f"unknown channel role(s) {unknown}; "
                             f"valid roles: {', '.join(ROLE_WEIGHTS)}")
        if len(set(self.roles)) != len(self.roles):
            raise ValueError(f"channel roles must be unique, got {','.join(self.roles)}")
        if all(r == "LFE" for r in self.roles):
            raise ValueError("a layout needs at least one channel besides the LFE")

    @property
    def channels(self) -> int:
        return len(self.roles)

    @property
    def weights(self) -> np.ndarray:
        """Per input channel; 0 marks a channel that is not measured (LFE)."""
        return np.array([ROLE_WEIGHTS[r] for r in self.roles])

    @property
    def measured(self) -> np.ndarray:
        """Indices of the measured (non-LFE) input channels."""
        return np.flatnonzero(self.weights > 0)

    @property
    def role_list(self) -> str:
        return ",".join(self.roles)


def get_layout(name: str) -> ChannelLayout:
    key = name.strip().lower()
    if key not in STANDARD_ROLES:
        raise ValueError(f"unknown layout {name!r}; standard layouts: "
                         f"{', '.join(STANDARD_ROLES)}, or a role list like L,R,C,LFE,Ls,Rs")
    return ChannelLayout(STANDARD_ROLES[key], key)


def parse_layout(text: str) -> ChannelLayout:
    """A standard layout name, or a comma-separated role list in file channel order."""
    text = text.strip()
    if text.lower() in STANDARD_ROLES:
        return get_layout(text)
    roles = tuple(r.strip() for r in text.split(",") if r.strip())
    if not roles:
        raise ValueError("empty channel layout")
    for name, std in STANDARD_ROLES.items():
        if roles == std:
            return ChannelLayout(roles, name)
    return ChannelLayout(roles)


def _from_speakers(speakers: list[str]) -> ChannelLayout | None:
    """Speaker names (stream order, FFmpeg names) -> a standard layout, else None."""
    side = "SL" in speakers
    roles = []
    for s in speakers:
        role = {"FL": "L", "FR": "R", "FC": "C", "LFE": "LFE", "SL": "Ls", "SR": "Rs"}.get(s)
        if s in ("BL", "BR"):  # a back pair is the surround pair unless side channels exist too
            left = s == "BL"
            role = ("Lb" if left else "Rb") if side else ("Ls" if left else "Rs")
        if role is None:
            return None
        roles.append(role)
    for name, std in STANDARD_ROLES.items():
        if tuple(roles) == std:
            return ChannelLayout(std, name)
    return None


def layout_from_mask(mask: int) -> ChannelLayout | None:
    """Layout for a WAVE_FORMAT_EXTENSIBLE channel mask; None if it is not a supported one."""
    if mask <= 0 or mask & ~sum(_MASK_SPEAKERS):
        return None
    return _from_speakers([name for bit, name in sorted(_MASK_SPEAKERS.items()) if mask & bit])


def layout_from_channel_names(names: list[str]) -> ChannelLayout | None:
    """Layout from FFmpeg channel names (FL, FR, FC, LFE, BL, BR, SL, SR) in stream order."""
    return _from_speakers(list(names))


def suggest_layout(channels: int) -> ChannelLayout | None:
    """Most plausible layout for a channel count without layout information (5, 6, 8 only)."""
    name = SUGGESTIONS.get(channels)
    return get_layout(name) if name else None


def resolve_layout(channels: int, detected: ChannelLayout | None = None,
                   override: ChannelLayout | str | None = None) -> ChannelLayout:
    """Layout for a source: user override > container information > mono / stereo by count.

    Anything else raises ValueError naming the suggestion (never applied silently).
    """
    if override is not None:
        layout = parse_layout(override) if isinstance(override, str) else override
    elif channels == 1:
        layout = get_layout("mono")
    elif channels == 2:
        layout = get_layout("stereo")
    elif detected is not None and detected.channels == channels:
        layout = detected
    else:
        sug = suggest_layout(channels)
        hint = (f"; the likely layout is {sug.name} ({sug.role_list}): pass it with "
                f"--layout {sug.name} (or confirm it in the app) to analyze this source"
                if sug else
                "; supported layouts: mono, stereo, 5.0, 5.1, 7.1 "
                "(pass a role list such as L,R,C,LFE,Ls,Rs if the order is known)")
        raise ValueError(f"{channels}-channel source without a channel layout{hint}")
    if layout.channels != channels:
        raise ValueError(f"layout {layout.name} ({layout.role_list}) has {layout.channels} "
                         f"channels but the source has {channels} channels")
    return layout
