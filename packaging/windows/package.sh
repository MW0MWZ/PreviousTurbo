#!/bin/sh
# In an MSYS2 UCRT64 shell, from the source folder, with the packages listed in
# .github/workflows/release.yml and zip: builds and makes
# out/PreviousTurbo-VERSION-windows-x86_64.zip, with the NeXT ROMs, the DLLs the programs load,
# the documents and the licences of everything in it.
set -e
b=build-release
cmake -S . -B $b -G Ninja -DCMAKE_BUILD_TYPE=Release -DBINDIR=. \
      -DCMAKE_EXE_LINKER_FLAGS='-static-libstdc++ -static-libgcc' 2>&1 | tee configure.log
for lib in png; do
  grep -q -- "- $lib : *found" configure.log || { echo "$lib was not found"; exit 1; }
done
cmake --build $b
ctest --test-dir $b --output-on-failure
v=$(sed -n 's/^project(PreviousTurbo VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
top=PreviousTurbo-$v-windows-x86_64
s=stage/$top
rm -rf stage && mkdir -p $s/licenses out
cmake --install $b --prefix "$PWD/$s"
strip $s/*.exe
cp README.md readme.previous.txt networking.howto.txt netboot.howto.txt filesharing.howto.txt $s/
cp gpl.txt $s/COPYING.txt
cp third_party/imgui/LICENSE.txt $s/licenses/imgui.txt
cp src/slirp/COPYRIGHT $s/licenses/slirp.txt
# The DLLs (all of them, through their own imports) and the packages they come from.
for dll in $(ldd $b/previousturbo.exe $b/ditool.exe | awk '$3 ~ /^\/ucrt64\// {print $3}' | sort -u); do
  cp $dll $s/
  pacman -Qqo $dll
done | sort -u | while read -r pkg; do
  n=0
  for f in $(pacman -Qlq $pkg | grep '/share/licenses/.' | grep -v '/$'); do
    cp $f "$s/licenses/${pkg#mingw-w64-ucrt-x86_64-}-$(basename $f)" && n=1
  done
  [ $n = 1 ] || echo "warning: no licence file in $pkg"
done
ls $s $s/licenses
(cd stage && zip -9 -r ../out/$top.zip $top)
