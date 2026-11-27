"""Check the actual cooked object imports and the saved editor slots for mercenary attachments."""
from pathlib import Path
import re
import sys


WEAPONS = "/Game/OpenSCCT/Weapons/"
MATERIALS = WEAPONS + "Materials/"
ATTACHMENTS = MATERIALS + "Attachments/"


def slots(*names):
    return {ATTACHMENTS + "OpenScctRifles_" + name if name.startswith("Optic") or name == "Suppressor"
            else MATERIALS + name for name in names}


HOUSING = ("OpticRubber", "OpticAnodized", "OpticFasteners")
SCOPE = ("MI_ScctScopeAnodized", "MI_ScctScopeRubber", "MI_ScctScopeHardware", "M_ScctAcogGlass", "M_ScctPrecisionImage")
RAIL = ("MI_ScctScopeRubber", "MI_ScctScopeAnodized", "OpticFasteners")
REQUIRED = {WEAPONS + name: slots(*materials) for name, materials in {
    "SM_MercACOG": (*HOUSING, "M_ScctAcogGlass", "M_ScctAcogChevron", "M_ScctPrecisionImage"),
    "SM_MercBoltScope": SCOPE,
    "SM_MercDmrScope": SCOPE,
    "SM_MercM4CarryHandle": ("MI_ScctCarryCasting", "MI_ScctCarryHardware"),
    "SM_MercOpticCap": ("OpticRubber",),
    "SM_MercOpticHousing": ("OpticRubber", "OpticAnodized"),
    "SM_MercPistolRedDot": (*HOUSING, "OpticLens", "M_ScctAcogChevron"),
    "SM_MercRailModule": RAIL,
    "SM_MercRedDot": ("OpticRubber", "OpticAnodized", "M_ScctAcogChevron"),
    "SM_MercSuppressor": ("Suppressor",),
    "SM_MercTorch": RAIL,
}.items()}
REQUIRED[WEAPONS + "SM_MercRedDot"].add("/Game/OpenSCCT/Materials/M_ScctRifleDotLens")


def cooked(description):
    dependencies = {}
    current = None
    for line in description.splitlines():
        header = re.search(r"Package '([^']+)' Summary", line)
        if header:
            current = dependencies.setdefault(header[1], set())
        elif current is not None:
            match = re.search(r"\bImport \d+: '([^']*)'", line)
            if match:
                current.add(match[1].split(".", 1)[0])
    expected = REQUIRED.copy()
    for name in ("OpticAnodized", "OpticFasteners", "OpticRubber", "Suppressor", "OpticLens"):
        texture = "suppressor" if name == "Suppressor" else name
        deps = {ATTACHMENTS + "Textures/" + texture}
        if name != "OpticLens":
            deps.add("/Game/OpenSCCT/Materials/M_ScctRifleFinish")
        expected[ATTACHMENTS + "OpenScctRifles_" + name] = deps
    for asset, required in expected.items():
        actual = dependencies.get(asset, set())
        missing = required - actual
        if missing:
            raise ValueError(f"{asset}: missing cooked dependency: {', '.join(sorted(missing))}")
        if any(dep.startswith("/Game/Imported/") for dep in actual):
            raise ValueError(f"{asset} depends on an editor import")


def editor():
    import unreal
    for path, expected in REQUIRED.items():
        mesh = unreal.load_asset(path)
        if not mesh:
            raise ValueError(f"Missing mesh {path}")
        materials = [slot.material_interface for slot in mesh.get_editor_property("static_materials")]
        if not all(materials):
            raise ValueError(f"{path}: at least one material slot is null")
        actual = {m.get_path_name().split(".", 1)[0] for m in materials}
        if actual != expected:
            raise ValueError(f"{path}: saved slots {actual} differ from expected {expected}")
        unreal.log_warning(f"SCCT-IMPORT verified {path} with {len(materials)} material slots")


if __name__ == "__main__":
    if sys.argv[1] == "--editor":
        editor()
    else:
        data = Path(sys.argv[1]).read_bytes()
        encoding = "utf-16" if data[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8-sig"
        cooked(data.decode(encoding))
        print(f"Verified cooked mesh/material imports for {len(REQUIRED)} weapon attachments.")
