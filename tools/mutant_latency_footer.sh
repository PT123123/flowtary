#!/bin/bash
# latency_footer_ui 门的 mutant 自证（不碰共享树源码，全部在 build/mutant 副本里做）：
#   1) 逐字节相同的控制副本必须全绿；
#   2) 把 pending 分支的阈值条件行 return ms >= kSlowMs 改成 >= 1000000（只改条件行），
#      门必须红在 "pending slow reports live elapsed"。
# 任一腿不符合预期即退出 1。用法：bash tools/mutant_latency_footer.sh
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
M="$R/build/mutant"
MW=$(cygpath -m "$M")
BW=$(cygpath -m "$R/build")
mkdir -p "$M/src" "$M/tests" "$M/obj"
cp "$R"/src/*.h "$M/src/"
cp "$R/src/main.cpp" "$M/src/main.cpp"
cp "$R/tests/latency_footer_ui_test.cpp" "$M/tests/"

export MSYS2_ARG_CONV_EXCL='*'
CL="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe"
LK="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/link.exe"

build() {
  "$CL" /nologo /TP /DWIN32 /D_WINDOWS /EHsc /O2 /Ob2 /DNDEBUG -std:c++17 -MT /W3 /utf-8 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS \
   "-IC:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/include" \
   "-IC:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/um" \
   "-IC:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/ucrt" \
   "-IC:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/shared" \
   "-IC:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/winrt" \
   "-I$MW/src" \
   "/Fo$MW/obj/lat_test.obj" "/Fd$MW/obj/lat.pdb" /FS -c "$MW/tests/latency_footer_ui_test.cpp" > "$M/compile.log" 2>&1
  "$LK" /nologo "$MW/obj/lat_test.obj" \
   "$BW/CMakeFiles/latency_footer_ui_test.dir/src/filedlg_jump.cpp.obj" \
   "$BW/CMakeFiles/latency_footer_ui_test.dir/src/settings_model.cpp.obj" \
   "$BW/CMakeFiles/latency_footer_ui_test.dir/src/search_history.cpp.obj" \
   "/out:$MW/$1" /machine:x64 /INCREMENTAL:NO /subsystem:console \
   "-LIBPATH:C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/lib/x64" \
   "-LIBPATH:C:/Program Files (x86)/Windows Kits/10/Lib/10.0.26100.0/um/x64" \
   "-LIBPATH:C:/Program Files (x86)/Windows Kits/10/Lib/10.0.26100.0/ucrt/x64" \
   user32.lib gdi32.lib shell32.lib ole32.lib imm32.lib dwmapi.lib advapi32.lib uxtheme.lib msimg32.lib oleaut32.lib uuid.lib shlwapi.lib kernel32.lib winspool.lib comdlg32.lib > "$M/link.log" 2>&1
}

build control_copy.exe
(cd "$M" && ./control_copy.exe) || { echo "MUTANT GATE BROKEN: control copy failed"; exit 1; }

# 只改 pending 分支的条件行（8 空格缩进那行），不动报齐分支
sed -i 's/^        return ms >= kSlowMs;/        return ms >= 1000000;/' "$M/src/main.cpp"
build mutant.exe
out=$(cd "$M" && ./mutant.exe 2>&1) && rc=0 || rc=$?
if [ "$rc" -ne 1 ] || ! printf '%s' "$out" | grep -q "FAIL: pending slow reports live elapsed"; then
  echo "MUTANT GATE BROKEN: mutant exit=$rc out=$out"
  exit 1
fi
echo "mutant self-proof ok: control green, threshold mutant red on expected assertion"