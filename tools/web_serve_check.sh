#!/usr/bin/env bash
# ============================================================================
# 网页服务回归检查(2026-10-15):每次改完网页/HTTP 相关代码、烧录后必跑一遍。
# 覆盖的历史教训:
#   - page_read_failed:低堆下 per-request fopen/fclose 随机失败(句柄缓存已修)
#   - 乱码:句柄缓存命中路径漏发 Content-Encoding: gzip,浏览器把 gzip 字节当文本渲染
#   - 半截页面:发送中断返回 <完整字节数
# 用法:tools/web_serve_check.sh <base_url> [user] [pass]
#   示例:tools/web_serve_check.sh http://192.168.1.42 admin 123456
# 判据(全部通过才 exit 0):
#   1) 登录拿 Cookie;
#   2) /config 连续请求两次(冷缓存 + 热缓存):均 200、
#      Content-Encoding: gzip 存在、body 完整(≥20000 字节)且 gzip 可解出 <!DOCTYPE html;
#   3) 带 If-None-Match 再请求 → 304。
# ============================================================================
set -u
BASE="${1:?用法: web_serve_check.sh <base_url> [user] [pass]}"
USER="${2:-admin}"
PASS="${3:-123456}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FAIL=0

note() { printf '%s\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*"; FAIL=1; }

# 1) 登录
curl -s -m 8 -c "$TMP/c" -d "username=$USER&password=$PASS" \
     -o /dev/null "$BASE/api/v1/auth/login" || fail "login 请求失败"

check_once() {
    local tag="$1"
    curl -s -m 20 -b "$TMP/c" -H "Accept-Encoding: gzip" \
         -D "$TMP/h_$tag" -o "$TMP/b_$tag" "$BASE/config"
    local code="$(grep -i '^HTTP/' "$TMP/h_$tag" | tail -1 | awk '{print $2}')"
    local enc="$(grep -i '^Content-Encoding:' "$TMP/h_$tag" | tr -d '\r')"
    local size="$(wc -c < "$TMP/b_$tag")"
    [ "$code" = "200" ] || { fail "$tag: HTTP $code"; return; }
    case "$enc" in
        *gzip*) : ;;
        *) fail "$tag: 缺 Content-Encoding: gzip(浏览器将显示乱码)"; return ;;
    esac
    [ "$size" -ge 20000 ] || { fail "$tag: body 过短($size 字节,疑似半截)"; return; }
    local head="$(zcat "$TMP/b_$tag" 2>/dev/null | head -c 15)"
    case "$head" in
        '<!DOCTYPE html>') : ;;
        *) fail "$tag: gzip 解压头不对($head)"; return ;;
    esac
    note "PASS: $tag 200 / gzip 头存在 / $size 字节 / 解压为完整 HTML"
}

# 2) 冷+热两次
check_once first
check_once second

# 3) ETag/304
ETAG="$(grep -i '^ETag:' "$TMP/h_first" | tr -d '\r' | awk '{print $2}')"
if [ -n "$ETAG" ]; then
    code304="$(curl -s -m 10 -b "$TMP/c" -H "Accept-Encoding: gzip" -H "If-None-Match: $ETAG" \
              -o /dev/null -w '%{http_code}' "$BASE/config")"
    [ "$code304" = "304" ] && note "PASS: If-None-Match -> 304" \
                            || fail "If-None-Match -> $code304(期望 304)"
else
    fail "响应缺 ETag"
fi

[ "$FAIL" -eq 0 ] && note "ALL PASS" || note "RESULT: FAIL"
exit "$FAIL"
