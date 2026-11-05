#!/usr/bin/env python3
"""中文：个人二改的最小 OTA feed：生成、防降级守卫与发布后线上自检。
English: Minimal self-hosted OTA feed for personal mods: build, downgrade guard and live verification.

上游 package_flash/stage_public_flash 把官方域名硬编码在校验断言里，且会连带发布整个官方
刷机站；二改发布只需要 update.json 与固件 bin 两个文件，独立成工具避免改动上游脚本。
Upstream package/stage tools hardcode the official origins in their assertions and ship the
whole official flasher site; a mod release only needs update.json plus the app image, so this
stays separate instead of patching those scripts.
"""
import hashlib
import json
import re
import sys
import urllib.request
from pathlib import Path

ORIGIN = "https://ismoss.github.io/mossread"
FEED = ORIGIN + "/update.json"
VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)-rc(\d+)(?:-mod(\d+))?$")


def version_key(version: str):
    # 设备端解析器接受 X.Y.Z-rcN（官方）与 X.Y.Z-rcN-modM（二改）。二改代际优先于 rc 数值，
    # 使任何 mod 版高于早期 rc2xx 试验号；mod 之间先比 rc 基数再比 mod 序号。
    # The device parser accepts X.Y.Z-rcN (official) and X.Y.Z-rcN-modM (fork). The fork
    # generation outranks the rc number so any mod build beats the legacy rc2xx experiments;
    # between mods compare the rc base first, then the mod serial.
    match = VERSION_RE.match(version)
    assert match, f"version {version!r} must look like X.Y.Z-rcN[-modM]"
    x, y, z, rc, mod = (int(part) for part in match.groups(default="0"))
    return (x, y, z, 1 if mod else 0, rc, mod)


# 自建源上的公共 DNS 列表（「特殊功能」页在线获取）；设备端保留同样的内置兜底。
# Public DNS list on the self-hosted origin (fetched by the special-features page); the firmware carries the same built-in fallback.
DNS_LIST = [
    {"name": "阿里公共 DNS", "main": "223.5.5.5", "backup": "223.6.6.6"},
    {"name": "腾讯公共 DNS", "main": "119.29.29.29", "backup": "119.28.28.28"},
    {"name": "114 公共 DNS", "main": "114.114.114.114", "backup": "114.114.115.115"},
    {"name": "谷歌公共 DNS", "main": "8.8.8.8", "backup": "8.8.4.4"},
]


def load_image(dist: Path, version: str) -> bytes:
    data = (dist / "Read_Pico.bin").read_bytes()
    assert data[0] == 0xE9, "not an ESP image"
    assert int.from_bytes(data[32:36], "little") == 0xABCD5432, "missing app description"
    embedded = data[48:80].split(b"\0")[0].decode("ascii")
    assert embedded == version, f"image embeds {embedded!r}, requested {version!r}"
    return data


def build(dist: Path, out: Path, version: str, notes: str) -> None:
    version_key(version)  # Format check only; both legacy rc2xx and -modN are accepted.
    data = load_image(dist, version)
    feed = {
        "schema": 1, "version": version, "project": "Read_Pico", "board": "RDP-G01-W",
        "layout": "pico-dual-4m-v1", "minimum_base_version": "0.3.3-rc72",
        "url": f"{ORIGIN}/Pico-update-{version}.bin",
        "size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "notes": notes,
    }
    out.mkdir(parents=True, exist_ok=True)
    (out / f"Pico-update-{version}.bin").write_bytes(data)
    (out / "Pico-update.bin").write_bytes(data)
    (out / "update.json").write_text(json.dumps(feed, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (out / "dns.json").write_text(json.dumps(DNS_LIST, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (out / ".nojekyll").touch()
    (out / "index.html").write_text(
        "<!doctype html><meta charset=\"utf-8\"><title>mossread mod feed</title>"
        f"<p>mossread mod OTA feed. Latest: {version}. <a href=\"update.json\">update.json</a></p>",
        encoding="utf-8")


def fetch_no_redirect(url: str, timeout: int):
    # OTA 客户端会跟随重定向，但发布地址仍固定为 GitHub Pages 直链；自检拒绝任何 302，
    # 保证清单 URL 不会依赖会变化的跳转目标。
    # The OTA client follows redirects, but release URLs stay on direct GitHub Pages links;
    # the check rejects any 302 so the manifest never depends on a changing redirect target.
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, *args, **kwargs):
            return None
    opener = urllib.request.build_opener(NoRedirect)
    return opener.open(url, timeout=timeout)


def guard(version: str) -> None:
    try:
        live = json.load(fetch_no_redirect(FEED, 20))
    except Exception:
        print("no live feed yet; first publication")
        return
    assert version_key(version) > version_key(live["version"]), \
        f"live {live['version']} is not older than {version}; refusing a downgrade"


def verify(version: str) -> None:
    with fetch_no_redirect(FEED, 20) as response:
        assert response.geturl() == FEED, f"feed redirected to {response.geturl()}"
        feed = json.load(response)
    assert feed["version"] == version, feed["version"]
    assert feed["url"] == f"{ORIGIN}/Pico-update-{version}.bin", feed["url"]
    assert feed["project"] == "Read_Pico" and feed["board"] == "RDP-G01-W" and feed["layout"] == "pico-dual-4m-v1"
    with fetch_no_redirect(feed["url"], 120) as response:
        assert response.geturl() == feed["url"], f"image redirected to {response.geturl()}"
        data = response.read()
    assert len(data) == feed["size"], (len(data), feed["size"])
    assert hashlib.sha256(data).hexdigest() == feed["sha256"], "sha256 mismatch"
    print(f"PASS: live feed {version}, no redirects, size and sha256 verified")


if __name__ == "__main__":
    command = sys.argv[1]
    if command == "build":
        build(Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4], sys.argv[5])
    elif command == "guard":
        guard(sys.argv[2])
    elif command == "verify":
        verify(sys.argv[2])
    else:
        raise SystemExit(f"unknown command {command!r}")
