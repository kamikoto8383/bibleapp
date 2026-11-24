#!/bin/bash
# ============================================================================
#  build_deb_universal.sh — сборка BibleApp и .deb для Linux Mint 21.3 / 22.3
# ============================================================================
#  Почему это нужно:
#    Mint 21.3 (Ubuntu 22.04) — Qt 6.2.4, glibc 2.35
#    Mint 22.3 (Ubuntu 24.04) — Qt 6.4.2, glibc 2.39
#
#  Бинарник, собранный на Mint 22.3, содержит версионную метку Qt_6.4 и
#  НЕ запускается на Mint 21.3 (там Qt 6.2). Поэтому:
#    * для поддержки обеих систем собирайте пакет на Mint 21.3
#      (метка будет Qt_6.2 — она совместима и с 22.3);
#    * либо собирайте отдельный пакет на каждой системе.
#
#  Использование:
#    ./build_deb_universal.sh              # зависимости + сборка + .deb
#    ./build_deb_universal.sh --skip-deps  # не ставить пакеты (уже стоят)
#    ./build_deb_universal.sh --no-deb     # только собрать бинарник
#    ./build_deb_universal.sh --install    # собрать .deb и установить его
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

SKIP_DEPS=0
MAKE_DEB=1
DO_INSTALL=0

for arg in "$@"; do
    case "$arg" in
        --skip-deps) SKIP_DEPS=1 ;;
        --no-deb)    MAKE_DEB=0 ;;
        --install)   DO_INSTALL=1 ;;
        -h|--help)   sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "Неизвестный параметр: $arg (см. --help)" >&2; exit 2 ;;
    esac
done

# ----------------------------------------------------------------------------
#  1. Определение дистрибутива
# ----------------------------------------------------------------------------
DISTRO_PRETTY="неизвестный дистрибутив"
DISTRO_ID=""
DISTRO_VER=""
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    DISTRO_PRETTY="${PRETTY_NAME:-${NAME:-unknown}}"
    DISTRO_ID="${ID:-}"
    DISTRO_VER="${VERSION_ID:-}"
fi

echo "==> Система: $DISTRO_PRETTY"
case "$DISTRO_ID:$DISTRO_VER" in
    linuxmint:21*|ubuntu:22.04)  DISTRO_SERIES="mint21" ;;
    linuxmint:22*|ubuntu:24.04)  DISTRO_SERIES="mint22" ;;
    *)                           DISTRO_SERIES="other" ;;
esac
echo "    Серия: $DISTRO_SERIES"
if [ "$DISTRO_SERIES" = "other" ]; then
    echo "    ⚠ Скрипт рассчитан на Mint 21.x/22.x (Ubuntu 22.04/24.04)." >&2
    echo "      Продолжаю, но имена пакетов могут отличаться." >&2
fi

# ----------------------------------------------------------------------------
#  2. Зависимости сборки
# ----------------------------------------------------------------------------
if [ "$SKIP_DEPS" -eq 0 ]; then
    echo "==> Установка зависимостей сборки..."
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config \
        qt6-base-dev qt6-base-dev-tools \
        libqt6sql6-sqlite qt6-qpa-plugins \
        libgl-dev libxcb-cursor0
else
    echo "==> Установка зависимостей пропущена (--skip-deps)"
fi

# ----------------------------------------------------------------------------
#  3. Сборка
# ----------------------------------------------------------------------------
echo "==> Сборка..."
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
JOBS="${JOBS:-$(nproc)}"
cmake --build build -j"$JOBS"

BIN="build/bibleapp"
if [ ! -x "$BIN" ]; then
    echo "✗ Бинарник не собран: $BIN" >&2
    exit 1
fi

# ----------------------------------------------------------------------------
#  4. Анализ совместимости бинарника
# ----------------------------------------------------------------------------
QT_TAG="$(readelf -V "$BIN" 2>/dev/null | grep -oE 'Qt_6\.[0-9]+' | sort -Vu | tail -1 || true)"
GLIBC_TAG="$(readelf -V "$BIN" 2>/dev/null | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -Vu | tail -1 || true)"
QT_MIN="${QT_TAG#Qt_}"
GLIBC_MIN="${GLIBC_TAG#GLIBC_}"

echo ""
echo "==> Совместимость собранного бинарника:"
echo "    Qt:    ${QT_TAG:-не определена}"
echo "    glibc: ${GLIBC_TAG:-не определена}"

case "$QT_MIN" in
    6.0|6.1|6.2)
        COMPAT_VERDICT="✓ работает на Mint 21.3 (Qt 6.2) и Mint 22.3 (Qt 6.4)"
        ;;
    "")
        COMPAT_VERDICT="? не удалось определить требуемую версию Qt"
        ;;
    *)
        COMPAT_VERDICT="✗ требует Qt >= $QT_MIN — на Mint 21.3 (Qt 6.2.4) НЕ запустится.
      Для универсального пакета соберите этот скрипт на Mint 21.3."
        ;;
esac
echo "    $COMPAT_VERDICT"

if [ -n "$GLIBC_MIN" ] && [ "$(printf '%s\n%s\n' "$GLIBC_MIN" "2.35" | sort -V | tail -1)" = "$GLIBC_MIN" ] \
   && [ "$GLIBC_MIN" != "2.35" ]; then
    echo "    ⚠ Требуется glibc >= $GLIBC_MIN, в Mint 21.3 — 2.35."
fi

