#!/usr/bin/env bash
# build-ch341-apps.sh - build IMSProg and SFP-Master for Windows with the libusb341
# shim, so a CH341A works through WCH's own driver (no libusbK/WinUSB rebinding).
#
# Run it from the MSYS2 MINGW64 (or UCRT64) shell:  ~/build-ch341-apps.sh
#
# On a new MSYS2 install, fetch it first:
#   curl -fsSL https://raw.githubusercontent.com/Anime4000/libusb341/main/tools/build-ch341-apps.sh -o ~/build-ch341-apps.sh
#   bash ~/build-ch341-apps.sh
#
# Everything happens in the home folder:
#   ~/IMSProg  ~/SFP-Master  ~/libusb341       sources: cloned if missing, else updated
#   ~/build-imsprog  ~/build-sfp-master        build trees
#   ~/IMSProg_win-amd64-shim                   ready-to-run programs; copy these folders
#   ~/SFP-Master_win-amd64-shim                to any 64-bit Windows PC (no MSYS2 needed)
#
# Settings (environment variables):
#   IMSPROG_URL, SFPMASTER_URL   git URLs to clone from (default: the Anime4000 forks)
#   LIBUSB341_URL                git URL of libusb341 (default: github.com/Anime4000/libusb341)
#   ONLY=imsprog | sfp-master    build just one of the two
#   SKIP_PACMAN=1                do not install/check MSYS2 packages
#   SKIP_UPDATE=1                do not git pull existing checkouts
#   RUN_TESTS=1                  run the libusb341 simulator tests first
#   WORKDIR=/some/dir            use another folder instead of ~
#
# The master copy is libusb341/tools/build-ch341-apps.sh; when libusb341 is updated
# and carries a newer SCRIPT_VERSION, this script replaces itself with it and restarts.

set -euo pipefail

SCRIPT_VERSION=2

