from __future__ import annotations

import asyncio
import logging
import os
import re
import time
from pathlib import Path

import discord
from discord import app_commands
from discord.ext import tasks
from discord.utils import escape_markdown as esc

from frtdb import SUMMARY_FIELDS, FrtDB, Record
from images import Image, ImageStore

ROOT = Path(__file__).resolve().parent.parent


def load_env(path: Path) -> None:
    if not path.exists():
        return
    for line in path.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            os.environ.setdefault(k.strip(), v.strip().strip('"').strip("'"))


load_env(Path(__file__).with_name(".env"))

TOKEN = os.environ.get("DISCORD_TOKEN", "")
DB_PATH = Path(os.environ.get("FRT_DB", ROOT / "frt.db"))
FRT_BIN = Path(os.environ.get("FRT_BIN", ROOT / "frt"))
REPORT_DIR = Path(os.environ.get("FRT_REPORT_DIR", ROOT / "frt-report"))
GUILD_ID = int(os.environ["FRT_GUILD_ID"]) if os.environ.get("FRT_GUILD_ID") else None
ADMIN_IDS = {int(x) for x in re.split(r"[,\s]+", os.environ.get("FRT_ADMIN_IDS", "")) if x}
ANNOUNCE_CHANNEL = int(os.environ["FRT_ANNOUNCE_CHANNEL_ID"]) if os.environ.get("FRT_ANNOUNCE_CHANNEL_ID") else None
AUTO_IMPORT = os.environ.get("FRT_AUTO_IMPORT", "0") == "1"
WATCH_MINUTES = float(os.environ.get("FRT_WATCH_MINUTES", "10"))
IMAGE_DIR = Path(os.environ.get("FRT_IMAGE_DIR", ROOT / "images"))
IMAGE_MAX_MB = float(os.environ.get("FRT_IMAGE_MAX_MB", "8"))
IMAGE_UPLOADERS = os.environ.get("FRT_IMAGE_UPLOADERS", "admins").lower()

log = logging.getLogger("frt-bot")
db = FrtDB(DB_PATH)
store = ImageStore(IMAGE_DIR)

DISCLAIMER = "Not a legal instrument; verify with the RCMP"
CLASS_COLORS = {
    "Prohibited": 0xC0392B,
    "Restricted": 0xE67E22,
    "Non-Restricted": 0x27AE60,
    "Antique": 0x2980B9,
}
CLASS_CHOICES = [
    app_commands.Choice(name=c, value=c)
    for c in ("Prohibited", "Restricted", "Non-Restricted", "Antique", 'CC 2 "firearm"/CC 84(3) Exempted')
]
TYPE_CHOICES = [
    app_commands.Choice(name=t, value=t)
    for t in ("Rifle", "Handgun", "Shotgun", "Machine Gun", "Submachine Gun", "Combination Gun",
              "Commercial Version", "Other", "Grenade Launcher", "Airsoft", "Cannon", "Mortar",
              "Receiver Blank", "Paintball")
]
KIND_CHOICES = [app_commands.Choice(name=k, value=k) for k in ("added", "modified", "removed", "restored")]
VIEW_TIMEOUT = 840


def clip(s: str | None, n: int) -> str:
    s = s or ""
    return s if len(s) <= n else s[: n - 1] + "…"


def paginate(lines: list[str], max_chars: int = 3800, max_lines: int = 40) -> list[str]:
    pages, cur, size = [], [], 0
    for line in lines:
        if cur and (size + len(line) + 1 > max_chars or len(cur) >= max_lines):
            pages.append("\n".join(cur))
            cur, size = [], 0
        cur.append(line)
        size += len(line) + 1
    if cur:
        pages.append("\n".join(cur))
    return pages or ["*No Data*"]


def record_color(rec: Record) -> int:
    if rec.removed:
        return 0x555555
    return CLASS_COLORS.get(rec.summary.get("legal_classification") or "", 0x7F8C8D)


def record_embed(rec: Record, section: str, description: str | None = None) -> discord.Embed:
    title = f"FRN {rec.frn} · {rec.title}"
    if rec.removed:
        title = "⚠ REMOVED · " + title
    e = discord.Embed(title=clip(title, 256), description=description, color=record_color(rec))
    e.set_author(name=section)
    e.set_footer(text=f"FRT valid as of {rec.last_report or '?'} · PDF page {rec.pdf_page or '?'} · {DISCLAIMER}")
    return e


def summary_pages(rec: Record) -> list[discord.Embed]:
    e = record_embed(rec, "Summary")
    for key, label in SUMMARY_FIELDS:
        val = rec.summary.get(key) or "—"
        e.add_field(name=label, value=clip(esc(val), 1024), inline=key not in ("level", "legal_classification"))
    classes = sorted({c["legal_classification"] for c in rec.calibres if c["legal_classification"]})
    if classes:
        e.add_field(name=f"Calibre rows ({len(rec.calibres)})", value=clip(", ".join(classes), 1024), inline=False)
    e.add_field(name="Reports",
                value=f"first {rec.first_report} · last {rec.last_report} · changed {rec.changed_report}",
                inline=False)
    return [e]


