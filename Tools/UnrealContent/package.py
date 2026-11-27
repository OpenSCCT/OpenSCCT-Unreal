"""Assemble the Unreal edition's offline Steam conversion tools from its vendored source."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


PROJECT = Path(__file__).resolve().parents[2]
RUNTIME = PROJECT / "Runtime"
ASSET_FOLDERS = ("cosmetics", "sidearms", "weapons", "rifles", "sidearm_audio", "user_gun_audio")
SOURCE_PATHS = (
    "content/svmbuild", "tools/scctpkg/scctpkg", "tools/scctpkg/UPSTREAM.md",
    "re/formats/scctformats", "spec/svmap", "shared/SteamVersusMaps.txt",
    "content/requirements.txt", "content/assets/cosmetics",
    *(f"content/assets/{category}/export" for category in ASSET_FOLDERS if category != "cosmetics"),
    *(f"content/assets/{category}/CREDITS.txt" for category in ASSET_FOLDERS),
)
MEDIA = {".jpg", ".png", ".glb", ".wav"}
TEXT = {".txt", ".md", ".py", ".json"}
RECEIPT = "unreal-steam-builder.json"
SETUP = "OpenScct.UnrealSetup.dll"
SETUP_FILES = {SETUP, "OpenScct.UnrealSetup.deps.json", "OpenScct.UnrealSetup.runtimeconfig.json",
               "OpenScct.Launcher.Core.dll", "OpenScct.Maps.dll"}
PYTHON_BUILD_SUFFIXES = {".lib", ".pdb", ".pyc", ".whl"}
VENDING_PLATES = {
    "Cabinet-Art.png": "b54a452ac3921eace23bd1bd9113d0426cc1add58a47105e77a8543c5d08e08b",
    "Shelf-Art.png": "e4d29305a036be84b78dcfc63e16b15cb33633e5b5f41f114e99eff0015cfbfe",
    "Panel-Art.png": "8bc3d4b185ccaaf7d72d7e1445007b2ea4b6c25927479488469f1274e01a0458",
}


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def tracked_sources():
    paths = subprocess.check_output(["git", "ls-files", "-z", "--", *SOURCE_PATHS], cwd=RUNTIME)
    names = sorted(set(paths.decode("utf-8").split("\0")) - {""})
    required = {"content/svmbuild/__main__.py", "shared/SteamVersusMaps.txt", "tools/scctpkg/UPSTREAM.md"}
    required.update(f"content/assets/{category}/CREDITS.txt" for category in ASSET_FOLDERS)
    required.update(f"content/assets/rifles/export/{stem}{suffix}.glb"
                    for stem in ("m4a1s", "ak47", "galil", "famas", "compactsmg", "pumpshotgun", "boltrifle", "designatedmarksman")
                    for suffix in ("", "_first"))
    if missing := required.difference(names):
        raise ValueError(f"Missing tracked converter inputs: {sorted(missing)}")
    return names


def check_sources(path, names):
    """Only ship source text and reviewed media. Retail surfaces and animations are generated on the player's PC."""
    provenance = json.loads((RUNTIME / "tools/ci/asset-provenance.json").read_text(encoding="utf-8"))["assets"]
    digest = hashlib.sha256()
    for name in names:
        source = RUNTIME / name
        file = path / name
        if file.is_symlink() or not file.is_file() or name.endswith("/class_defaults.json"):
            raise ValueError(f"Unexpected or missing source: {name}")
        actual = sha256(file)
        if file.suffix.lower() in MEDIA:
            entry = provenance.get(name)
            if entry:
                if actual != entry["sha256"] or file.stat().st_size != entry["bytes"]:
                    raise ValueError(f"Asset differs from the reviewed media: {name}")
            elif name.startswith("content/assets/rifles/export/textures/"):
                # The standalone project contains the reviewed original material maps for its four new long guns.
                original = PROJECT / "Art/Weapons/LongGuns/Export/textures" / file.name
                if not original.is_file() or sha256(original) != actual:
                    raise ValueError(f"Missing matching Unreal source texture: {name}")
            else:
                raise ValueError(f"Media has no distribution provenance: {name}")
        elif file.suffix.lower() not in TEXT or (file.suffix == ".py" and "/assets/" in name):
            raise ValueError(f"Unexpected source file: {name}")
        if path != RUNTIME and actual != sha256(source):
            raise ValueError(f"Packaged source differs from this release: {name}")
        digest.update(name.encode("utf-8") + b"\0" + bytes.fromhex(actual))
    return digest.hexdigest()


