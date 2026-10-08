"""Adapt the retained linked-module packager for two bounded public consumers."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
helper = Path(__file__).resolve().parents[2] / "tests/BuildPlatformServiceFixture.py"
spec = importlib.util.spec_from_file_location("native_service_packager", helper)
module = importlib.util.module_from_spec(spec)
# Keep genuine linker metadata validation; parameterize only fixture names/scopes.
source = helper.read_text()
replacements = {
    'expected_exports = {"_start", "SceGuestMain"} if main else {"PlatformServiceGuestMath"}':
        'expected_exports = {"_start"} if main else {"NativeServiceDependency"}',
    'info & 15 not in (0, 2)': 'info & 15 != 2',
    'for filename in (GUEST_FILE, "libSceNpManager.prx", "libSceNet.prx", "libSceCommonDialog.prx"):':
        'for filename in (GUEST_FILE, "libc.prx", SERVICE + ".prx"):',
}
for old,new in replacements.items():
    if source.count(old)!=1: raise RuntimeError("Retained packager anchor changed: "+old)
    source=source.replace(old,new)
# Retained helper imports its sibling module by filename.
sys.path.insert(0,str(helper.parent))
exec(compile(source,str(helper),"exec"),module.__dict__)
if len(sys.argv)!=6:
    raise SystemExit("Usage: BuildNativeServiceFixture.py np-identity|http-uri dependency-linked main-linked dependency.prx main.elf")
kind,dep_linked,main_linked,dep,main=sys.argv[1:]
if kind not in ("np-identity","http-uri"): raise ValueError("Unknown bounded public fixture")
module.GUEST_NAME="NativeServiceDependency"
module.GUEST_FILE="NativeServiceDependency.prx"
module.MAIN_NAME="NativeNpIdentityGuest" if kind=="np-identity" else "NativeUriGuest"
module.SERVICE="libSceNpManager" if kind=="np-identity" else "libSceHttp"
module.SCOPES={1:module.SERVICE,2:"libc",3:module.GUEST_NAME}
module.IMPORTS={"NativeServiceDependency":(module.nid("NativeServiceDependency"),3),"uMei1W9uyNo":("uMei1W9uyNo",2)}
service="sceNpGetOnlineId" if kind=="np-identity" else "sceHttpUriEscape"
service_nid="XDncXQIJUSk" if kind=="np-identity" else "YuOW3dDAKYc"
module.IMPORTS[service]=(service_nid,1)
if kind=="np-identity": module.IMPORTS["eQH7nWPcAgc"]=("eQH7nWPcAgc",1)
module.package(dep_linked,dep,False)
module.package(main_linked,main,True)
# A genuine differently scoped original row, reparsed independently in owner tests.
wrong=Path(main).parent/"wrong-row"/Path(main).name
wrong.parent.mkdir(exist_ok=True)
module.IMPORTS[service]=("AAAAAAAAAAA",1)
module.package(main_linked,wrong,True)
for path in (Path(main),wrong,Path(dep)):
    receipt={"schema_version":1,"kind":kind,"path":path.name,"size":path.stat().st_size,
             "sha256":hashlib.sha256(path.read_bytes()).hexdigest(),"linked_source":"actual clang/lld ELF; genuine FUNC imports, RELATIVE and PLT relocations",
             "scope":"bounded public engineering fixture; no private title identity or firmware claim"}
    path.with_suffix(".receipt.json").write_text(json.dumps(receipt,indent=2)+"\n")
