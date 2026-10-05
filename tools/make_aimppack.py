#!/usr/bin/env python3
"""Builds PreventResampling-<version>.aimppack (a ZIP archive AIMP installs when it is opened):

    PreventResampling/
        PreventResampling.dll        Windows 32-bit
        PreventResampling.txt        package info (name, version, author, description)
        x64/
            PreventResampling.dll    Windows 64-bit
            PreventResampling.so     Linux x86_64 (optional)

Layout as documented in the SDK (sdk/AIMP Addon Package.rtf): 32-bit binaries and platform independent files
in the plugin folder, all 64-bit binaries - including the Linux library - in x64.

Usage: tools/make_aimppack.py --x86 <dll> --x64 <dll> [--linux <so>] [--out <file>]
Also writes <file>.sha256 (the update check uses GitHub's asset digest, this file is the fallback).
"""
import argparse
import hashlib
import os
import re
import sys
import zipfile

NAME = "PreventResampling"


def read_version():
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "..", "CMakeLists.txt"), encoding="utf-8") as f:
        m = re.search(r"project\(PreventResampling VERSION ([0-9.]+)", f.read())
    return m.group(1) if m else "0.0.0"


def main():
    version = read_version()
    ap = argparse.ArgumentParser()
    ap.add_argument("--x86", required=True, help="32-bit Windows DLL")
    ap.add_argument("--x64", required=True, help="64-bit Windows DLL")
    ap.add_argument("--linux", help="Linux x86_64 .so (optional)")
    ap.add_argument("--out", default=f"{NAME}-{version}.aimppack")
    ap.add_argument("--topic", default="https://github.com/RainBowFl4sh/AIMP-No-Resmapling",
                    help="link to the plugin's forum topic (description file)")
    args = ap.parse_args()

    for path in filter(None, [args.x86, args.x64, args.linux]):
        if not os.path.isfile(path):
            sys.exit("missing file: " + path)

    # Description file in the format of the AIMP plugin catalog (forum rules, aimp.ru topic 32363):
    # purpose, AIMP versions, name, version, author, contact, forum topic, description - English and Russian
    info = (
        "\ufeff"
        "Назначение: Расширения функционала\r\n"
        "Версия: AIMP4, AIMP5, AIMP6\r\n"
        "\r\n"
        "Name: Prevent Resampling\r\n"
        f"Version: {version}\r\n"
        "Author: Fl4sh\r\n"
        "AuthorContact: https://github.com/RainBowFl4sh/AIMP-No-Resmapling/issues\r\n"
        f"Topic: {args.topic}\r\n"
        "Description: Switches the output sample rate to the sample rate of every track - no resampling "
        "(WASAPI, Voicemeeter; ASIO / WASAPI exclusive / DirectSound via an optional AIMP restart). "
        "Windows 32/64-bit, Linux x86_64. Disabled after installation.\r\n"
        "Описание: Переключает частоту дискретизации вывода на частоту каждого трека - без ресемплинга "
        "(WASAPI, Voicemeeter; ASIO / WASAPI Exclusive / DirectSound через необязательный перезапуск AIMP). "
        "Windows 32/64-bit, Linux x86_64. После установки выключен.\r\n"
        "\r\n"
        "Installation: drag this archive onto the AIMP window (or open the .aimppack from GitHub with a double-click). "
        "Restart AIMP and enable the plugin in Preferences -> Plugins -> Prevent Resampling -> General.\r\n"
        "Установка: перетащите этот архив в окно AIMP (или откройте .aimppack с GitHub двойным щелчком). "
        "Перезапустите AIMP и включите плагин в Настройки -> Плагины -> Prevent Resampling -> General.\r\n"
    )
    entries = [(args.x86, f"{NAME}/{NAME}.dll"), (args.x64, f"{NAME}/x64/{NAME}.dll")]
    if args.linux:
        entries.append((args.linux, f"{NAME}/x64/{NAME}.so"))

    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(f"{NAME}/", b"")
        z.writestr(f"{NAME}/x64/", b"")
        for src, dst in entries:
            z.write(src, dst)
        z.writestr(f"{NAME}/{NAME}.txt", info.encode("utf-8"))
    digest = hashlib.sha256(open(args.out, "rb").read()).hexdigest()
    with open(args.out + ".sha256", "w") as f:
        f.write(f"{digest}  {os.path.basename(args.out)}\n")
    print("written", args.out, digest)
    with zipfile.ZipFile(args.out) as z:
        for i in z.infolist():
            print(f"  {i.file_size:>9}  {i.filename}")


if __name__ == "__main__":
    main()
