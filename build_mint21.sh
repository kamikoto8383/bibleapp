#!/bin/bash
# Сборка «Программы по изучению БИБЛИИ» на Linux Mint 21.3 / Ubuntu 22.04
set -e
cd "$(dirname "$0")"

echo "==> Установка зависимостей сборки..."
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake pkg-config \
  qt6-base-dev qt6-base-dev-tools \
  libqt6sql6-sqlite qt6-qpa-plugins \
  libgl-dev libgl1-mesa-dev \
  libxcb-cursor0

echo "==> Сборка..."
rm -rf build
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j"$(nproc)"

echo "==> Установка в /usr/local ..."
sudo cmake --install . 2>/dev/null || {
  sudo cp bibleapp /usr/local/bin/bibleapp
  sudo mkdir -p /usr/share/bibleapp/modules/synodal \
                /usr/share/bibleapp/modules/interlinear \
                /usr/share/bibleapp/fonts \
                /usr/share/pixmaps \
                /usr/share/applications
  sudo cp -r ../resources/*.sqlite /usr/share/bibleapp/ 2>/dev/null || true
  sudo cp -r ../resources/modules/synodal/* /usr/share/bibleapp/modules/synodal/ 2>/dev/null || true
  sudo cp -r ../resources/modules/interlinear/* /usr/share/bibleapp/modules/interlinear/ 2>/dev/null || true
  sudo cp ../resources/fonts/*.ttf /usr/share/bibleapp/fonts/ 2>/dev/null || true
  sudo cp ../resources/bibleapp.png /usr/share/pixmaps/bibleapp.png 2>/dev/null || true
  cat << 'DESK' | sudo tee /usr/share/applications/bibleapp.desktop >/dev/null
[Desktop Entry]
Type=Application
Name=Изучение Библии
Exec=bibleapp
Icon=/usr/share/pixmaps/bibleapp.png
Terminal=false
Categories=Education;
DESK
}

echo ""
echo "Готово. Запуск: bibleapp"
echo "Если ярлык не появился — перезайдите в сессию или выполните:"
echo "  update-desktop-database ~/.local/share/applications 2>/dev/null || true"