def has_debug_symbols(image):
    """Read the PE debug directory, where even a separate PDB can leave an absolute build path."""
    pe, = struct.unpack_from("<I", image, 0x3c)
    if image[:2] != b"MZ" or image[pe:pe + 4] != b"PE\0\0":
        raise ValueError("Invalid setup DLL")
    optional = pe + 24
    magic, = struct.unpack_from("<H", image, optional)
    if magic not in (0x10b, 0x20b):
        raise ValueError("Invalid setup DLL optional header")
    directories = optional + (96 if magic == 0x10b else 112)
    directory_count, = struct.unpack_from("<I", image, directories - 4)
    if directory_count <= 6:
        return False
    debug_rva, debug_size = struct.unpack_from("<II", image, directories + 6 * 8)
    if not debug_size:
        return False
    sections, = struct.unpack_from("<H", image, pe + 6)
    header_size, = struct.unpack_from("<H", image, pe + 20)
    for section in range(sections):
        offset = optional + header_size + 40 * section
        _, address, raw_size, raw_offset = struct.unpack_from("<IIII", image, offset + 8)
        if address <= debug_rva and debug_rva + debug_size <= address + raw_size:
            start = raw_offset + debug_rva - address
            records = struct.iter_unpack("<IIHHIIII", image[start:start + debug_size])
            return any(record[4] in (2, 17) for record in records)  # CodeView or embedded Portable PDB
    raise ValueError("Invalid setup DLL debug directory")


def python_build_files(folder):
    # The interpreter's standard library stays zipped. Loose bytecode and link libraries
    # supplied by a wheel can contain build-host paths and are unused for conversion.
    return (path for path in folder.rglob("*") if path.suffix.lower() in PYTHON_BUILD_SUFFIXES and path.is_file())


def check_setup(folder):
    entries = list(folder.iterdir())
    files = {entry.name for entry in entries}
    if files != SETUP_FILES:
        raise ValueError(f"Unexpected setup payload: extra={sorted(files - SETUP_FILES)}, "
                         f"missing={sorted(SETUP_FILES - files)}")
    local_paths = {str(PROJECT), str(Path.home())}
    for entry in entries:
        if entry.is_symlink() or not entry.is_file():
            raise ValueError(f"Expected a regular setup file: {entry.name}")
        payload = entry.read_bytes()
        if entry.suffix == ".dll" and has_debug_symbols(payload):
            raise ValueError(f"Setup DLL contains debug symbols or a PDB reference: {entry.name}")
        payload = payload.lower()
        for local in local_paths:
            if len(local) <= 3:
                continue
            for path in (local.replace("\\", "/"), local.replace("/", "\\")):
                if any(path.lower().encode(encoding) in payload for encoding in ("utf-8", "utf-16le")):
                    raise ValueError(f"Setup payload contains a local build path: {entry.name}")


def cabinet_sources():
    art = PROJECT / "Art/Maps/MounE/Vending"
    return [*(art / name for name in VENDING_PLATES), art / "CREDITS.txt",
            PROJECT / "Tools/Import/build_vending_override.py"]


def check_community(folder):
    sources = cabinet_sources()
    expected = {f"MounE/{source.name}" for source in sources}
    actual = {path.relative_to(folder).as_posix() for path in folder.rglob("*") if path.is_file()}
    if actual != expected:
        raise ValueError(f"Unexpected community tools: extra={sorted(actual - expected)}, missing={sorted(expected - actual)}")
    for source in sources:
        file = folder / "MounE" / source.name
        approved = VENDING_PLATES.get(source.name) or sha256(source)
        if file.is_symlink() or sha256(file) != approved:
            raise ValueError(f"Community tool differs from its reviewed source: {source.name}")