WORKDIR=${WORKDIR:-$HOME}
IMSPROG_URL=${IMSPROG_URL:-https://github.com/Anime4000/IMSProg.git}
SFPMASTER_URL=${SFPMASTER_URL:-https://github.com/Anime4000/SFP-Master.git}
LIBUSB341_URL=${LIBUSB341_URL:-https://github.com/Anime4000/libusb341.git}
ONLY=${ONLY:-}

log()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

case "${MSYSTEM:-}" in
	MINGW64|UCRT64) ;;
	*) die "run this from the MSYS2 MINGW64 or UCRT64 shell (MSYSTEM=${MSYSTEM:-unset})" ;;
esac
prefix=$MINGW_PREFIX            # /mingw64 or /ucrt64
pkg=$MINGW_PACKAGE_PREFIX       # mingw-w64-x86_64 or mingw-w64-ucrt-x86_64
mkdir -p "$WORKDIR"
WORKDIR=$(cd "$WORKDIR" && pwd)
SHIM=$WORKDIR/libusb341

# ---------------------------------------------------------------------------

install_packages() {
	[ -n "${SKIP_PACMAN:-}" ] && return
	log "Installing build packages"
	pacman -S --needed --noconfirm git make \
		"$pkg-gcc" "$pkg-binutils" "$pkg-pkgconf" "$pkg-cmake" "$pkg-ninja" \
		"$pkg-qt6-base" "$pkg-qt6-tools" "$pkg-libusb" "$pkg-libftdi" ||
		die "pacman failed; update MSYS2 first with: pacman -Syu"
}

# sync_repo <dir> <url>: clone if missing, fast-forward if present.
sync_repo() {
	local dir=$1 url=$2
	if [ -d "$dir/.git" ]; then
		[ -n "${SKIP_UPDATE:-}" ] && return 0
		log "Updating $dir"
		git -C "$dir" pull --ff-only ||
			warn "could not fast-forward $dir (local changes or diverged); building it as it is"
	elif [ -d "$dir" ]; then
		warn "$dir is not a git checkout; building it as it is"
	elif [ -n "$url" ]; then
		log "Cloning $url"
		GIT_TERMINAL_PROMPT=0 git clone "$url" "$dir" || return 1
	else
		return 1
	fi
}

sync_shim() {
	sync_repo "$SHIM" "$LIBUSB341_URL" ||
		die "could not get libusb341 from ${LIBUSB341_URL:-(no URL)}: copy the folder to $SHIM or set LIBUSB341_URL"
}

# Replace this script with libusb341's copy if that one is newer, then restart it.
self_update() {
	local master="$SHIM/tools/build-ch341-apps.sh" me ver
	[ -f "$master" ] || return 0
	me=$(realpath "$0")
	[ "$me" = "$(realpath "$master")" ] && return 0
	# tr: a checkout made with Git for Windows may have CRLF line endings
	ver=$(tr -d '\r' < "$master" | sed -n 's/^SCRIPT_VERSION=\([0-9][0-9]*\)$/\1/p' | head -n 1)
	[ "${ver:-0}" -gt "$SCRIPT_VERSION" ] || return 0
	[ -z "${CH341_APPS_RESTARTED:-}" ] || return 0
	log "Updating $me to version $ver from libusb341"
	tr -d '\r' < "$master" > "$me.new"
	mv -f "$me.new" "$me"
	chmod +x "$me"
	export CH341_APPS_RESTARTED=1
	exec bash "$me" "$@"
}

build_shim() {
	log "Building libusb341"
	make -C "$SHIM"
	if [ -n "${RUN_TESTS:-}" ]; then
		log "Testing libusb341 (simulated CH341A)"
		make -C "$SHIM" test IMSPROG="$WORKDIR/IMSProg" SFPMASTER="$WORKDIR/SFP-Master"
	fi
}

# build_app <source dir> <build dir>
build_app() {
	log "Building $(basename "$1")"
	cmake -S "$1" -B "$2" -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build "$2"
}

# Print "<dll name> <path>" for every DLL from $prefix/bin that something in the folder loads.
mingw_deps() {
	find "$1" \( -name '*.exe' -o -name '*.dll' \) -print0 |
		while IFS= read -r -d '' f; do ldd "$f" 2>/dev/null || true; done |
		awk -v p="$prefix/bin/" 'index($3, p) == 1 {print $1, $3}' | sort -u
}

copy_runtime_dlls() {
	local dir=$1 pass name path
	for pass in 1 2 3; do       # copied DLLs can need more DLLs
		while read -r name path; do
			[ "$name" = libusb-1.0.dll ] && continue    # the shim takes this name
			[ -f "$dir/$name" ] || cp "$path" "$dir/"
		done < <(mingw_deps "$dir")
	done
}

# deploy <destination> <files...>: assemble in <destination>.new, swap in when complete.
deploy() {
	local dest=$1 tmp=$1.new exe app missing
	shift
	log "Deploying $(basename "$dest")"
	rm -rf "$tmp"
	mkdir -p "$tmp"
	cp -f "$@" "$tmp/"
	for exe in "$tmp"/*.exe; do
		windeployqt6 --release --no-translations --no-system-d3d-compiler --no-opengl-sw \
			--dir "$tmp" "$exe" >/dev/null 2>&1 || die "windeployqt6 failed for $exe"
	done
	copy_runtime_dlls "$tmp"

	# The shim answers as libusb-1.0.dll; the real libusb stays available to it
	# as libusb-1.0.orig.dll for FT232H, CH347 and CH341s not on the WCH driver.
	cp -f "$prefix/bin/libusb-1.0.dll" "$tmp/libusb-1.0.orig.dll"
	cp -f "$SHIM/build/libusb-1.0.dll" "$SHIM/build/ch341probe.exe" "$tmp/"

	for app in IMSProg.exe SFP-Master.exe; do
		[ -f "$tmp/$app" ] || continue
		{ ldd "$tmp/$app" 2>/dev/null || true; } | grep -q 'libusb-1.0.dll' ||
			die "$app does not load libusb-1.0.dll (libusb linked statically?); the shim would not be used"
	done
	missing=$(mingw_deps "$tmp" | while read -r name path; do [ -f "$tmp/$name" ] || echo "$name"; done)
	[ -z "$missing" ] || die "DLLs still missing from $tmp: $missing"

	rm -rf "$dest"
	mv "$tmp" "$dest"
}

do_imsprog() {
	local src=$WORKDIR/IMSProg bld=$WORKDIR/build-imsprog qm
	sync_repo "$src" "$IMSPROG_URL" || die "could not clone $IMSPROG_URL"
	build_app "$src" "$bld"
	mapfile -d '' qm < <(find "$bld" -name '*.qm' -print0)
	deploy "$WORKDIR/IMSProg_win-amd64-shim" \
		"$bld/IMSProg_programmer/IMSProg.exe" \
		"$bld/IMSProg_editor/IMSProg_editor.exe" \
		"$bld/IMSProg_database_update/IMSProg_database_update.exe" \
		"$src/IMSProg_programmer/database/IMSProg.Dat" \
		"${qm[@]}"
}

do_sfpmaster() {
	local src=$WORKDIR/SFP-Master bld=$WORKDIR/build-sfp-master qm
	sync_repo "$src" "$SFPMASTER_URL" || die "could not clone $SFPMASTER_URL"
	build_app "$src" "$bld"
	mapfile -d '' qm < <(find "$bld" -name '*.qm' -print0)
	deploy "$WORKDIR/SFP-Master_win-amd64-shim" "$bld/SFP-Master.exe" "${qm[@]}"
}

# ---------------------------------------------------------------------------

install_packages
sync_shim
self_update "$@"
# Fetch the application sources before building: the shim's tests compile parts of them.
case "$ONLY" in ""|imsprog)    sync_repo "$WORKDIR/IMSProg" "$IMSPROG_URL" || die "could not clone $IMSPROG_URL" ;; esac
case "$ONLY" in ""|sfp-master) sync_repo "$WORKDIR/SFP-Master" "$SFPMASTER_URL" || die "could not clone $SFPMASTER_URL" ;; esac
build_shim
export SKIP_UPDATE=1    # already updated above
case "$ONLY" in
	"")         do_imsprog; do_sfpmaster ;;
	imsprog)    do_imsprog ;;
	sfp-master) do_sfpmaster ;;
	*)          die "ONLY must be imsprog or sfp-master" ;;
esac

log "Done"
for d in IMSProg_win-amd64-shim SFP-Master_win-amd64-shim; do
	[ -d "$WORKDIR/$d" ] && printf '  %s\n' "$(cygpath -w "$WORKDIR/$d")"
done
echo "Copy a folder anywhere and run the .exe inside; MSYS2 is not needed on that PC."
echo "The CH341A must use WCH's CH341PAR driver (Device Manager: Interface > USB-EPP/I2C... CH341A)."
