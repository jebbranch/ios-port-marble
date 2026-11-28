#!/usr/bin/env python3
"""Stage the verified release; alter only backend registration and notifications."""
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile

root = Path(__file__).resolve().parents[1]
subprocess.run(["python3", str(root / "tools/fetch-sources.py"), "--check"], check=True)
lock = json.loads((root / "tools/source-lock.json").read_text())["supplicant"]
out = root / "out/wpa-src"
out.mkdir(exist_ok=True)
staged = {}
def write_changed(path, data):
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)
with tarfile.open(root / "deps" / lock["archive"]) as archive:
    for member in archive.getmembers():
        relative = Path(member.name).relative_to(lock["directory"])
        target = out / relative
        if member.isdir():
            target.mkdir(exist_ok=True, parents=True)
        elif member.isfile():
            target.parent.mkdir(exist_ok=True, parents=True)
            staged[str(relative)] = archive.extractfile(member).read()
        else:
            raise SystemExit(f"unsupported archive entry: {relative}")

changes = {}
def replace(name, old, new):
    before = staged[name].decode()
    if before.count(old) != 1:
        raise SystemExit(f"backend registration context changed: {name}")
    staged[name] = before.replace(old, new).encode()
    record = changes.setdefault(name, {"original_sha256": hashlib.sha256(before.encode()).hexdigest()})
    record["staged_sha256"] = hashlib.sha256(staged[name]).hexdigest()

replace("src/drivers/drivers.mak", "##### COMMON DRIVERS\n", """##### COMMON DRIVERS

ifdef CONFIG_DRIVER_SHENG
DRV_CFLAGS += -DCONFIG_DRIVER_SHENG
DRV_OBJS += ../src/drivers/driver_sheng.o
DRV_OBJS += ../src/drivers/nw_control_client.o
DRV_OBJS += ../src/drivers/nw_control_wire.o
DRV_OBJS += ../src/drivers/nw_supplicant_frontend.o
DRV_OBJS += ../src/drivers/nw_wireless_event.o
NEED_SME=y
endif
""")
replace("src/drivers/drivers.c", "const struct wpa_driver_ops *const wpa_drivers[] =",
        "extern const struct wpa_driver_ops wpa_driver_sheng_ops;\n\nconst struct wpa_driver_ops *const wpa_drivers[] =")
replace("src/drivers/drivers.c", "#ifdef CONFIG_DRIVER_NL80211\n",
        "#ifdef CONFIG_DRIVER_SHENG\n\t&wpa_driver_sheng_ops,\n#endif\n#ifdef CONFIG_DRIVER_NL80211\n")
replace("wpa_supplicant/wpa_supplicant.c", "\tssid->auth_failures++;",
        "#ifdef CONFIG_DRIVER_SHENG\n\tnw_supplicant_auth_failed(wpa_s, reason);\n#endif\n\n\tssid->auth_failures++;")
replace("wpa_supplicant/wpa_supplicant.c", '#include "ctrl_iface.h"',
        '#include "ctrl_iface.h"\n#ifdef CONFIG_DRIVER_SHENG\n#include "nw_supplicant_frontend.h"\n#endif')
for name, data in staged.items():
    write_changed(out / name, data)
for relative, source in {
    "src/drivers/driver_sheng.c": "userspace/driver_sheng.c",
    "src/drivers/nw_control_client.c": "userspace/nw_control_client.c",
    "src/drivers/nw_control_wire.c": "src/nw_control_wire.c",
    "src/drivers/nw_supplicant_frontend.c": "userspace/nw_supplicant_frontend.c",
    "src/drivers/nw_wireless_event.c": "userspace/nw_wireless_event.c",
    "src/l2_packet/l2_packet_sheng.c": "userspace/l2_packet_sheng.c",
    "wpa_supplicant/.config": "userspace/supplicant.config",
}.items():
    destination = out / relative
    if destination.is_symlink():
        if destination.resolve() == root / source: continue
        destination.unlink()
    elif destination.exists():
        raise SystemExit(f"preserve non-generated adapter: {destination}")
    destination.symlink_to(root / source)
(out / "native-adapter.json").write_text(json.dumps({"archive_sha256": lock["sha256"],
    "changes": changes, "authentication_algorithms_modified": False}, indent=2) + "\n")
print(f"staged original wpa_supplicant with native backend: {out}")