def stage_community(folder):
    destination = folder / "MounE"
    destination.mkdir(parents=True, exist_ok=True)
    for source in cabinet_sources():
        shutil.copyfile(source, destination / source.name)
    check_community(folder)


def receipt_for(folder, names):
    expected = {"python", "repo", "setup", "community", "steam-builder-v1", RECEIPT}
    extras = {p.name for p in folder.iterdir()} - expected
    if extras:
        raise ValueError(f"Unexpected files at the root of the Unreal converter: {sorted(extras)}")
    paths = {file.relative_to(folder / "repo").as_posix() for file in (folder / "repo").rglob("*") if file.is_file()}
    if paths != set(names):
        raise ValueError(f"Unexpected converter payload: extra={sorted(paths - set(names))}, missing={sorted(set(names) - paths)}")
    check_setup(folder / "setup")
    check_community(folder / "community")
    if unexpected := next(python_build_files(folder / "python"), None):
        raise ValueError(f"Unexpected Python build artifact: {unexpected.relative_to(folder)}")
    for name in ("python/python.exe", "python/LICENSE.txt", "steam-builder-v1"):
        if not (folder / name).is_file():
            raise ValueError(f"Incomplete Unreal content tools: {name}")
    if (folder / "steam-builder-v1").read_text(encoding="ascii").strip() != "win-x64":
        raise ValueError("The converter is not the Windows x64 build.")
    return source_receipt(folder / "repo", names)


def source_receipt(path, names):
    return {"schema": 1, "platform": "win-x64", "recipeDigest": check_sources(path, names), "sourceFiles": len(names)}


def write_receipt(folder, receipt):
    folder.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=folder) as temporary:
        pending = Path(temporary) / RECEIPT
        pending.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        pending.replace(folder / RECEIPT)


def package(output):
    if output.name.lower() != "contenttools":
        raise ValueError("Use the contenttools folder inside the installed Unreal release as output.")
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists() and any(output.iterdir()) and not (output / RECEIPT).is_file():
        raise ValueError(f"Refusing to replace a folder other than this edition's tools: {output}")
    names = tracked_sources()
    check_sources(RUNTIME, names)
    spec = importlib.util.spec_from_file_location("runtime_packager", RUNTIME / "tools/release/pack_content_tools.py")
    packager = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(packager)
    packager.SOURCES = SOURCE_PATHS
    packager.CACHE = PROJECT / "Saved/UnrealContentTools/cache"
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        stage = Path(temporary) / "contenttools"
        packager.package(stage, "win-x64")
        stage_community(stage / "community")
        for path in python_build_files(stage / "python"):
            path.unlink()
        subprocess.run(["dotnet", "publish", str(PROJECT / "Tools/SteamSetup/OpenScct.UnrealSetup.csproj"),
                        "-c", "Release", "-r", "win-x64", "--self-contained", "false", "-p:UseAppHost=false",
                        "--artifacts-path", str(Path(temporary) / "dotnet"), "-p:DebugType=none", "-p:DebugSymbols=false",
                        "-o", str(stage / "setup")], check=True)
        receipt = receipt_for(stage, names)
        write_receipt(stage, receipt)
        previous = Path(temporary) / "previous"
        if output.exists():
            output.rename(previous)
        try:
            stage.rename(output)
        except OSError:
            if previous.exists():
                previous.rename(output)
            raise
    return receipt


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="Unreal release's contenttools directory")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--verify", action="store_true", help="verify a staged release against its vendored sources")
    mode.add_argument("--receipt-only", action="store_true", help="write the expected recipe into a server release without installing tools")
    args = parser.parse_args()
    folder = args.output.resolve()
    if args.receipt_only:
        receipt = source_receipt(RUNTIME, tracked_sources())
        write_receipt(folder, receipt)
    elif args.verify:
        receipt = receipt_for(folder, tracked_sources())
        if receipt != json.loads((folder / RECEIPT).read_text(encoding="utf-8")):
            raise ValueError("The Unreal Steam recipe does not match its receipt.")
    else:
        receipt = package(folder)
    print(f"Verified {receipt['sourceFiles']} converter sources, recipe {receipt['recipeDigest']}")
