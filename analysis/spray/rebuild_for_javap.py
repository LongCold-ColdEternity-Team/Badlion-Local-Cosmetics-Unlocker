"""Build inspection-only class shells from JVMTI metadata. NOT executable:
exception tables, annotations, bootstrap data, stack maps and debug attrs are
not exported. Use javap -p -c only; never put these in the game classpath.
"""
import pathlib
import struct
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent / 'probe-bin'
JAVAP = pathlib.Path('C:/Program Files/Eclipse Adoptium/jdk-17.0.19.10-hotspot/bin/javap.exe')
u2 = lambda n: struct.pack('>H', n)
u4 = lambda n: struct.pack('>I', n)

for meta in (ROOT / 'classes').rglob('*.meta'):
    lines = [x.split('\t') for x in meta.read_text().splitlines()]
    _, name, flags, count, error = lines[0]
    if error != '0':
        continue
    cp = bytearray(meta.with_suffix('.cp').read_bytes())
    count = int(count)
    def utf(s):
        global count
        b = s.encode('utf-8'); i = count; count += 1
        cp.extend(b'\x01' + u2(len(b)) + b)
        return i
    def cls(s):
        global count
        ui = utf(s); i = count; count += 1; cp.extend(b'\x07' + u2(ui))
        return i
    this = cls(name)
    super_name = next((x[1][1:-1] for x in lines if x[0] == 'SUPER'), 'java/lang/Object')
    super_idx = cls(super_name)
    code_idx = utf('Code')
    fields, methods = [], []
    for x in lines[1:]:
        if x[0] == 'FIELD':
            fields.append(u2(int(x[1]) & 0xffff) + u2(utf(x[2])) + u2(utf(x[3])) + u2(0))
        elif x[0] == 'METHOD':
            head = u2(int(x[1]) & 0xffff) + u2(utf(x[2])) + u2(utf(x[3]))
            code = bytes.fromhex(x[5]) if len(x) > 5 else b''
            if x[4] == '0' and code:
                attr = u2(256) + u2(256) + u4(len(code)) + code + u2(0) + u2(0)
                methods.append(head + u2(1) + u2(code_idx) + u4(len(attr)) + attr)
            else:
                methods.append(head + u2(0))
    data = b'\xca\xfe\xba\xbe' + u2(0) + u2(52) + u2(count) + cp
    data += u2(int(flags) & 0xffff) + u2(this) + u2(super_idx) + u2(0)
    data += u2(len(fields)) + b''.join(fields) + u2(len(methods)) + b''.join(methods) + u2(0)
    dest = ROOT / 'inspection-only' / (name + '.class')
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    run = subprocess.run([str(JAVAP), '-p', '-c', str(dest)], capture_output=True)
    dest.with_suffix('.javap.txt').write_bytes(run.stdout + run.stderr)
    print(name, 'exit=', run.returncode)
