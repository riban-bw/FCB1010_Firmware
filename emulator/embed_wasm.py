# Puts the emulator's WebAssembly module into emulator/index.html as base64, between the
# wasm:begin and wasm:end markers, so the emulator stays a single self-contained file.
# Usage: python3 -I emulator/embed_wasm.py build/fcb1010_emu.wasm emulator/index.html
import base64
import re
import sys

wasm_path, html_path = sys.argv[1], sys.argv[2]
data = base64.b64encode(open(wasm_path, 'rb').read()).decode('ascii')
html = open(html_path, encoding='utf-8').read()
pattern = re.compile(r'(/\* wasm:begin \*/ ")[A-Za-z0-9+/=]*(" /\* wasm:end \*/)')
if len(pattern.findall(html)) != 1:
    sys.exit('embed_wasm: expected exactly one wasm:begin/wasm:end marker pair in ' + html_path)
html = pattern.sub(lambda m: m.group(1) + data + m.group(2), html)
open(html_path, 'w', encoding='utf-8').write(html)
print('embedded %d bytes of WebAssembly into %s' % (len(base64.b64decode(data)), html_path))
