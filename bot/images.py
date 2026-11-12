from __future__ import annotations

import json
import os
import secrets
import threading
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

EXTENSIONS = {".png", ".jpg", ".jpeg", ".gif", ".webp"}
CAPTIONS = "captions.json"
_lock = threading.Lock()


@dataclass
class Image:
    path: Path
    caption: str | None
    added_by: int | None
    added_at: str | None

    @property
    def name(self) -> str:
        return self.path.name


def sniff(data: bytes) -> str | None:
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        return ".png"
    if data.startswith(b"\xff\xd8\xff"):
        return ".jpg"
    if data[:6] in (b"GIF87a", b"GIF89a"):
        return ".gif"
    if data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        return ".webp"
    return None


class ImageStore:
    def __init__(self, root: str | Path):
        self.root = Path(root).resolve()

    def folder(self, frn: int) -> Path:
        return self.root / str(int(frn))

    def _captions(self, frn: int) -> dict:
        try:
            return json.loads((self.folder(frn) / CAPTIONS).read_text())
        except (OSError, ValueError):
            return {}

    def _save_captions(self, frn: int, data: dict) -> None:
        f = self.folder(frn) / CAPTIONS
        if not data:
            f.unlink(missing_ok=True)
            return
        tmp = f.with_suffix(".tmp")
        tmp.write_text(json.dumps(data, indent=2))
        os.replace(tmp, f)

    def list(self, frn: int) -> list[Image]:
        d = self.folder(frn)
        if not d.is_dir():
            return []
        meta = self._captions(frn)
        files = sorted(p for p in d.iterdir() if p.is_file() and p.suffix.lower() in EXTENSIONS)
        return [Image(p, meta.get(p.name, {}).get("caption"), meta.get(p.name, {}).get("added_by"),
                      meta.get(p.name, {}).get("added_at")) for p in files]

    def count(self, frn: int) -> int:
        d = self.folder(frn)
        if not d.is_dir():
            return 0
        return sum(1 for p in d.iterdir() if p.is_file() and p.suffix.lower() in EXTENSIONS)

    def add(self, frn: int, data: bytes, caption: str | None, added_by: int | None) -> Image:
        ext = sniff(data)
        if not ext:
            raise ValueError("not a PNG, JPEG, GIF or WebP image")
        with _lock:
            d = self.folder(frn)
            d.mkdir(parents=True, exist_ok=True)
            now = datetime.now(timezone.utc)
            path = d / f"{now:%Y%m%d-%H%M%S}-{secrets.token_hex(3)}{ext}"
            path.write_bytes(data)
            meta = self._captions(frn)
            meta[path.name] = {"caption": caption or None, "added_by": added_by,
                               "added_at": now.isoformat(timespec="seconds")}
            self._save_captions(frn, meta)
        return Image(path, caption, added_by, meta[path.name]["added_at"])

    def set_caption(self, frn: int, name: str, caption: str | None) -> None:
        with _lock:
            meta = self._captions(frn)
            meta.setdefault(name, {})["caption"] = caption or None
            self._save_captions(frn, meta)

    def remove(self, frn: int, name: str) -> None:
        with _lock:
            img = next((i for i in self.list(frn) if i.name == name), None)
            if not img:
                raise FileNotFoundError(name)
            img.path.unlink()
            meta = self._captions(frn)
            meta.pop(name, None)
            self._save_captions(frn, meta)
            d = self.folder(frn)
            if not any(d.iterdir()):
                d.rmdir()