# ----------------------------------------------------------------------------
#  5. Упаковка .deb
# ----------------------------------------------------------------------------
APP_VERSION="$(sed -n 's/.*setApplicationVersion(QStringLiteral("\([^"]*\)")).*/\1/p' src/main.cpp | head -1)"
APP_VERSION="${APP_VERSION:-1.0.0}"
ARCH="$(dpkg --print-architecture)"

DEB_NAME="bibleapp_${APP_VERSION}_${ARCH}.deb"

if [ "$MAKE_DEB" -eq 1 ]; then
    echo ""
    echo "==> Упаковка $DEB_NAME (версия $APP_VERSION, $ARCH)..."

    STAGE="$(mktemp -d)"
    cleanup() { rm -rf "$STAGE"; }
    trap cleanup EXIT

    # Бинарник и иконка
    install -Dm755 "$BIN" "$STAGE/usr/bin/bibleapp"
    install -Dm644 resources/bibleapp.png "$STAGE/usr/share/pixmaps/bibleapp.png"

    # Словари и общие SQLite-ресурсы
    mkdir -p "$STAGE/usr/share/bibleapp"
    cp resources/*.sqlite "$STAGE/usr/share/bibleapp/" 2>/dev/null || true

    # Шрифты
    if [ -d resources/fonts ]; then
        cp -r resources/fonts "$STAGE/usr/share/bibleapp/fonts"
    fi

    # Встроенные модули (synodal, interlinear)
    mkdir -p "$STAGE/usr/share/bibleapp/modules"
    for mod in synodal interlinear; do
        if [ -d "resources/modules/$mod" ]; then
            cp -r "resources/modules/$mod" "$STAGE/usr/share/bibleapp/modules/$mod"
        fi
    done

    # Ярлык
    mkdir -p "$STAGE/usr/share/applications"
    cat > "$STAGE/usr/share/applications/bibleapp.desktop" <<'DESK'
[Desktop Entry]
Type=Application
Name=Изучение Библии
GenericName=Bible Study
Comment=Чтение и изучение Библии: переводы, Стронг, словари, комментарии
Exec=bibleapp
Icon=/usr/share/pixmaps/bibleapp.png
Terminal=false
Categories=Education;Literature;
Keywords=bible;библия;стронг;перевод;
DESK
    chmod 644 "$STAGE/usr/share/applications/bibleapp.desktop"

    # Контроль пакета.
    # Альтернативы «libqt6xxx | libqt6xxxt64» нужны из-за перехода Ubuntu 24.04
    # на t64-пакеты: пакет, собранный на 22.04, ставится и на 24.04.
    QT_DEP_VERSION="${QT_MIN:-6.2}.0"
    GLIBC_DEP="${GLIBC_MIN:-2.34}"
    INSTALLED_SIZE="$(du -sk "$STAGE" | cut -f1)"

    mkdir -p "$STAGE/DEBIAN"
    cat > "$STAGE/DEBIAN/control" <<EOF
Package: bibleapp
Version: $APP_VERSION
Section: education
Priority: optional
Architecture: $ARCH
Maintainer: BibleApp Developer <bibleapp@local>
Installed-Size: $INSTALLED_SIZE
Depends: libc6 (>= $GLIBC_DEP), libstdc++6, libqt6core6 (>= $QT_DEP_VERSION) | libqt6core6t64, libqt6gui6 | libqt6gui6t64, libqt6widgets6 | libqt6widgets6t64, libqt6printsupport6 | libqt6printsupport6t64, libqt6sql6 | libqt6sql6t64, libqt6sql6-sqlite, qt6-qpa-plugins
Description: Bible study application (Qt 6)
 Программа для изучения Библии: модули BibleQuote, параллельные
 переводы, номера Стронга, словари и комментарии, конвертация
 модулей BibleQuote в SQLite.
EOF

    rm -f "$DEB_NAME"
    dpkg-deb --build --root-owner-group "$STAGE" "$DEB_NAME" >/dev/null
    echo "✓ Создан пакет: $SCRIPT_DIR/$DEB_NAME"
    dpkg-deb -f "$DEB_NAME" Package Version Depends | sed 's/^/    /'
fi

# ----------------------------------------------------------------------------
#  6. Установка
# ----------------------------------------------------------------------------
if [ "$DO_INSTALL" -eq 1 ]; then
    if [ "$MAKE_DEB" -eq 1 ]; then
        echo "==> Установка $DEB_NAME..."
        sudo apt-get install -y "./$DEB_NAME"
    else
        echo "==> Установка без .deb (--no-deb)..."
        sudo install -Dm755 "$BIN" /usr/local/bin/bibleapp
        sudo mkdir -p /usr/share/bibleapp/modules /usr/share/bibleapp/fonts /usr/share/pixmaps
        sudo cp resources/*.sqlite /usr/share/bibleapp/ 2>/dev/null || true
        sudo cp -r resources/fonts/* /usr/share/bibleapp/fonts/ 2>/dev/null || true
        for mod in synodal interlinear; do
            [ -d "resources/modules/$mod" ] || continue
            sudo mkdir -p "/usr/share/bibleapp/modules/$mod"
            sudo cp -r "resources/modules/$mod/." "/usr/share/bibleapp/modules/$mod/"
        done
        sudo cp resources/bibleapp.png /usr/share/pixmaps/bibleapp.png
        printf '%s\n' \
            '[Desktop Entry]' \
            'Type=Application' \
            'Name=Изучение Библии' \
            'Exec=bibleapp' \
            'Icon=/usr/share/pixmaps/bibleapp.png' \
            'Terminal=false' \
            'Categories=Education;Literature;' \
            | sudo tee /usr/share/applications/bibleapp.desktop >/dev/null
    fi
fi

echo ""
echo "Готово. Запуск: bibleapp"
