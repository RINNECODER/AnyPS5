"""Actual package-shaped Main service profile oracle, with guest-owned goldens."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def run(arguments, root):
    result=subprocess.run(arguments,cwd=root,capture_output=True,text=True,timeout=35)
    rows=[json.loads(line) for line in result.stderr.splitlines() if line.startswith('{')]
    assert result.stdout=='' and rows and all(row.get('schema_version')==1 for row in rows), (result.returncode,result.stdout,result.stderr)
    return result,rows


def main():
    runner,np_main,np_dep,uri_main,uri_dep,utility=map(lambda value:Path(value).resolve(),sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='native service package é ') as temporary:
        root=Path(temporary)
        (root/'bin').mkdir();(root/'fixtures').mkdir()
        binary=root/'bin'/runner.name
        shutil.copy2(runner,binary)
        shutil.copyfile(utility,root/'fixtures'/'AnyPS5Utilities.metallib')
        for kind,source,dep_source in [('np-identity',np_main,np_dep),('http-uri',uri_main,uri_dep)]:
            directory=root/kind;directory.mkdir()
            guest=directory/source.name;dep=directory/dep_source.name
            shutil.copyfile(source,guest);shutil.copyfile(dep_source,dep)
            digest=hashlib.sha256(guest.read_bytes()).hexdigest();size=guest.stat().st_size
            base=[str(binary),'--diagnostics-json','--sce-module',str(dep)]
            profile=['--native-service-public-profile',kind,digest,str(size)]
            result,rows=run(base+profile+[str(guest)],root)
            assert result.returncode==0 and [r['event'] for r in rows]==['startup','guest_exit'] and rows[-1]['exit_code']==0,(kind,result.returncode,result.stderr)
            assert rows[0]['executable']==str(guest) and rows[0]['host_architecture']=='arm64',rows
            for label,negative in [('omitted-profile',[]),('wrong-hash',['--native-service-public-profile',kind,('0' if digest[0]!='0' else '1')+digest[1:],str(size)]),('wrong-size',['--native-service-public-profile',kind,digest,str(size+1)])]:
                rejected,receipt=run(base+negative+[str(guest)],root)
                assert rejected.returncode!=0 and receipt[-1]['event']=='error', (kind,label,rejected.returncode,rejected.stderr)
            rejected,receipt=run([str(binary),'--diagnostics-json']+profile+[str(guest)],root)
            assert rejected.returncode!=0 and receipt[-1]['event']=='error',('profile-without-graph',kind,rejected.returncode,rejected.stderr)
    print('Actual Main explicit pinned NP/URI profiles execute genuine guest golden exits0; omitted/wrong certificates and absent graph refuse PASS; no title qualification')


if __name__=='__main__':
    assert len(sys.argv)==7,'Usage: native-service-cli runner np-main np-dep uri-main uri-dep utility'
    main()
