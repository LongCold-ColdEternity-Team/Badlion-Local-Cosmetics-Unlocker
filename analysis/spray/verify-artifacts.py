"""Read-only local evidence packaging; never inject or launch the client."""
from datetime import datetime, timezone
import difflib
import hashlib
import json
from pathlib import Path
import re
import zipfile
import pefile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent

def sha(data):
    return hashlib.sha256(data).hexdigest()

paths = [ROOT / p for p in (
    "agent.cpp", "spray_support.inl", "README.md", "build.ps1",
    "bin/blc_unlock_agent.dll", "bin/BadlionUnlockInjector.exe",
    "dist/BadlionUnlockUI.exe", "target-classes.txt",
)]
paths += [HERE / "test-bin/blc_unlock_agent.dll"]
artifacts = {p.relative_to(ROOT).as_posix(): {"bytes": p.stat().st_size, "sha256": sha(p.read_bytes())} for p in paths}

# Inspect PE resources; do not execute the release binary.
pe = pefile.PE(str(ROOT / "dist/BadlionUnlockUI.exe"))
embedded = {}
for kind in pe.DIRECTORY_ENTRY_RESOURCE.entries:
    if kind.id != 10:  # RT_RCDATA
        continue
    for entry in kind.directory.entries:
        if entry.id not in (101, 102):
            continue
        data = entry.directory.entries[0].data.struct
        raw = pe.get_data(data.OffsetToData, data.Size)
        expected = ROOT / ("bin/blc_unlock_agent.dll" if entry.id == 101 else "target-classes.txt")
        embedded[str(entry.id)] = {"bytes": len(raw), "sha256": sha(raw), "matches": raw == expected.read_bytes()}
pe.close()
assert set(embedded) == {"101", "102"} and all(v["matches"] for v in embedded.values())
assert artifacts["bin/blc_unlock_agent.dll"]["sha256"] == artifacts["analysis/spray/test-bin/blc_unlock_agent.dll"]["sha256"]

profile = Path("E:/mc/Badlion/BLClient-Mod-Profiles/New Profile 1.zip")
with zipfile.ZipFile(profile) as z:
    data = json.loads(z.read("data.json"))
    spray = data["spraySettings"]
    # Only relevant spray settings, not full profile or personal metadata.
    profile_evidence = {
        "path": str(profile), "slots": spray["sprays"],
        "menuKey": spray["spraysKey"], "wheelKey": spray["wheelKey"],
        "disableSprays": data["betterframesConfig"]["disableSprays"],
    }

raw_log = (HERE / "test-bin/blc_unlock_agent.log").read_text(encoding="utf-8", errors="replace")
lines = [s for s in raw_log.splitlines() if re.match(
    r"^(SPRAY_|UNLOCK_RESPONSE_SNAPSHOT|UNLOCK_DIRECT|SELECTION_RESTORE |SELECTION_MONITOR|PUBLIC_LIST i\(SPRAY\))", s)]
(HERE / "runtime-evidence.txt").write_text(
    "Captured from test-bin/blc_unlock_agent.log; JVM PID 28548; not the synthetic fixture.\n" + "\n".join(lines) + "\n", encoding="utf-8")
diff = []
for name in ("agent.cpp", "README.md", "spray_support.inl"):
    baseline = HERE / "baseline" / name
    before = baseline.read_text(encoding="utf-8-sig").splitlines(keepends=True) if baseline.exists() else []
    after = (ROOT / name).read_text(encoding="utf-8-sig").splitlines(keepends=True)
    diff.extend(difflib.unified_diff(before, after, fromfile="baseline/" + name, tofile="current/" + name))
(HERE / "spray-source-from-baseline.patch").write_text("".join(diff), encoding="utf-8")
result = {
    "recordedAt": datetime.now(timezone.utc).isoformat(),
    "artifacts": artifacts, "embeddedResources": embedded,
    "gameProfileSpraySettings": profile_evidence,
    "screenshots": {p.name: sha(p.read_bytes()) for p in sorted((HERE / "ui").glob("*.png"))},
    "realClientVerified": ["253 owned entries", "sample ownership predicates", "catalog textures", "slot2 ID0 saved via UI", "wheel shows ID0"],
    "pending": ["placement/world rendering", "clean restart using only release payload", "other-player visibility not supported by evidence"],
}
(HERE / "verification.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(json.dumps({"embeddedResources": embedded, "profile": profile_evidence, "runtimeLines": len(lines)}, ensure_ascii=False, indent=2))