def calibre_pages(rec: Record) -> list[discord.Embed]:
    blocks = []
    for c in rec.calibres:
        line = (f"{c['sub']:<3} {clip(c['calibre'], 26):<26} {c['shots'] or '':>4} "
                f"{(c['barrel_mm'] or '') + 'mm':>7}  {c['legal_classification'] or ''}")
        extra = " · ".join(x for x in (c["legal_authority"], c["level"], c["barrel_type_code"]) if x)
        blocks.append(line + ("\n    " + clip(extra, 160) if extra else ""))
    if not blocks:
        return [record_embed(rec, "Calibre, Shots and Barrel Length", "*No Data*")]
    head = f"{'#':<3} {'Calibre':<26} {'Shot':>4} {'Barrel':>7}  Class"
    pages = []
    for i in range(0, len(blocks), 12):
        body = "\n".join(blocks[i:i + 12])
        pages.append(record_embed(rec, "Calibre, Shots and Barrel Length", f"```\n{head}\n{body}\n```"))
    return pages


def notes_pages(rec: Record) -> list[discord.Embed]:
    lines, last = [], None
    for n in rec.notes:
        if n["category"] != last:
            last = n["category"]
            lines.append(f"\n**{esc(last or 'Notes')}**")
        lines.append(f"• {esc(clip(n['text'], 1800))}")
    return [record_embed(rec, "Notes", p.strip()) for p in paginate(lines)]


def xref_lines(rec: Record) -> list[str]:
    return [f"`{x['ref_frn']}` **{esc(x['make'] or '')} {esc(x['model'] or '')}**"
            f" · {esc(x['manufacturer'] or '—')} · {x['type'] or ''} / {x['action'] or ''}"
            for x in rec.xrefs]


XREF_PAGE = 15


def xref_pages(rec: Record) -> list[discord.Embed]:
    lines = xref_lines(rec)
    if not lines:
        return [record_embed(rec, "Cross-References", "*No Data*")]
    return [record_embed(rec, "Cross-References", "\n".join(lines[i:i + XREF_PAGE]))
            for i in range(0, len(lines), XREF_PAGE)]


def list_pages(rec: Record) -> list[discord.Embed]:
    lines = []
    for title, items in (("Also Known As / Product Code", rec.aliases),
                         ("Year Dates", rec.year_dates), ("Importer", rec.importers)):
        lines.append(f"\n**{title}**")
        lines += [f"• {esc(x)}" for x in items] or ["*No Data*"]
    return [record_embed(rec, "Aliases · Year Dates · Importers", p.strip()) for p in paginate(lines)]


def history_pages(rec: Record) -> list[discord.Embed]:
    lines = []
    for h in rec.history:
        s = f"`{h['valid_as_of']}` **{h['kind']}**"
        if h["field"]:
            s += f" {h['field']}"
        if h["kind"] == "modified" and (h["old_value"] or h["new_value"]):
            s += f": {esc(clip(h['old_value'], 300))} → {esc(clip(h['new_value'], 300))}"
        lines.append(s)
    if not lines:
        lines = ["*No changes recorded since this FRN was first imported.*"]
    return [record_embed(rec, "History", p) for p in paginate(lines)]


def attachment_name(img: Image) -> str:
    return "image" + img.path.suffix.lower()


def summary_with_images(rec: Record, images: list[Image]) -> list[discord.Embed]:
    if not images:
        e = summary_pages(rec)[0]
        e.add_field(name="📷 No image yet",
                    value=f"Have a photo of this firearm? It helps others identify it, so please add it"
                          f" with `/image add frn:{rec.frn}`.",
                    inline=False)
        return [e]
    pages = []
    for i, img in enumerate(images, 1):
        e = summary_pages(rec)[0]
        meta = []
        if img.added_by:
            meta.append(f"added by <@{img.added_by}>")
        if img.added_at:
            meta.append(f"on {img.added_at[:10]}")
        meta.append(f"`{img.name}`")
        value = (f"**{esc(img.caption)}**\n" if img.caption else "") + " · ".join(meta)
        e.add_field(name=f"📷 Image {i} of {len(images)}", value=clip(value, 1024), inline=False)
        e.set_image(url=f"attachment://{attachment_name(img)}")
        pages.append(e)
    return pages


