#!/system/bin/sh
# 改 SSC JSON 配置里某个字段的 data 值，并回读验证
# 用法: setfield.sh <文件> <字段名> <新值>
F="$1"; KEY="$2"; VAL="$3"
[ -f "$F" ] || { echo "FAIL: 文件不存在 $F"; exit 1; }
grep -q "\"$KEY\"" "$F" || { echo "FAIL: 文件里没有字段 $KEY"; exit 1; }
awk -v k="$KEY" -v v="$VAL" '
  { if ($0 ~ "\""k"\"[ ]*:") { f=1; print; next }
    if (f && $0 ~ /"data"/) { sub(/"[^"]*"[ ]*$/, "\"" v "\""); f=0 }
    print }' "$F" > "$F.new" && mv "$F.new" "$F"
# 回读验证
GOT=$(awk -v k="$KEY" '
  { if ($0 ~ "\""k"\"[ ]*:") { f=1; next }
    if (f && $0 ~ /"data"/) { gsub(/.*"data"[ ]*:[ ]*"/,""); gsub(/".*/,""); print; exit } }' "$F")
if [ "$GOT" = "$VAL" ]; then echo "OK  $KEY = $GOT"; else echo "FAIL: $KEY 回读得到 [$GOT]，期望 [$VAL]"; exit 1; fi
