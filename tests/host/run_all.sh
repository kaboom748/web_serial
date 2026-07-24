#!/usr/bin/env bash
# web_serial host suites -- one command, CI and bench alike.
set -e
cd "$(dirname "$0")"
python3 - <<'PY'
import re
s=open('../../components/web_serial/web_serial.cpp').read()
html=re.search(r'R"HTMLDOC\((.*?)\)HTMLDOC"',s,re.S).group(1)
open('/tmp/wser_page.html','w').write(html)
print("page extracted:", len(html), "bytes")
PY
for t in test_crc16 test_framing test_guard test_switch test_loopdet test_shaper; do
  cc "$t.c" -o "/tmp/$t"
  "/tmp/$t"
done
node test_decoder.js
if [ ! -d node_modules/jsdom ]; then npm i --no-save jsdom@24 >/dev/null 2>&1 || true; fi
node test_ui.js /tmp/wser_page.html