class OwnedView(discord.ui.View):

    def __init__(self, owner_id: int):
        super().__init__(timeout=VIEW_TIMEOUT)
        self.owner_id = owner_id
        self.message: discord.InteractionMessage | discord.Message | None = None

    async def interaction_check(self, interaction: discord.Interaction) -> bool:
        if interaction.user.id != self.owner_id:
            await interaction.response.send_message("Run the command yourself to browse.", ephemeral=True)
            return False
        return True

    async def on_timeout(self) -> None:
        for item in self.children:
            item.disabled = True
        if self.message:
            try:
                await self.message.edit(view=self)
            except discord.HTTPException:
                pass

    def add_button(self, label: str, callback, *, row: int, style=discord.ButtonStyle.secondary,
                   disabled: bool = False) -> None:
        b = discord.ui.Button(label=label, style=style, row=row, disabled=disabled)
        b.callback = callback
        self.add_item(b)

    def files(self) -> list[discord.File]:
        return []

    async def render(self, interaction: discord.Interaction, view: "OwnedView | None" = None) -> None:
        v = view or self
        await interaction.response.edit_message(embed=v.embed, view=v, attachments=v.files())


class RecordView(OwnedView):
    SECTIONS = [
        ("summary", "Summary", summary_pages), ("calibres", "Calibres", calibre_pages),
        ("notes", "Notes", notes_pages), ("xrefs", "Cross-refs", xref_pages),
        ("lists", "AKA / Importers", list_pages), ("history", "History", history_pages),
    ]

    def __init__(self, rec: Record, owner_id: int, back: "SearchView | None" = None):
        super().__init__(owner_id)
        self.rec, self.back = rec, back
        self.images = store.list(rec.frn)
        self.pages = {key: build(rec) for key, _, build in self.SECTIONS}
        self.pages["summary"] = summary_with_images(rec, self.images)
        self.counts = {
            "calibres": len(rec.calibres), "notes": len(rec.notes), "xrefs": len(rec.xrefs),
            "lists": len(rec.aliases) + len(rec.year_dates) + len(rec.importers),
            "history": len(rec.history),
        }
        self.section, self.idx = "summary", 0
        self.rebuild()

    def files(self) -> list[discord.File]:
        if self.section != "summary" or not self.images:
            return []
        img = self.images[self.idx]
        if not img.path.is_file():
            return []
        return [discord.File(img.path, filename=attachment_name(img))]

    @property
    def embed(self) -> discord.Embed:
        pages = self.pages[self.section]
        e = pages[self.idx]
        if len(pages) > 1:
            e.set_author(name=f"{e.author.name.split(' (')[0]} ({self.idx + 1}/{len(pages)})")
        return e

    def rebuild(self) -> None:
        self.clear_items()
        for i, (key, label, _) in enumerate(self.SECTIONS):
            n = self.counts.get(key)
            text = label if n is None else f"{label} ({n})"
            style = discord.ButtonStyle.primary if key == self.section else discord.ButtonStyle.secondary
            self.add_button(text, self._switch(key), row=i // 3, style=style, disabled=(n == 0))

        pages = self.pages[self.section]
        if len(pages) > 1:
            self.add_button("◀", self._step(-1), row=2, disabled=self.idx == 0)
            label = f"{self.idx + 1} / {len(pages)}"
            if self.section == "summary":
                label = "📷 " + label
            self.add_button(label, self._noop, row=2, disabled=True)
            self.add_button("▶", self._step(1), row=2, disabled=self.idx >= len(pages) - 1)
        if self.back:
            self.add_button("↩ Results", self._go_back, row=2, style=discord.ButtonStyle.success)

        if self.section == "xrefs" and self.rec.xrefs:
            chunk = self.rec.xrefs[self.idx * XREF_PAGE:(self.idx + 1) * XREF_PAGE]
            sel = discord.ui.Select(placeholder="Open a cross-referenced FRN…", row=3, options=[
                discord.SelectOption(label=clip(f"{x['ref_frn']} · {x['make'] or ''} {x['model'] or ''}", 100),
                                     description=clip(f"{x['type'] or ''} / {x['action'] or ''}", 100),
                                     value=str(x["ref_frn"]))
                for x in chunk])
            sel.callback = self._open_xref(sel)
            self.add_item(sel)

    async def show(self, interaction: discord.Interaction) -> None:
        self.rebuild()
        await self.render(interaction)

    def _switch(self, key: str):
        async def cb(interaction: discord.Interaction):
            self.section, self.idx = key, 0
            await self.show(interaction)
        return cb

    def _step(self, delta: int):
        async def cb(interaction: discord.Interaction):
            self.idx = max(0, min(self.idx + delta, len(self.pages[self.section]) - 1))
            await self.show(interaction)
        return cb

    async def _noop(self, interaction: discord.Interaction):
        await interaction.response.defer()

    async def _go_back(self, interaction: discord.Interaction):
        self.stop()
        self.back.message = self.message
        self.back.rebuild()
        await self.render(interaction, self.back)

    def _open_xref(self, sel: discord.ui.Select):
        async def cb(interaction: discord.Interaction):
            frn = int(sel.values[0])
            rec = await asyncio.to_thread(db.record, frn)
            if not rec:
                await interaction.response.send_message(
                    f"FRN {frn} is cross-referenced but has no record of its own in the database.", ephemeral=True)
                return
            view = RecordView(rec, self.owner_id, back=self.back)
            view.message = self.message
            self.stop()
            await self.render(interaction, view)
        return cb


class SearchView(OwnedView):
    PER_PAGE = 10

    def __init__(self, title: str, results: list[dict], owner_id: int, truncated: bool):
        super().__init__(owner_id)
        self.title, self.results, self.truncated, self.idx = title, results, truncated, 0
        self.npages = max(1, (len(results) + self.PER_PAGE - 1) // self.PER_PAGE)
        self.rebuild()

    @property
    def page(self) -> list[dict]:
        return self.results[self.idx * self.PER_PAGE:(self.idx + 1) * self.PER_PAGE]

    @property
    def embed(self) -> discord.Embed:
        lines = [f"`{r['frn']}` **{esc(r['make'] or '')} {esc(r['model'] or '')}**"
                 f" · {r['type'] or ''} · {r['action'] or ''} · *{r['legal_classification'] or '?'}*"
                 + (" ⚠ removed" if r["removed"] else "")
                 + (f" 📷{n}" if (n := store.count(r["frn"])) else "")
                 for r in self.page]
        count = f"{len(self.results)}{'+' if self.truncated else ''} results"
        e = discord.Embed(title=clip(self.title, 256), description="\n".join(lines) or "*No results*",
                          color=0x34495E)
        e.set_footer(text=f"{count} · page {self.idx + 1}/{self.npages} · {DISCLAIMER}")
        return e

    def rebuild(self) -> None:
        self.clear_items()
        if self.page:
            sel = discord.ui.Select(placeholder="Open a record…", row=0, options=[
                discord.SelectOption(label=clip(f"{r['frn']} · {r['make'] or ''} {r['model'] or ''}", 100),
                                     description=clip(f"{r['type'] or ''} · {r['legal_classification'] or ''}", 100),
                                     value=str(r["frn"]))
                for r in self.page])
            sel.callback = self._open(sel)
            self.add_item(sel)
        if self.npages > 1:
            self.add_button("◀", self._step(-1), row=1, disabled=self.idx == 0)
            self.add_button(f"{self.idx + 1} / {self.npages}", self._noop, row=1, disabled=True)
            self.add_button("▶", self._step(1), row=1, disabled=self.idx >= self.npages - 1)

    def _step(self, delta: int):
        async def cb(interaction: discord.Interaction):
            self.idx = max(0, min(self.idx + delta, self.npages - 1))
            self.rebuild()
            await self.render(interaction)
        return cb

    async def _noop(self, interaction: discord.Interaction):
        await interaction.response.defer()

    def _open(self, sel: discord.ui.Select):
        async def cb(interaction: discord.Interaction):
            rec = await asyncio.to_thread(db.record, int(sel.values[0]))
            if not rec:
                await interaction.response.send_message("That record no longer exists.", ephemeral=True)
                return
            view = RecordView(rec, self.owner_id, back=self)
            view.message = self.message
            await self.render(interaction, view)
        return cb


class PagedView(OwnedView):
    def __init__(self, embeds: list[discord.Embed], owner_id: int):
        super().__init__(owner_id)
        self.embeds, self.idx = embeds, 0
        self.rebuild()

    @property
    def embed(self) -> discord.Embed:
        return self.embeds[self.idx]

    def rebuild(self) -> None:
        self.clear_items()
        if len(self.embeds) > 1:
            self.add_button("◀", self._step(-1), row=0, disabled=self.idx == 0)
            self.add_button(f"{self.idx + 1} / {len(self.embeds)}", self._noop, row=0, disabled=True)
            self.add_button("▶", self._step(1), row=0, disabled=self.idx >= len(self.embeds) - 1)

    def _step(self, delta: int):
        async def cb(interaction: discord.Interaction):
            self.idx = max(0, min(self.idx + delta, len(self.embeds) - 1))
            self.rebuild()
            await self.render(interaction)
        return cb

    async def _noop(self, interaction: discord.Interaction):
        await interaction.response.defer()


async def send_view(interaction: discord.Interaction, view: OwnedView, ephemeral: bool = False) -> None:
    kwargs = {"embed": view.embed, "ephemeral": ephemeral}
    if view.children:
        kwargs["view"] = view
    if files := view.files():
        kwargs["files"] = files
    if interaction.response.is_done():
        view.message = await interaction.followup.send(wait=True, **kwargs)
    else:
        await interaction.response.send_message(**kwargs)
        view.message = await interaction.original_response()


class FrtBot(discord.Client):
    def __init__(self):
        super().__init__(intents=discord.Intents.default())
        self.tree = app_commands.CommandTree(self)
        self.import_lock = asyncio.Lock()
        self.attempted: set[str] = set()

    async def setup_hook(self) -> None:
        if GUILD_ID:
            guild = discord.Object(id=GUILD_ID)
            self.tree.copy_global_to(guild=guild)
            await self.tree.sync(guild=guild)
        else:
            await self.tree.sync()
        if AUTO_IMPORT:
            self.watch_reports.change_interval(minutes=WATCH_MINUTES)
            self.watch_reports.start()

    async def on_ready(self) -> None:
        log.info("logged in as %s (db: %s)", self.user, DB_PATH)


    async def run_import(self, pdf: Path, force: bool = False, progress=None) -> tuple[int, str]:
        args = [str(FRT_BIN), "--db", str(DB_PATH), "import"] + (["--force"] if force else []) + [str(pdf)]
        env = dict(os.environ, FRT_PROGRESS="1")
        proc = await asyncio.create_subprocess_exec(
            *args, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT, env=env, cwd=ROOT)
        out, buf, last = [], b"", 0.0
        while True:
            chunk = await proc.stdout.read(4096)
            if not chunk:
                break
            buf += chunk
            *parts, buf = re.split(rb"[\r\n]", buf)
            for p in parts:
                s = p.decode(errors="replace").strip()
                if not s:
                    continue
                if s.startswith("pages "):
                    if progress and time.monotonic() - last > 5:
                        last = time.monotonic()
                        await progress(s)
                else:
                    out.append(s)
        if buf.strip():
            out.append(buf.decode(errors="replace").strip())
        rc = await proc.wait()
        return rc, "\n".join(out)

    async def announce(self, text: str) -> None:
        if not ANNOUNCE_CHANNEL:
            return
        ch = self.get_channel(ANNOUNCE_CHANNEL) or await self.fetch_channel(ANNOUNCE_CHANNEL)
        rep, changes = await asyncio.to_thread(db.changes)
        e = discord.Embed(title="FRT database updated", description=f"```\n{clip(text, 3900)}\n```", color=0x2ECC71)
        if rep and changes:
            sample = "\n".join(f"`{c['frn']}` {c['kind']} {esc(c['make'] or '')} {esc(c['model'] or '')}"
                               f"{' · ' + c['field'] if c['field'] else ''}" for c in changes[:15])
            e.add_field(name=f"Changes in {rep['valid_as_of']} (first 15, see /changes)",
                        value=clip(sample, 1024), inline=False)
        await ch.send(embed=e)

    @tasks.loop(minutes=10)
    async def watch_reports(self) -> None:
        if self.import_lock.locked() or not REPORT_DIR.is_dir():
            return
        done = await asyncio.to_thread(db.imported_files) if db.exists() else set()
        for pdf in sorted(REPORT_DIR.glob("*.pdf"), key=lambda p: p.stat().st_mtime):
            if pdf.name in done or pdf.name in self.attempted:
                continue
            if time.time() - pdf.stat().st_mtime < 120:
                continue
            self.attempted.add(pdf.name)
            async with self.import_lock:
                log.info("auto-importing %s", pdf)
                rc, text = await self.run_import(pdf)
            log.info("import of %s finished rc=%s\n%s", pdf.name, rc, text)
            await self.announce(text if rc == 0 else f"Import of {pdf.name} FAILED\n{text}")

    @watch_reports.before_loop
    async def _wait_ready(self) -> None:
        await self.wait_until_ready()


bot = FrtBot()


def is_admin(interaction: discord.Interaction) -> bool:
    if ADMIN_IDS:
        return interaction.user.id in ADMIN_IDS
    perms = getattr(interaction.user, "guild_permissions", None)
    return bool(perms and perms.administrator)


async def need_db(interaction: discord.Interaction) -> bool:
    if db.exists():
        return True
    await interaction.response.send_message("No database yet. An admin needs to run `/import` first.",
                                            ephemeral=True)
    return False


async def frn_autocomplete(interaction: discord.Interaction, current: str) -> list[app_commands.Choice[str]]:
    if not current.strip() or not db.exists():
        return []
    rows = await asyncio.to_thread(db.suggest, current)
    return [app_commands.Choice(name=clip(f"{r['frn']} · {r['make'] or ''} {r['model'] or ''}", 100),
                                value=str(r["frn"])) for r in rows[:25]]


@bot.tree.command(name="frn", description="Look up a firearm by FRN, or by make / model / alias")
@app_commands.describe(query="FRN number, or text like 'norinco sks' (pick from the suggestions)",
                       private="Only you can see the reply")
@app_commands.autocomplete(query=frn_autocomplete)
async def frn_cmd(interaction: discord.Interaction, query: str, private: bool = False):
    if not await need_db(interaction):
        return
    q = query.strip()
    if q.isdigit():
        rec = await asyncio.to_thread(db.record, int(q))
        if not rec:
            await interaction.response.send_message(f"FRN {q} not found.", ephemeral=True)
            return
        await send_view(interaction, RecordView(rec, interaction.user.id), ephemeral=private)
        return
    results = await asyncio.to_thread(db.search, q.split(), None, None, False, 501)
    if len(results) == 1:
        rec = await asyncio.to_thread(db.record, results[0]["frn"])
        await send_view(interaction, RecordView(rec, interaction.user.id), ephemeral=private)
    else:
        view = SearchView(f"Search: {q}", results[:500], interaction.user.id, len(results) > 500)
        await send_view(interaction, view, ephemeral=private)


def value_autocomplete(column: str):
    async def complete(interaction: discord.Interaction, current: str) -> list[app_commands.Choice[str]]:
        if not db.exists():
            return []
        rows = await asyncio.to_thread(db.values, column, current)
        return [app_commands.Choice(name=f"{v[:88]} ({n:,})", value=v[:100]) for v, n in rows]
    return complete


@bot.tree.command(name="search", description="Search the Firearms Reference Table")
@app_commands.describe(text="Words that must all match make, model, manufacturer or an alias",
                       classification="Only firearms with this legal classification",
                       type="Only firearms of this type",
                       make="Only firearms with this make",
                       manufacturer="Only firearms from this manufacturer",
                       include_removed="Also show FRNs that were dropped from the latest report",
                       private="Only you can see the reply")
@app_commands.choices(classification=CLASS_CHOICES, type=TYPE_CHOICES)
@app_commands.autocomplete(make=value_autocomplete("make"), manufacturer=value_autocomplete("manufacturer"))
async def search_cmd(interaction: discord.Interaction, text: str = "",
                     classification: app_commands.Choice[str] | None = None,
                     type: app_commands.Choice[str] | None = None, make: str | None = None,
                     manufacturer: str | None = None, include_removed: bool = False, private: bool = False):
    if not await need_db(interaction):
        return
    if not text.strip() and not classification and not type and not make and not manufacturer:
        await interaction.response.send_message("Give some search text or a filter.", ephemeral=True)
        return
    cls = classification.value if classification else None
    typ = type.value if type else None
    results = await asyncio.to_thread(db.search, text.split(), cls, typ, include_removed, 501, make, manufacturer)
    desc = " ".join(x for x in (text, make and f"[make: {make}]", manufacturer and f"[mfr: {manufacturer}]",
                                cls and f"[{cls}]", typ and f"[{typ}]") if x)
    view = SearchView(f"Search: {desc}", results[:500], interaction.user.id, len(results) > 500)
    await send_view(interaction, view, ephemeral=private)


async def report_autocomplete(interaction: discord.Interaction, current: str) -> list[app_commands.Choice[str]]:
    if not db.exists():
        return []
    reps = await asyncio.to_thread(db.reports)
    return [app_commands.Choice(name=f"{r['valid_as_of']} (#{r['id']}, {r['record_count']} records)",
                                value=r["valid_as_of"]) for r in reps if current in r["valid_as_of"]][:25]


@bot.tree.command(name="changes", description="What a report added, modified or removed")
@app_commands.describe(report="Report date (default: latest)", kind="Only this kind of change")
@app_commands.autocomplete(report=report_autocomplete)
@app_commands.choices(kind=KIND_CHOICES)
async def changes_cmd(interaction: discord.Interaction, report: str | None = None,
                      kind: app_commands.Choice[str] | None = None):
    if not await need_db(interaction):
        return
    rep, rows = await asyncio.to_thread(db.changes, report, kind.value if kind else None)
    if not rep:
        await interaction.response.send_message("No such report.", ephemeral=True)
        return
    lines = []
    for c in rows:
        s = f"`{c['frn']}` **{c['kind']}** {esc(clip((c['make'] or '') + ' ' + (c['model'] or ''), 60))}"
        if c["field"]:
            s += f" · {c['field']}"
        if c["kind"] == "modified" and (c["old_value"] or c["new_value"]):
            s += f": {esc(clip(c['old_value'], 120))} → {esc(clip(c['new_value'], 120))}"
        lines.append(s)
    if not lines:
        lines = ["*No changes recorded.* The first import is the baseline."]
    title = f"Changes in report {rep['valid_as_of']}"
    counts = (f"added {rep['added']} · modified {rep['modified']} · removed {rep['removed']}"
              f" · restored {rep['restored']}")
    embeds = []
    for p in paginate(lines, max_lines=25):
        e = discord.Embed(title=title, description=p, color=0x8E44AD)
        e.set_footer(text=counts)
        embeds.append(e)
    await send_view(interaction, PagedView(embeds, interaction.user.id))


@bot.tree.command(name="reports", description="List imported FRT reports")
async def reports_cmd(interaction: discord.Interaction):
    if not await need_db(interaction):
        return
    reps = await asyncio.to_thread(db.reports)
    e = discord.Embed(title="Imported FRT reports", color=0x34495E)
    for r in reps[:25]:
        e.add_field(name=f"#{r['id']} · valid as of {r['valid_as_of']}", inline=False,
                    value=f"{r['record_count']} records, {r['page_count']} pages\n"
                          f"+{r['added']} added · {r['modified']} modified · -{r['removed']} removed"
                          f" · {r['restored']} restored\nimported {r['imported_at']} UTC")
    if not reps:
        e.description = "*None yet*"
    await interaction.response.send_message(embed=e)


@bot.tree.command(name="stats", description="Database summary")
async def stats_cmd(interaction: discord.Interaction):
    if not await need_db(interaction):
        return
    s = await asyncio.to_thread(db.stats)
    with_images, n_images = await asyncio.to_thread(store.totals)
    e = discord.Embed(title="FRT database", color=0x34495E,
                      description=f"**{s['active']:,}** active FRNs · {s['removed']:,} removed · "
                                  f"{s['calibres']:,} calibre rows\nLatest report: **{s['latest']}**")
    e.add_field(name="By classification", value="\n".join(f"{n:,} · {esc(c)}" for c, n in s["by_class"]) or "—")
    e.add_field(name="By type", value="\n".join(f"{n:,} · {esc(t)}" for t, n in s["by_type"]) or "—")
    pct = f" ({with_images / s['active']:.1%})" if s["active"] else ""
    e.add_field(name="📷 Images", inline=False,
                value=f"**{with_images:,}** firearms with images{pct} · {n_images:,} images total")
    e.set_footer(text=DISCLAIMER)
    await interaction.response.send_message(embed=e)


async def pdf_autocomplete(interaction: discord.Interaction, current: str) -> list[app_commands.Choice[str]]:
    if not REPORT_DIR.is_dir():
        return []
    pdfs = sorted(REPORT_DIR.glob("*.pdf"), key=lambda p: p.stat().st_mtime, reverse=True)
    return [app_commands.Choice(name=p.name[:100], value=p.name[:100])
            for p in pdfs if current.lower() in p.name.lower()][:25]


@bot.tree.command(name="import", description="(Admin) import an FRT report PDF from the report folder")
@app_commands.describe(file="PDF in the report folder (default: newest)", force="Re-import / allow older report")
@app_commands.autocomplete(file=pdf_autocomplete)
@app_commands.default_permissions(administrator=True)
async def import_cmd(interaction: discord.Interaction, file: str | None = None, force: bool = False):
    if not is_admin(interaction):
        await interaction.response.send_message("Only bot admins can import reports.", ephemeral=True)
        return
    if bot.import_lock.locked():
        await interaction.response.send_message("An import is already running.", ephemeral=True)
        return
    pdfs = sorted(REPORT_DIR.glob("*.pdf"), key=lambda p: p.stat().st_mtime, reverse=True)
    if file:
        target = REPORT_DIR / Path(file).name
        if not target.is_file():
            await interaction.response.send_message(f"`{file}` not found in the report folder.", ephemeral=True)
            return
    elif pdfs:
        target = pdfs[0]
    else:
        await interaction.response.send_message(f"No PDFs in `{REPORT_DIR}`.", ephemeral=True)
        return

    await interaction.response.send_message(f"Importing `{target.name}`… this takes about a minute.")
    msg = await interaction.original_response()

    async def progress(line: str):
        try:
            await msg.edit(content=f"Importing `{target.name}`…\n`{line}`")
        except discord.HTTPException:
            pass

    async with bot.import_lock:
        bot.attempted.add(target.name)
        rc, text = await bot.run_import(target, force, progress)
    status = "✅ Import finished" if rc == 0 else "❌ Import failed"
    await msg.edit(content=f"{status}: `{target.name}`\n```\n{clip(text, 1800)}\n```")
    if rc == 0 and "skipping" not in text:
        await bot.announce(text)


image_group = app_commands.Group(name="image", description="Pictures attached to FRT records")


def can_upload(interaction: discord.Interaction) -> bool:
    return IMAGE_UPLOADERS == "everyone" or is_admin(interaction)


async def resolve_frn(interaction: discord.Interaction, value: str) -> Record | None:
    if not await need_db(interaction):
        return None
    m = re.match(r"\s*(\d+)", value)
    rec = await asyncio.to_thread(db.record, int(m.group(1))) if m else None
    if not rec:
        await interaction.response.send_message(f"No FRN matching `{clip(value, 50)}`. Pick one from the "
                                                "suggestions or type the number.", ephemeral=True)
    return rec


async def image_autocomplete(interaction: discord.Interaction, current: str) -> list[app_commands.Choice[str]]:
    m = re.match(r"\s*(\d+)", str(getattr(interaction.namespace, "frn", "") or ""))
    if not m:
        return []
    imgs = await asyncio.to_thread(store.list, int(m.group(1)))
    out = []
    for i, img in enumerate(imgs, 1):
        label = f"#{i} · {img.caption or img.name}"
        if current.lower() in label.lower():
            out.append(app_commands.Choice(name=clip(label, 100), value=img.name[:100]))
    return out[:25]


def open_images(rec: Record, owner_id: int, index: int = 0) -> RecordView:
    view = RecordView(rec, owner_id)
    view.section, view.idx = "summary", max(0, min(index, len(view.pages["summary"]) - 1))
    view.rebuild()
    return view


@image_group.command(name="add", description="Attach a picture to an FRN")
@app_commands.describe(frn="FRN number (or start typing a make/model)", image="PNG, JPEG, GIF or WebP",
                       caption="Optional caption, e.g. 'left side markings'")
@app_commands.autocomplete(frn=frn_autocomplete)
async def image_add(interaction: discord.Interaction, frn: str, image: discord.Attachment,
                    caption: app_commands.Range[str, 1, 200] | None = None):
    if not can_upload(interaction):
        await interaction.response.send_message("Only bot admins can add images.", ephemeral=True)
        return
    rec = await resolve_frn(interaction, frn)
    if not rec:
        return
    if image.size > IMAGE_MAX_MB * 1024 * 1024:
        await interaction.response.send_message(
            f"That file is {image.size / 1048576:.1f} MB; the limit is {IMAGE_MAX_MB:g} MB.", ephemeral=True)
        return
    await interaction.response.defer(thinking=True)
    try:
        data = await image.read()
        await asyncio.to_thread(store.add, rec.frn, data, caption, interaction.user.id)
    except ValueError as e:
        await interaction.followup.send(f"Couldn't add `{clip(image.filename, 80)}`: {e}.", ephemeral=True)
        return
    except (discord.HTTPException, OSError) as e:
        log.exception("image upload failed")
        await interaction.followup.send(f"Couldn't save the image: {e}", ephemeral=True)
        return
    log.info("image added to FRN %s by %s", rec.frn, interaction.user)
    view = open_images(rec, interaction.user.id, index=10**9)
    await send_view(interaction, view)


@image_group.command(name="view", description="Open an FRN's summary with its pictures")
@app_commands.describe(frn="FRN number (or start typing a make/model)", private="Only you can see the reply")
@app_commands.autocomplete(frn=frn_autocomplete)
async def image_view(interaction: discord.Interaction, frn: str, private: bool = False):
    rec = await resolve_frn(interaction, frn)
    if rec:
        await send_view(interaction, open_images(rec, interaction.user.id), ephemeral=private)


async def _pick_image(interaction: discord.Interaction, rec: Record, name: str) -> Image | None:
    img = next((i for i in store.list(rec.frn) if i.name == name), None)
    if not img and name.lstrip("#").isdigit():
        imgs = store.list(rec.frn)
        n = int(name.lstrip("#"))
        img = imgs[n - 1] if 1 <= n <= len(imgs) else None
    if not img:
        await interaction.response.send_message("No such image on that FRN.", ephemeral=True)
    return img


@image_group.command(name="remove", description="Delete a picture from an FRN")
@app_commands.describe(frn="FRN number", image="Which image (pick from the list)")
@app_commands.autocomplete(frn=frn_autocomplete, image=image_autocomplete)
async def image_remove(interaction: discord.Interaction, frn: str, image: str):
    rec = await resolve_frn(interaction, frn)
    if not rec or not (img := await _pick_image(interaction, rec, image)):
        return
    if not (is_admin(interaction) or img.added_by == interaction.user.id):
        await interaction.response.send_message("Only an admin or the person who added it can remove it.",
                                                ephemeral=True)
        return
    await asyncio.to_thread(store.remove, rec.frn, img.name)
    log.info("image %s removed from FRN %s by %s", img.name, rec.frn, interaction.user)
    await interaction.response.send_message(f"Removed `{img.name}` from FRN {rec.frn}.", ephemeral=True)


@image_group.command(name="caption", description="Change a picture's caption")
@app_commands.describe(frn="FRN number", image="Which image", caption="New caption (leave empty to clear)")
@app_commands.autocomplete(frn=frn_autocomplete, image=image_autocomplete)
async def image_caption(interaction: discord.Interaction, frn: str, image: str,
                        caption: app_commands.Range[str, 1, 200] | None = None):
    rec = await resolve_frn(interaction, frn)
    if not rec or not (img := await _pick_image(interaction, rec, image)):
        return
    if not (is_admin(interaction) or img.added_by == interaction.user.id):
        await interaction.response.send_message("Only an admin or the person who added it can edit it.",
                                                ephemeral=True)
        return
    await asyncio.to_thread(store.set_caption, rec.frn, img.name, caption)
    idx = [i.name for i in store.list(rec.frn)].index(img.name)
    await send_view(interaction, open_images(rec, interaction.user.id, idx), ephemeral=True)


bot.tree.add_command(image_group)


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    if not TOKEN:
        raise SystemExit("DISCORD_TOKEN is not set (put it in bot/.env, see bot/.env.example)")
    if not FRT_BIN.exists():
        log.warning("importer binary %s not found; run `make` in %s", FRT_BIN, ROOT)
    bot.run(TOKEN, log_handler=None)


if __name__ == "__main__":
    main()
