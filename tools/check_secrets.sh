#!/bin/bash
# 开源前的最后一道闸:扫 git 将要提交/已经跟踪的内容里有没有密钥、
# 私有服务器地址、个人数据。推之前跑一次,非零退出就别推。
#
#   ./tools/check_secrets.sh            # 扫已跟踪 + 已暂存的文件
#   ./tools/check_secrets.sh --history  # 连历史提交一起扫(慢)
set -u
cd "$(dirname "$0")/.."

FAIL=0

note() { printf '  %s\n' "$*"; }
bad()  { FAIL=1; printf '\033[31m  !! %s\033[0m\n' "$*"; }

# 会被 git 带走的文件:已跟踪 + 已暂存 + 未跟踪但没被忽略的
# (最后一类最要紧 —— 一个 git add -A 就会把它们全带走)
files() {
    { git ls-files
      git diff --cached --name-only
      git ls-files --others --exclude-standard
    } 2>/dev/null | sort -u
}

echo "== 1. 敏感文件是否真的被忽略 =="
for f in main/chat_config.h server/.env server/voice.log; do
    if git check-ignore -q "$f" 2>/dev/null; then
        note "$f 已忽略"
    else
        bad "$f 没有被 .gitignore 忽略"
    fi
    if git ls-files --error-unmatch "$f" >/dev/null 2>&1; then
        bad "$f 已经被 git 跟踪了!需要 git rm --cached"
    fi
done

echo "== 2. 将要提交的内容里扫密钥特征 =="
# sk-/gsk_ 开头的 key、Bearer token、私钥块
PAT='(sk-[A-Za-z0-9_-]{16,}|gsk_[A-Za-z0-9]{16,}|AIza[A-Za-z0-9_-]{20,}|BEGIN [A-Z ]*PRIVATE KEY|Authorization: *Bearer +[A-Za-z0-9._-]{16,})'
HITS=$(files | while read -r f; do
    [ -f "$f" ] || continue
    grep -InE "$PAT" "$f" 2>/dev/null | sed "s|^|$f:|"
done)
if [ -n "$HITS" ]; then
    bad "疑似密钥:"
    echo "$HITS" | sed 's/^/     /'
else
    note "没扫到密钥特征"
fi

echo "== 3. 硬编码的公网地址(应当只出现在 .example 里的占位值)=="
IPS=$(files | while read -r f; do
    [ -f "$f" ] || continue
    case "$f" in *.example|*.md) continue;; esac
    grep -InE '\b((25[0-5]|2[0-4][0-9]|1?[0-9]?[0-9])\.){3}(25[0-5]|2[0-4][0-9]|1?[0-9]?[0-9])\b' "$f" 2>/dev/null \
        | grep -vE '127\.0\.0\.1|0\.0\.0\.0|192\.168\.4\.1|255\.|1\.1\.1\.1' | sed "s|^|$f:|"
done)
if [ -n "$IPS" ]; then
    bad "疑似写死的 IP(确认不是你自己的服务器):"
    echo "$IPS" | sed 's/^/     /'
else
    note "没扫到可疑 IP"
fi

if [ "${1:-}" = "--history" ]; then
    echo "== 4. 历史提交里扫(慢)=="
    H=$(git log -p --all 2>/dev/null | grep -InE "$PAT" | head -20)
    if [ -n "$H" ]; then
        bad "历史提交里有疑似密钥 —— 改 .gitignore 没用,得重写历史(git filter-repo)"
        echo "$H" | sed 's/^/     /'
    else
        note "历史干净"
    fi
fi

echo
if [ "$FAIL" = 0 ]; then
    echo -e "\033[32m通过:没发现会泄露的内容\033[0m"
else
    echo -e "\033[31m有问题,先处理完再推\033[0m"
fi
exit $FAIL
