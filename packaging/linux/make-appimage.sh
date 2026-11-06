#!/bin/sh
# In the packaging/linux container: builds /src and makes
# /out/PreviousTurbo-VERSION-linux-ARCH.AppImage, with the NeXT ROMs, the documents and the
# licences of everything built in or bundled.
set -e
b=/tmp/build
cmake -S /src -B $b -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
      -DCMAKE_EXE_LINKER_FLAGS='-static-libstdc++ -static-libgcc' 2>&1 | tee /tmp/configure.log
for lib in png pcap xattr.h; do
  grep -q -- "- $lib : *found" /tmp/configure.log || { echo "$lib was not found"; exit 1; }
done
cmake --build $b
ctest --test-dir $b --output-on-failure
v=$(sed -n 's/^project(PreviousTurbo VERSION \([0-9.]*\).*/\1/p' /src/CMakeLists.txt)
d=/tmp/AppDir
rm -rf $d
DESTDIR=$d cmake --install $b
strip $d/usr/bin/*
doc=$d/usr/share/doc/previousturbo
l=$doc/licenses
mkdir -p $l
cp /src/README.md /src/readme.previous.txt /src/networking.howto.txt /src/netboot.howto.txt \
   /src/filesharing.howto.txt $doc/
cp /src/gpl.txt $doc/COPYING.txt
cp /src/third_party/imgui/LICENSE.txt $l/imgui.txt
cp /src/src/slirp/COPYRIGHT $l/slirp.txt
cp /usr/local/share/licenses/SDL3/LICENSE.txt $l/SDL3.txt
cd /tmp
linuxdeploy --appdir $d -d $d/usr/share/applications/previousturbo.desktop \
             -i $d/usr/share/icons/hicolor/256x256/apps/previousturbo.png
# Each bundled distribution library's copyright file, by package.
for f in $d/usr/lib/*.so*; do
  n=$(basename $f)
  [ $n = libSDL3.so.0 ] && continue
  p=
  for q in /lib/$(uname -m)-linux-gnu/$n /usr/lib/$(uname -m)-linux-gnu/$n; do
    p=$(dpkg -S $q 2>/dev/null | cut -d: -f1) && [ -n "$p" ] && break
  done
  [ -n "$p" ] || { echo "no package for $n"; exit 1; }
  cp /usr/share/doc/$p/copyright $l/$p.txt
done
ls $l
LDAI_OUTPUT=/out/PreviousTurbo-$v-linux-$(uname -m).AppImage linuxdeploy --appdir $d --output appimage
ls -l /out
