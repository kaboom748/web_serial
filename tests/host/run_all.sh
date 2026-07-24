#!/usr/bin/env bash
# web_serial host suites -- one command, CI and bench alike.
# Self-diagnosing: every failure states its cause in the log (never silent).
set -e
cd "$(dirname "$0")"
echo "== env: $(uname -s) | node $(node --version 2>/dev/null || echo MISSING) | npm $(npm --version 2>/dev/null || echo MISSING) | cc $(cc --version 2>/dev/null | head -1 || echo MISSING)"

echo "== extracting the UI page from PROGMEM"
python3 - <<'PY'
import re
s=open('../../components/web_serial/web_serial.cpp').read()
html=re.search(r'R"HTMLDOC\((.*?)\)HTMLDOC"',s,re.S).group(1)
open('/tmp/wser_page.html','w').write(html)
print("   page:", len(html), "bytes")
PY

for t in test_crc16 test_framing test_guard test_switch test_loopdet test_shaper; do
  echo "== $t"
  cc "$t.c" -o "/tmp/$t" || { echo "::error::cc failed on $t.c"; exit 1; }
  "/tmp/$t" || { echo "::error::$t reported failures"; exit 1; }
done

echo "== test_decoder"
node test_decoder.js || { echo "::error::decoder mirrors failed"; exit 1; }

echo "== jsdom for the UI suite"
if ! node -e "require.resolve('jsdom')" 2>/dev/null; then
  npm install --no-save --no-audit --no-fund jsdom@24 || { echo "::error::npm install jsdom failed -- see npm output above"; exit 1; }
fi

echo "== test_ui (177 assertions on the extracted page)"
node test_ui.js /tmp/wser_page.html || { echo "::error::UI suite failed"; exit 1; }
echo "== ALL HOST SUITES GREEN"
