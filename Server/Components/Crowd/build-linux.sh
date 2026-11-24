#!/usr/bin/env bash
# The Crowd component for Linux open.mp servers, built in Debian containers (glibc 2.36):
#   Crowd-linux-x86.so    32-bit x86, for the official linux-x86 server
#   Crowd-linux-arm64.so  64-bit ARM (Raspberry Pi and other arm64 servers)
# Drop the one for your server into its components/ as Crowd.so. Run from the repo root:
#   Server/Components/Crowd/build-linux.sh [out-dir]
set -euo pipefail
OUT="${1:-build-linux-crowd}"
mkdir -p "$OUT"
build() {
	local platform="$1" name="$2"
	docker run --rm --platform "$platform" -v "$PWD:/src" -w /src debian:bookworm bash -euo pipefail -c "
		apt-get update -qq && apt-get install -y -qq --no-install-recommends g++ file >/dev/null
		g++ -std=c++17 -O2 -w -fPIC -shared -static-libstdc++ -static-libgcc \
			-DNDEBUG -DPAWN_CELL_SIZE=32 -DHAVE_STDINT_H=1 -DLINUX \
			-Dnssv_CONFIG_SELECT_STRING_VIEW=nssv_STRING_VIEW_NONSTD -Dspan_CONFIG_SELECT_SPAN=span_SPAN_NONSTD -DGLM_FORCE_QUAT_DATA_WXYZ \
			-ISDK/include -ISDK/lib/glm -ISDK/lib/span-lite/include -ISDK/lib/string-view-lite/include -ISDK/lib/robin-hood-hashing/src/include \
			-Ilib -Ilib/pawn/source -Ilib/pawn/source/amx -Ilib/pawn/source/linux \
			Server/Components/Crowd/crowd.cpp -o '$OUT/$name'
		file '$OUT/$name'
	"
}
build linux/386 Crowd-linux-x86.so
build linux/arm64 Crowd-linux-arm64.so
