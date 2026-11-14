#!/usr/bin/env python3
import asyncio
import atexit
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path
from types import SimpleNamespace

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
FRT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "frt"

tmp = Path(tempfile.mkdtemp(prefix="frt-bot-test-"))
atexit.register(shutil.rmtree, tmp, ignore_errors=True)
os.environ.update(DISCORD_TOKEN="test", FRT_DB=str(tmp / "test.db"), FRT_IMAGE_DIR=str(tmp / "images"),
                  FRT_BIN=str(FRT))
subprocess.run([str(FRT), "--db", str(tmp / "test.db"), "import", str(HERE / "fixture.txt")],
               check=True, stdout=subprocess.DEVNULL)

sys.path.insert(0, str(ROOT / "bot"))
import bot as B


def check_embed(e, where):
    assert len(e) <= 6000, (where, "embed total", len(e))
    assert e.title is None or len(e.title) <= 256, where
    assert e.description is None or len(e.description) <= 4096, where
    assert len(e.fields) <= 25, where
    for f in e.fields:
        assert 0 < len(f.value) <= 1024 and len(f.name) <= 256, (where, f.name)


def check_view(v, where):
    rows = {}
    for it in v.children:
        is_select = hasattr(it, "options")
        rows[it.row] = rows.get(it.row, 0) + (5 if is_select else 1)
        if is_select:
            assert 1 <= len(it.options) <= 25, where
            assert all(len(o.label) <= 100 and len(o.value) <= 100 for o in it.options), where
        else:
            assert len(it.label) <= 80, where
    assert all(n <= 5 for n in rows.values()), (where, rows)
    assert len(rows) <= 5, where


def png() -> bytes:
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"\x00\xff\x00\x00")) + chunk(b"IEND", b""))


async def main():
    img = B.store.add(900001, png(), "test caption", 42)
    try:
        B.store.add(900001, b"%PDF-1.4", None, 42)
        raise AssertionError("non-image accepted")
    except ValueError:
        pass
    assert [i.caption for i in B.store.list(900001)] == ["test caption"]

    pages = 0
    for frn in (900001, 900002):
        rec = B.db.record(frn)
        assert rec, frn
        view = B.RecordView(rec, owner_id=1)
        for key, _, _ in view.SECTIONS:
            view.section = key
            for i in range(len(view.pages[key])):
                view.idx = i
                view.rebuild()
                check_embed(view.embed, (frn, key, i))
                check_view(view, (frn, key, i))
                files = view.files()
                assert bool(files) == (key == "summary" and frn == 900001), (frn, key, files)
                pages += 1
        view.stop()

    res = B.db.search(["acme"])
    assert {r["frn"] for r in res} == {900001, 900002}, res
    v = B.RecordView(B.db.record(900001), owner_id=1)
    assert v.embed.image.url == "attachment://image.png" and "Image 1 of 1" in v.embed.fields[-1].name
    assert v.files()[0].filename == "image.png"
    empty = B.RecordView(B.db.record(900002), owner_id=1)
    assert not empty.embed.image.url and "No image yet" in empty.embed.fields[-1].name
    assert "/image add frn:900002" in empty.embed.fields[-1].value
    empty.stop()
    v.stop()
    sv = B.SearchView("acme", res, owner_id=1, truncated=False)
    check_embed(sv.embed, "search")
    check_view(sv, "search")
    assert "📷1" in sv.embed.description
    assert B.db.suggest("9000")[0]["frn"] == 900001
    assert {r["frn"] for r in B.db.search([], make="acme test arms")} == {900001, 900002}
    assert [r["frn"] for r in B.db.search([], make="Acme", type_="Rifle")] == [900001]
    assert B.db.search([], manufacturer="nobody") == []
    assert B.db.values("make", "acm") == [("Acme Test Arms", 2)]
    assert [c.value for c in await B.value_autocomplete("manufacturer")(None, "test")] == ["Acme Test Arms Ltd"]

    for cmd in B.bot.tree.get_commands():
        cmd.to_dict(B.bot.tree)

    assert [r["frn"] for r in B.db.search([], calibre="12 ga x 2 3/4\"")] == [900001]
    assert [r["frn"] for r in B.db.search([], calibre="6.5X55 SWEDISH MAUSER")] == [900001]
    assert [r["frn"] for r in B.db.search([], calibre="5.56")] == [900001]
    assert B.db.search([], calibre="9MM LUGER") == []
    assert B.db.calibres("5.56") == [("5.56MM NATO", 1)]
    assert B.db.existing([900001, 900002, 123]) == {900001, 900002}
    assert B.store.totals() == (1, 1)
    sent = []

    class Resp:
        done = False
        def is_done(self): return self.done
        async def send_message(self, *a, **k): self.done = True; sent.append(k.get("content") or a[0])
        async def defer(self, **k): self.done = True

    class Followup:
        async def send(self, *a, **k): sent.append(k.get("content") or (a[0] if a else ""))

    class Attachment:
        size, filename = 100, "x.png"
        async def read(self): return png()

    def interaction():
        i = SimpleNamespace(user=SimpleNamespace(id=7), response=Resp(), followup=Followup())
        async def original(): return None
        i.original_response = original
        return i

    B.can_upload = lambda i: True
    await B.image_add.callback(interaction(), "900002", Attachment(), None, "900001, 900002 900001")
    assert B.store.count(900002) == 1 and B.store.count(900001) == 2, sent
    assert "Also added to 1 more FRN: 900001" in sent[-1], sent
    await B.image_add.callback(interaction(), "900002", Attachment(), None, "900001 555")
    assert "Not in the database: 555" in sent[-1] and B.store.count(900002) == 1, sent
    for frn in (900001, 900002):
        for i in B.store.list(frn):
            if i.name != img.name:
                B.store.remove(frn, i.name)
    B.store.remove(900001, img.name)
    assert B.store.list(900001) == []
    print(f"ok   bot: {pages} record pages, search, images, commands")


asyncio.run(main())
