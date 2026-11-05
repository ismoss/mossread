# mossread · 小纸 Pico (RDP-G01-W)

> **mossread** is a community reading firmware for 小纸 Pico (RDP-G01-W). It stands on the work of [kiikoread](https://github.com/wegooo-cell/read-pico-reader) (upstream **0.3.3-rc100**, by wegooo-cell), which in turn builds on [MindReset's demo firmware](https://github.com/MindReset/read_pico_firmware). This repository is an independent branch maintained by **ismoss**: naming, release cadence and the self-hosted update feed are ours; the foundation and credit belong to the upstream authors. It is not an official release of MindReset or kiikoread; upstream sites, release notes, stores, support channels and issue trackers do not cover it.

> **License note.** Upstream code through rc93 is Apache-2.0. Upstream original material first published from rc94 and explicitly marked carries a [personal-study license](licenses/kiikoread-Personal-Study-1.0.txt) (no commercial use). New files added by this fork are Apache-2.0, but **the combined firmware still contains upstream noncommercial material** — use it for personal study and personal devices only; do not sell it or ship it commercially. See [LICENSE](LICENSE) for the exact scope.

**Current build:** live [update.json](https://ismoss.github.io/mossread/update.json) is authoritative (2026-11-05: `0.3.3-rc100-mod120`) · **Update feed:** `ismoss.github.io/mossread` · **Device:** 小纸 Pico RDP-G01-W (ESP32-S3 / 16 MB flash / 684×1216)

[Install](#3-install-and-rollback) · [What changed](#4-what-this-mod-changes) · [Version line](#5-version-line) · [Build](#7-build-from-source) · [License & credit](#9-license-credit-and-boundaries) · [简体中文](README.zh-CN.md)

---

## 1. What this is

One sentence: **build mossread on the kiikoread-compatible base, and make the parts that make you frown behave.**

| Position | Meaning |
| :--- | :--- |
| Not a rewrite | Home, shelf, files, settings, reader, transfer and lock screens keep the kiikoread-compatible skeleton; book formats, configuration backups, partition table and NVS keys stay compatible |
| Not official | No endorsement from MindReset or the kiikoread upstream; the current system brand is mossread, the update feed is self-hosted, and problems belong here rather than upstream |
| Not a one-off hack | CI (host regressions + board-timing build), a self-hosted OTA feed, a per-release [changelog](docs/CHANGELOG.md) and a [roadmap](docs/ROADMAP.md) |

The mod focuses on four things:

1. **Reading actually rotates**: body, TOC, fifteen secondary panels and the lock screen all follow the handset orientation.
2. **Networking behaves like a phone**: multiple known WiFi networks, switching when you move, explicit switches and honest state for connect/disconnect.
3. **Every wait has feedback**: updates, transfers, wallpaper previews and shelf scans show what they are doing and can be left or cancelled.
4. **Lock and refresh settle cleanly**: passcode ghosts, list ghost lines and deep-sleep residue are fixed at the root cause, not papered over.

## 2. Relationship to upstream

| Item | Upstream kiikoread | This mod |
| :--- | :--- | :--- |
| Baseline | `0.3.3-rc100` | Shared origin, then independent evolution (mod line) |
| Version | `X.Y.Z-rcN` | `X.Y.Z-rcN-modM`, e.g. `0.3.3-rc100-mod120` |
| Updates | kiikoread.com / upstream GitHub Pages | Self-hosted `ismoss.github.io/mossread`; never follows upstream |
| Support | Upstream issues / site | This repository; upstream does not support this firmware |
| License | Apache-2.0 + personal-study from rc94 | Upstream terms preserved; new files here Apache-2.0 |

### Branches

| Branch | Role |
| :--- | :--- |
| `main` | Upstream source mirror + this firmware's landing README; **no mod code** |
| `mod/glass-dock-lockoff` | Firmware source, CI, OTA feed publishing; day-to-day work happens here |
| `pr/*` | Upstream-worthy fixes prepared as separate pull requests, keeping mod-only changes out of upstream PRs |

> Why a self-hosted feed: a higher official version would overwrite this firmware on the next online check, taking landscape lock, DNS and the remote monitor with it. The self-hosted feed prevents that at the root. The trade-off: mossread devices **stop receiving official updates**, and going back requires a manual flash (see below).

### Upstream rc100 → rc105: what they changed, what we take (checked 2026-11-05)

Upstream restructured the repository into a Pico + Metalio S31 two-board tree (`boards/`) and merged PR55–75. What matters for the Pico:

| Release | Upstream did | Our call |
| :--- | :--- | :--- |
| rc102 | Liquid-glass shelf, unconfigured default look, English TeX hyphenation + word spacing, TXT volume TOC/quick volume picker, image viewer actions | Glass shelf already ours (mod118); word spacing already in rc100; hyphenation and volumes only if needed |
| rc103 | Full-pixel GL16 turns, static WebP, full progressive JPEG, EPUB EOCD-trailer and EPUB3-spine compatibility, files management suite, first-run setup / UI language switch | WebP (mod84), progressive JPEG (mod85) and files management (mod88–93) are ours already; the two EPUB compatibility fixes are worth taking (small, no conflicts) |
| rc104 | Shelf management aligned with Files, reader status/toolbar regression fixes, EPUB footnote popups, GIF first frame, JPEG decode fallback, double-tap image zoom, ripple erase/black re-drive, one-press deep-sleep resume | Shelf management and deep-sleep resume already ours; footnotes/GIF/JPEG fallback worth taking; ripple cleanup needs a device A/B |
| rc105 | Built-in font coverage to 7668 chars (16-level supplements, per-glyph ink), Default-fullscreen setting, reading guide text/full-row, overlay reader chrome without repagination, exact-bounds partial refresh | Font coverage is the biggest win (logic +6 KB first, assets +469 KB later); Default fullscreen / guide are cheap; overlay reflow is risky |

**Pulled into `mod119` → `mod120`**: 1) exact-bounds partial refresh, 2) the two EPUB open failures, 3) inline GIF first frame, 4) JPEG fallback, 5) missing-glyph fallback (mod119 logic plus mod120's PIF4 16-level supplement assets), 6) batched shelf progress, 7) turn phase cap 40→64, 8) the reading guide's follow-text / full-row switch.

**Not done**: a Default-fullscreen switch — mossread already opens a book fullscreen, so the switch only makes sense alongside a standard-mode entry model; left for a separate change.

**Not taking for now**: the shelf-management re-architecture (we already have an equivalent implementation, and the change surface is large), the two header/status-bar adjustments (our current behaviour already matches, so there is nothing to port), per-turn metadata exclusion (differs from this project's fused-footer refresh strategy), and the backup-file rename (would break existing backup compatibility).

### Deliberately unchanged

- Partition table, dual-slot layout and OTA data partition: untouched, so TF updates and configuration backups stay compatible.
- Book formats and parsing rules: existing EPUB/TXT limits, font caches and reading-progress format stay as they are.
- UI brand: the firmware now presents itself as mossread; data, partition layout, protocols and upstream compatibility identifiers remain unchanged.
- Telemetry: no account, no analytics, no cloud shelf.

## 3. Install and rollback

### 3.1 Prerequisites

| Requirement | Value |
| :--- | :--- |
| Device | 小纸 Pico **RDP-G01-W** (16 MB flash, dual 4 MB slots) |
| Current system | kiikoread **rc72 or newer** on a compatible dual-slot base |
| Storage | FAT32 microSD with a writable root |
| Power | Keep it connected during the update; do not remove the card |

If the current system does not qualify, use the official web flasher to install a stock kiikoread rc72+ first, then update as below. This repository publishes **application images only**, not bootloader/partition-table bundles.

### 3.2 From kiikoread (recommended)

1. Open [update.json](https://ismoss.github.io/mossread/update.json) and read `version` and `url`; 2026-11-05: `0.3.3-rc100-mod120`.
2. Download the matching `Pico-update-*.bin`, **rename it to `Pico-update.bin`**, and put it in the TF-card root.
3. On the device: **Settings → Upgrade & restore → System update → TF-card update**, confirm, and wait for the write and reboot.
4. After reboot, **Settings → Upgrade & restore → System update** should show the self-hosted feed; later checks can run over WiFi.

Direct link to the current image:
<https://ismoss.github.io/mossread/Pico-update-0.3.3-rc100-mod120.bin>

> `Pico-update.bin` is the compatibility name expected by the upstream firmware; do not change it. Overwrite the same path for each update.

### 3.3 From the official demo or another system

This fork has no full web-flash bundle. First install stock kiikoread rc72+ with the [official web flasher](https://kiikoread.com/#flash-action) over USB (desktop Chrome/Edge), then follow 3.2.

### 3.4 Roll back to upstream

A mod device only checks the self-hosted feed and **will not** offer official versions. To go back:

1. Open the [official web flasher](https://kiikoread.com/#flash-action) in desktop Chrome/Edge.
2. Connect over USB and complete a full official install.
3. If the device cannot enter download mode, open **Settings → Upgrade & restore → BOOT flashing** first.

A full flash replaces the system; ticking “erase device” clears internal settings and reading records. Books, fonts and images on the TF card are not touched.

### 3.5 Data boundaries

| Operation | Internal settings / records | TF card | Books / fonts / images |
| :--- | :--- | :--- | :--- |
| TF-card app update | Kept | Untouched | Kept |
| Official full web flash | May be cleared (depends on “erase”) | Untouched | Kept |
| Factory reset | Cleared | Untouched | Kept |

Before updating, save a `Pico-settings.backup` via **Settings → Save & restore → Save to TF card**. **The backup contains the WiFi password in plaintext** — keep the card private.

## 4. What this mod changes

Grouped by page, then logic, then root causes. `modN` is the mod release number; the full per-release record is in the [changelog](docs/CHANGELOG.md), and the batch/decision history is in the [roadmap](docs/ROADMAP.md).

### 4.1 At a glance

| Area | What the mod does | Why it matters |
| :--- | :--- | :--- |
| Reading | In-book landscape, landscape TOC and fifteen panels, first-turn prefetch, font picker no longer reverts | Comfortable in landscape, faster turns, font choices stick |
| Lock screen | Ticket/wallpaper/collage/transparent cover all adapt to landscape; portrait + landscape wallpaper slots; GC16 settle; PIN grace | No portrait flash when locking from a landscape book; no passcode ghosts |
| Shelf | Chunked scan + persisted index; instant cold boot/deep wake; UI stays responsive while scanning | A large library no longer freezes the shelf for seconds |
| Network | Real auto-switch fix, auto-connect switch, real link display, router-following DNS + online list | No more retrying the previous hotspot after you move |
| Updates | Self-hosted feed, download rate, redirects, 90 s slow-link tolerance, non-blocking cancel, no auto-check on entry | Visible, cancellable, and never overwritten by upstream |
| Transfer | Leaving stops the session but reconnects WiFi; “N files transferred” counter; portrait/landscape wallpaper upload | No lingering web server, no lost network |
| Remote monitor | LAN screen mirror + touch injection; off every boot; one-time RAM token + optional 4-digit PIN | Handy for debugging/demos without leaving a permanent backdoor |
| Engineering | CI host regressions + board-timing build, post-deploy feed verification, changelog + roadmap | Every release is reproducible, rollback-able and traceable |

### 4.2 Reader

- **In-book landscape**: the toolbar's landscape key cycles portrait → landscape-right → landscape-left. Body, toolbar, TOC and all fifteen secondary panels re-anchor at the current reading position; entering or leaving a panel no longer rotates the view. Shelf and global menus stay portrait.
- **Landscape details**: page turns submit a whole-page diff (an area diff drove only half the body under rotation on hardware); the status bar follows the rotated width; ripple direction follows rotation; periodic cleanup and illustration zoom no longer use portrait-hardcoded rects.
- **TOC and panels**: the landscape TOC uses two columns and eight entries per page. Stats (with the chapter-jump keypad and 30-day chart), reading settings, turn mode, key settings, font settings, bookmarks and background preview are all landscape-native, while portrait layouts stay pixel-identical.
- **First-turn latency**: after the first reader frame outputs, the next-page prefetch finishes synchronously (bounded at 250 ms). A cold start's first turn hits the cache instead of degrading into a full synchronous repaint.
- **Font picker no longer hijacks the body**: painting the cards no longer changes the body face, the reader never repaginates while a panel is open, every pick is persisted, and load/save failures print a reason. This fixes the “change the font, it flashes and reverts to built-in Source Han Sans” root cause.
- **Font bench does not write settings**: the demo font page only swaps the active face to measure raster speed and hands the face back on exit.
- **TOC jump bar**: the press refreshes only the sheet locally, so a fast drag's release coordinate settles the percentage instead of every drag stopping at the press position.
- **EPUB details**: chapter file names containing colons open; pages without a TOC entry no longer get an invented “section N”; a lone full-page illustration is centred; zoom validates page/layout ownership.

### 4.3 Lock screen

- **Five lock styles**: reading ticket, wallpaper, shelf collage, transparent PNG cover, and “lock screen off” (blank only). Styles and PIN protection are independent.
- **Landscape lock suite**: locking from a landscape book draws the ticket at 1216×684 (cover left, title/progress/stats middle, a stub with today's figures and date right); the PIN pad lays out sideways (digits right, lock icon and dots left); the collage uses four rows of eight landscape covers; wallpaper and transparent covers follow the live canvas. Portrait compositions stay pixel-identical.
- **Two wallpaper slots**: the wallpaper lock has independent 684×1216 and 1216×684 slots. With no landscape image it follows the portrait one with a centre crop; deleting or renaming either file invalidates both paths.
- **Deep-sleep boot keeps orientation**: a landscape ticket locked before deep sleep comes back landscape after the key press, instead of turning into a portrait ticket. The PIN grace window still applies.
- **Residue actually settles**: returning to the lock face, unlocking and going to deep sleep all request a full GC16 settle. The root cause was `present()` hard-coding GL16 and ignoring the settle request; the pad's partial DU updates left the dots on the wallpaper.
- **PIN experience**: four digits, optional unlock grace window (off / 5 min / 1 h / 8 h / 24 h), single-press unlock by default with an optional double press. Only a salted digest is stored; ordinary configuration backups neither contain nor replace it.
- **Wallpaper list**: six per page, with a full row for “set landscape wallpaper / follow portrait” so the button is no longer half-clipped by the bottom bar.
- **Transparent cover in landscape**: frame, centre offset and shadow bounds follow the live canvas, and the cache key includes the canvas size, so the same PNG produces separate portrait and landscape composites.

### 4.4 Shelf, files and home

- **Chunked shelf scan**: entering the shelf paints “scanning…” immediately and stays responsive (switch pages, open a book) while the scan advances in per-loop time slices; it no longer freezes the UI for the whole scan.
- **Persisted index**: a successful scan writes the shelf (categories and trigger values included) to internal storage. Cold boots and deep-sleep wakes paint the full list in milliseconds, then reconcile in a shadow array and swap in place without flashing empty. USB card sessions bump the store revision so books changed on a PC are not missed.
- **Categories and page memory**: the shelf header offers all books / uncategorised / `books` subfolders, and each category remembers its cover-shelf and list-shelf page.
- **Files**: the listing extends under the dock with an untappable next-page preview row; the root preview row no longer overlaps the pager.
- **Home**: resume card, book clip, clock and calendar keep the upstream look; network/settings state now refreshes live instead of showing a stale snapshot (see 4.8).

### 4.5 Network: WiFi, DNS, updates

- **Automatic network switching, actually fixed**: the old path was dead code — the active credential was wiped before the picker ran, and the picker returned immediately without credentials, so it never worked from mod4 through mod19 and the device kept retrying the home hotspot at the office. The pick now runs on reconnection paths; if the active network is absent it switches to the strongest known one in range, retries once immediately after a failure, then backs off 45 s → 90 s → 180 s.
- **Known WiFi library**: up to eight networks, password-free rejoin, “saved · tap to connect” in scan results, forget removes it from the library. Automatic switching **only covers networks whose password is already saved**; a network never joined manually is never added by itself.
- **Auto-connect switch**: at the bottom-right of the nearby-networks page and in Settings → Connection & time → WiFi. New devices default to off; existing devices inherit their previous state (decided once on the boot where the key is still absent, from whether any saved credential exists). When off, neither unlock nor the background connects; only a manual connect does.
- **Real link display**: the settings home row and WiFi subpage show the SSID actually associated (“not connected” when offline), not the saved network name.
- **Manual disconnect stays disconnected**: after “disconnect”, the loop fallback and unlock auto-connect leave it alone; a manual connect or save restores it. One disconnect lasts until the next screen wake.
- **Wake releases the hold**: after a manual disconnect, the next wake still joins a saved network that is in range.
- **DNS settings** (Connection & time → DNS): defaults to **following the router**, no longer overriding public resolvers. Built-in list plus an online list from the self-hosted origin; the choice persists label plus both addresses, so reordering/removing entries or a firmware update cannot silently move it to another pair. Online refresh requires WiFi, downloads in a background task, and a failure keeps the previous list.
- **Self-hosted OTA**: online updates read the self-hosted feed first; there is no official fallback, so a higher official version cannot overwrite the mod. Range resume, retries, full SHA verification and inactive-slot write protection remain.
- **Upgrade page feedback**: no automatic check on entry (mod49); checking dots and elapsed seconds align to the row end; the download shows a live rate on the progress row; redirects are followed; the read-idle window grows from 30 s to 90 s so a brief stall is not a failed download; failures distinguish offline / unreachable server / HTTP status / invalid manifest / broken transfer; cancel does not block, TF install is not blocked by a download, and only a running download takes the TF-card row.

### 4.6 Transfer

- **Leaving stops the session, not the network**: exiting the transfer page always stops the HTTP server and hotspot; if a station was connected, it is brought back in `network_only` mode. No lingering web server and no lost network.
- **Counter and wording**: the counter reads “N files transferred” (firmware and wallpapers count); the entries read “WiFi transfer / hotspot transfer”; after an upload the row returns to “no file being transferred” instead of keeping the last name and a full bar.
- **Wallpaper upload**: the crop section gains portrait/landscape buttons that output 684×1216 or 1216×684 into the two independent slots; a failure says which slot.
- **Preview no longer stalls**: the wallpaper preview decodes once and keeps the pixels in PSRAM (it used to re-decode on every frame, which is the long white screen on a 3 MB image); a wide image turns 90° clockwise to fill the tall preview area with corrected direction and offset; the cache key carries the source mtime; leaving settings releases it.

### 4.7 Remote monitor

Settings → Connection & time → Remote monitor mirrors the screen to a browser on the LAN and injects taps/drags.

| Item | Behavior |
| :--- | :--- |
| Default | Off; **forced off on every boot**, never restored from NVS |
| Authentication | A 128-bit one-time token kept only in RAM, generated on each manual enable; optional four-digit entry PIN (persistent switch) |
| Address | `http://<ip>:8123/#k=<pin>`; the token lives in the URL `#fragment` (never sent to the server) and the page forwards it in a request header |
| Expiry | Disable, reboot or ten idle minutes invalidates it; never written to NVS or logs |
| Touch | The injection session expires 1.5 s after the last touch request, so closing the page mid-press cannot keep the device “locked” |
| Image | Half-resolution BMP (about a quarter of the payload), 1 Hz; clients serialise BMP generation; the page scales to the window and has an 8 s watchdog plus foreground reconnect |

> This is a LAN tool. Do not expose it to the internet and do not port-forward it.

### 4.8 Settings and display

- **Settings groups**: Connection & time (WiFi / BLE page turner / Date & time / Remote monitor / DNS), Display (system font / contrast / home refresh mode / home force refresh / auto sleep lock / double-press unlock / shutdown sleep), Personalization (shelf style / lock style / lock PIN / status signature), Upgrade & restore.
- **DNS moved**: from Personalization to Connection & time, next to WiFi, clock sync and the monitor; the two groups go 4+5 → 5+4, so the page height and the Upgrade & restore position are unchanged.
- **Pure black/white switches**: a gray track transits dark phases under the scroll's differential scan and flashes; pure BW holds clean in every refresh mode.
- **Full settle per page switch**: every switch into or out of a secondary page asks for one GC16, so the previous list leaves no ghost line at the bottom.
- **Settings notice**: moved beside the title on the same row and centred on the title's measured ink, so an imported face of any height cannot overlap the title.
- **Live network state**: the settings home no longer shows a stale snapshot; the row and status bar follow background connect/disconnect at once.
- **Floating glass dock**: the four main pages use a glass capsule dock; ordinary mode samples page content beneath it, fast/water keep a fixed light pill, and partial updates never recompute the glass.
- **Lock screen off**: locking only blanks the panel; a single power press returns to the previous page, and an enabled PIN still validates first.

### 4.9 Root causes fixed under the hood

These are not new features; they are paths that looked fine but never actually worked.

| Symptom | Root cause | Fix | Version |
| :--- | :--- | :--- | :--- |
| Office device keeps retrying the home hotspot | The active credential was wiped before the picker ran; the picker returned immediately (dead code) | Pick on reconnection paths; switch only when the active network is absent | mod20 |
| Monitor page stuck on “waiting” with a broken image | BMP `biPlanes`/`biBitCount` are 16-bit fields but only the low bytes were written; high bytes were heap garbage | Zero the whole 54-byte header, then write fields; host test prefills 0xFF and compares the whole header | mod25–26 |
| Checking dots/seconds never appear | Animation was driven by the network task's wait callback, which was deregistered when the task spawned | Drive both phases from the UI loop's per-second tick | early mod line |
| Font change flashes back to Source Han Sans | Restoring the body face after drawing the picker could fail silently; the next tick repaginated the whole book in the built-in face | Drawing never changes the body face; no repagination while a panel is open; every pick persisted | mod31–32 |
| Passcode dots stay on the lock wallpaper | `present()` hard-coded GL16 and ignored the settle request; deep sleep also skipped a clean settle | Return to the lock face with GC16; whiten and settle before deep sleep | mod51–52 |
| PIN grace window freezes taps | The step cycle used a bit flag as an index (`& 4`) | Bounded lookup table with a five-step cycle | early mod line |
| Landscape turns drive only half the body | The area diff covers half the body under rotation on hardware | Submit a whole-page diff for landscape turns | early mod line |
| Wallpaper preview shows white for a long time | Every frame re-decoded the large image | Decode once into PSRAM, key the cache on mtime | mod37–38 |
| Web server lingers after a transfer / network is lost | Leaving the page only stopped the hotspot; STA handling was inconsistent | Always stop the session; bring STA back in `network_only` | mod48 |
| Cancel takes ages during a download | Cancel waited for the worker; TF install was blocked by the download | Cancel without blocking; only a running download takes the TF row | mod48 |

> The changelog marks each item's evidence: hardware, serial log, code-level reasoning or CI. **A claim is not evidence** — see section 6.

### 4.10 Known limits

- **RDP-G01-W only**; Metalio EINK 4 Plus is a separate upstream line and is not covered.
- No full web-flash bundle for the mod; coming from the official demo or another system requires stock kiikoread rc72+ first.
- The remote monitor is a LAN tool, off by default, with no public-internet authentication/encryption; do not port-forward it.
- FAT16/FAT32 only; exFAT is not enabled, and a large card is not automatically compatible.
- Inherited EPUB limits remain: 4 MB per XHTML resource, 8 MB per decompressed image, 8192 ZIP entries/chapters.
- The full on-device walk-through for mod1–mod118 is still in progress; green CI is not a hardware acceptance for every item.

## 5. Version line

| Era | Versions | Theme |
| :--- | :--- | :--- |
| Start | mod1–mod19 | Fork release line, version scheme, self-hosted OTA, dual-repo engineering rules; in-book landscape, chunked shelf scan + index, font system; DNS, known WiFi, first remote monitor, dual-mode transfer |
| Network & monitor | mod20–mod26 | Real auto-switch fix, auto-connect switch, upgrade indicators, monitor image/scale/connection fixes |
| Landscape lock, update chain & rebrand | mod27–mod64 | Monitor move + one-time RAM token, DNS router default, auto-connect inheritance, zero warnings; font root cause; landscape ticket/keypad/collage/wallpaper slots/transparent cover; wallpaper preview; transfer semantics; update rate/redirect/slow link; monitor PIN and touch expiry; GC16 lock settle; transparent-cover boot-cache guard; audit hardening (strict landscape enum, cover bounds, 90s manifest, disconnect hold); key evidence separates real power/deep-sleep wakes from OTA/reset boots; boot face is no longer duplicated before a direct passcode, and transparent-cover startup avoids the white badge flash |
| Direction, files & highlights | mod65–mod118 | Four turn modes, wallpaper direction/cover rotation; adaptive reading direction (four-way incl. 180°, absolute attitude, persisted lock, touch/gesture rotation); configurable reader gestures; power-key tap to turn; WebP + progressive JPEG; memory policy; deep-sleep single-press with the small lock face; file-management suite (sort / recursive search / book stats / list-grid / cross-page multi-select / batch delete / batch move); highlights (panel, underline, line capture, char-precise spans, tags, notes, Markdown export, press-drag select); Markdown reading and light rendering; minimal clock lock; liquid-glass shelf |

- Per-release user-visible changes: [docs/CHANGELOG.md](docs/CHANGELOG.md)
- Batches, decisions and rejected work: [docs/ROADMAP.md](docs/ROADMAP.md)
- Live version: [update.json](https://ismoss.github.io/mossread/update.json)

## 6. Verification status

| Item | How | Status |
| :--- | :--- | :--- |
| Build | GitHub Actions, ESP-IDF v6.1, board `sdkconfig.defaults` timing | Passing; zero warnings since mod30 |
| Host regressions | Display/reader/lock/shelf/EPUB/settings/OTA TLS/DNS/monitor BMP suites | Passing on every push |
| Published feed | Post-deploy check of update.json, image size, SHA-256 and live fetch | Passing on every dispatch |
| On-device walk-through | Manual acceptance of mod1–mod118 | **In progress**; individual fixes marked hardware-confirmed in the changelog |

We try to keep “verified” and “merely compiled” separate. If an item has no hardware note, treat it as code/CI-level evidence, not a device promise.

## 7. Build from source

Requires **ESP-IDF v6.1**, target ESP32-S3:

```sh
python3 tools/prepare_tls_bundle.py "$IDF_PATH"
idf.py set-target esp32s3
PICO_PROJECT_VER=0.3.3-rc100-mod120 idf.py build
```

- `sdkconfig.defaults` carries this board's flash/PSRAM timing; `sdkconfig.ci` is compile-only. Do not ship the CI timing as the product default.
- `prepare_tls_bundle.py` only fixes ESP-IDF 6.1's handling of the certificate bundle's synthetic root; it does not disable validity, hostname or signature checks.
- The app image is `build/Read_Pico.bin`; the TF package must be named `Pico-update.bin`.

CI and publishing:

| Workflow | Trigger | What it does |
| :--- | :--- | :--- |
| `mod-build` | Push to the mod branch | Host regressions + board-timing build |
| `mod-build` | Manual dispatch (must pass `--ref mod/glass-dock-lockoff`) | After tests/build, publish the minimal OTA feed to GitHub Pages and verify it live |
| `pages.yml` | Website files change on `main` | Publish the upstream web flasher (not the mod feed) |

> `mod-build.yml` exists only on the mod branch; `main` is an upstream mirror and intentionally lacks it. A dispatch without `--ref` fails with “Workflow does not have workflow_dispatch trigger”.

## 8. Privacy and security

- No telemetry, account or cloud shelf; books, reading records and settings are never uploaded.
- Network access is limited to update checks, time sync, optional DNS, user-initiated transfers and the manually enabled LAN monitor.
- Remote monitor: off by default, forced off every boot, one-time RAM token (optionally a four-digit PIN), ten idle minutes auto-off, touch injection expires after 1.5 s; the token is never written to NVS or logs.
- The lock PIN stores only a salted digest; ordinary configuration backups neither contain nor replace it.
- Configuration backups contain the **WiFi password in plaintext**; redact them before sharing logs or backups.
- The mod does not disable TLS/SHA verification and does not add arbitrary mirrors; OTA trusts only the self-hosted feed.

## 9. License, credit and boundaries

### License scope

| Material | License |
| :--- | :--- |
| Upstream kiikoread through rc93 | Apache-2.0 |
| Upstream original material from rc94, explicitly marked | [Personal Study License](licenses/kiikoread-Personal-Study-1.0.txt): personal study/research/testing; commercial use requires written permission |
| New files added by this fork | Apache-2.0 (SPDX headers) |
| Lucide icons | ISC |
| epdiy driver (trimmed fork) | LGPL-3.0-or-later |
| pypinyin dictionary data | MIT |
| Embedded Noto Sans SC subset | SIL OFL 1.1 |

The combined firmware contains both Apache-2.0 and upstream personal-study material. **It is not “open source” as a whole**, and it must not be sold or shipped commercially. When redistributing, keep [LICENSE](LICENSE), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and the file headers.

### Credit

- [MindReset](https://dot.mindreset.tech/docs/read_0): hardware, demo firmware and documentation.
- [Kiiko厂长 / wegooo-cell](https://github.com/wegooo-cell/read-pico-reader) and the kiikoread contributors: the entire technical foundation of mossread.
- Upstream third-party components and font authors: Lucide, epdiy, pypinyin, Noto.
- The other 小纸 Pico community system authors: their public work and discussions made many problems easier to locate.

### Support boundaries

- Mod bugs, installation and feature requests: this repository's issues.
- Upstream kiikoread problems: the upstream repository/site; do not file mod logs as upstream bugs.
- Device purchase, repair and hardware: MindReset's official channels.

## 10. Reporting and etiquette

- **When reporting a bug**, include the version (shown under `Settings → Upgrade & restore → System update`, or update.json), device model, reproduction steps, whether it was a TF/OTA install, and a serial log or photo. Do not paste WiFi passwords, device tokens or full configuration backups.
- **Do not** pass a mossread build off as an official build, and do not use “official” or “by MindReset” wording.
- **Do not** strip copyright headers, license files or credits before redistributing; never sell it.
- Fixes that are also useful upstream are prepared as separate `pr/*` branches rather than smuggling mod-only changes into an upstream PR; whether upstream accepts them is upstream's call.
- Other community systems (if-ink OS, Take Easy, PageRest, Pico Page, the CrossMux family, etc.) make different trade-offs. mossread is one opinionated choice; when borrowing ideas, respect their licenses and credit them.

## Appendix A: Inherited upstream capabilities

The mod keeps the full kiikoread foundation. The most-asked parts:

- Home / shelf / files / settings; shelf styles, lock styles, status signature.
- EPUB / TXT reading: TOC, bookmarks, progress, reading time, monthly goal, typography (size, line spacing, margins, first-line indent, letter spacing, reading line).
- WiFi / hotspot / USB transfer; Pinyin/T9/full keyboard; BLE page turner.
- Ordinary / fast / water home refresh; auto sleep, shutdown sleep, lock PIN, configuration backup, panel repair, BOOT flashing.
- Full upstream docs: [upstream README](https://github.com/wegooo-cell/read-pico-reader/blob/main/README.zh-CN.md) · [online manual](https://kiikoread.com/manual.html)

## Appendix B: Useful links

| Purpose | URL |
| :--- | :--- |
| Firmware update feed | <https://ismoss.github.io/mossread/update.json> |
| Firmware DNS online list | <https://ismoss.github.io/mossread/dns.json> |
| Firmware source (this branch) | <https://github.com/ismoss/mossread/tree/mod/glass-dock-lockoff> |
| Upstream kiikoread | <https://github.com/wegooo-cell/read-pico-reader> |
| Official web flasher (rollback) | <https://kiikoread.com/#flash-action> |
| Official device docs | <https://dot.mindreset.tech/docs/read_0> |
| Firmware changelog | [docs/CHANGELOG.md](docs/CHANGELOG.md) |
| Firmware roadmap | [docs/ROADMAP.md](docs/ROADMAP.md) |
| License scope | [LICENSE](LICENSE) |
| Third-party notices | [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) |
| Privacy | [PRIVACY.md](PRIVACY.md) |
