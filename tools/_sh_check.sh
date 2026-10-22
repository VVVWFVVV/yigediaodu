#!/bin/sh
# 临时语法校验脚本：对一个调度全部 sh 脚本跑 sh -n + CRLF 检查
cd /d/新调度 || exit 1
rc=0
for f in customize.sh post-fs-data.sh service.sh action.sh uninstall.sh \
         script/pathinfo.sh script/setup.sh script/fcctl.sh script/diag.sh \
         META-INF/com/google/android/update-binary; do
    if sh -n "$f" 2>err.tmp; then
        echo "OK   $f"
    else
        echo "FAIL $f"
        cat err.tmp
        rc=1
    fi
done
rm -f err.tmp
echo "---"
for f in $(find . -name '*.sh' -not -path './tools/_*'); do
    if grep -q $'\r' "$f" 2>/dev/null; then
        echo "CRLF $f"
        rc=1
    fi
done
[ "$rc" = "0" ] && echo "ALL PASS (syntax + LF)" || echo "HAS FAILURE"
exit $rc
