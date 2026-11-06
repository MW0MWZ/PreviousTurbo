#!/bin/sh
# On macOS, from the source folder, with the README's Homebrew packages: builds and makes
# out/PreviousTurbo-VERSION-macos-ARCH.zip, the app (with the NeXT ROMs and its libraries
# copied in, signed by cmake --install), the documents and the licences of everything in it.
# MACOS_MIN, if set, is the oldest macOS the download is said to run on: it stops if any of
# the app's code needs a newer one.
set -e
b=build-release
cmake -S . -B $b -G Ninja -DCMAKE_BUILD_TYPE=Release 2>&1 | tee configure.log
for lib in png readline pcap xattr.h; do
  grep -q -- "- $lib : *found" configure.log || { echo "$lib was not found"; exit 1; }
done
cmake --build $b
ctest --test-dir $b --output-on-failure
v=$(sed -n 's/^project(PreviousTurbo VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
top=PreviousTurbo-$v-macos-$(uname -m)
s=stage/$top
rm -rf stage && mkdir -p $s/licenses out
cmake --install $b --prefix "$PWD/$s"  # (an absolute path, as fixup_bundle needs)
app=$s/PreviousTurbo.app
codesign --verify --deep --strict $app
cp README.md readme.previous.txt networking.howto.txt netboot.howto.txt filesharing.howto.txt $s/
cp gpl.txt $s/COPYING.txt
cp third_party/imgui/LICENSE.txt $s/licenses/imgui.txt
cp src/slirp/COPYRIGHT $s/licenses/slirp.txt
# The Homebrew formulas the libraries come from, through their own imports.
brew_libs() { otool -L "$1" | awk 'NR > 1 {print $1}' | grep "^$(brew --prefix)/" || true; }
seen= todo=$(brew_libs $b/PreviousTurbo.app/Contents/MacOS/PreviousTurbo)
while [ -n "$todo" ]; do
  set -- $todo; l=$1; shift; todo="$*"
  case " $seen " in *" $l "*) continue;; esac
  seen="$seen $l"; todo="$todo $(brew_libs $l)"
done
for f in $(for l in $seen; do realpath $l; done | sed -n "s|^$(brew --prefix)/Cellar/\([^/]*\)/.*|\1|p" | sort -u); do
  n=0
  for lf in $(brew --prefix $f)/LICENSE* $(brew --prefix $f)/LICENCE* $(brew --prefix $f)/COPYING* $(brew --prefix $f)/COPYRIGHT*; do
    [ -f "$lf" ] && cp "$lf" "$s/licenses/$f-$(basename "$lf")" && n=1
  done
  # Without a file in the keg, the standard text of the formula's licence.
  if [ $n = 0 ]; then
    id=$(brew info --json=v2 $f | python3 -c 'import json, sys; print(json.load(sys.stdin)["formulae"][0]["license"])')
    curl -fsSL -o "$s/licenses/$f-$id.txt" "https://raw.githubusercontent.com/spdx/license-list-data/main/text/$id.txt" ||
      { echo "no licence for $f ($id)"; exit 1; }
  fi
done
ls $s/licenses
# The newest macOS any of the app's code was built for.
need=$(find $app/Contents -type f \( -perm -u+x -o -name '*.dylib' \) | while read -r f; do
  otool -l "$f" | awk '/LC_BUILD_VERSION/ {b = 1} b && /minos/ {print $2; exit}'
done | sort -t. -k1,1n -k2,2n | tail -1)
echo "needs macOS $need"
if [ -n "$MACOS_MIN" ] && [ "$(printf '%s\n%s\n' "$need" "$MACOS_MIN" | sort -t. -k1,1n -k2,2n | tail -1)" != "$MACOS_MIN" ]; then
  echo "needs macOS $need, newer than $MACOS_MIN"; exit 1
fi
(cd stage && ditto -c -k --keepParent $top ../out/$top.zip)
