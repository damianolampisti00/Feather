#!/bin/bash
# Builds libjpeg-turbo's JPEG *decoder* (libjpeg API, NEON SIMD) for BB10
# armv7 with the BBNDK's qcc, into ../third_party/libjpeg-turbo/.
#
# Why: bb::utility::ImageConverter took ~60 ms per 720x630 frame on the Q10
# (feather_debug.log, 2026-10-06), capping the frame rate near 15 fps no
# matter what the server did. libjpeg-turbo's hand-written NEON IDCT/upsampling/
# colour conversion is several times faster on this Cortex-A9-class CPU.
#
# 2.0.x on purpose: its ARM32 SIMD is one plain GAS file (simd/arm/jsimd_neon.S);
# later releases moved to NEON intrinsics that the NDK's gcc 4.8 handles badly.
# No CMake: the two config headers it would generate are written here.
#
# Usage (Git Bash):  bash tools/build-libjpeg-turbo.sh
set -e
VER=2.0.6
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=$HERE/third_party/libjpeg-turbo
WORK=${TMPDIR:-/tmp}/ljt-build
export QNX_HOST=/c/bbndk/ndk/host_10_3_1_12/win32/x86
export QNX_TARGET=/c/bbndk/ndk/target_10_3_1_995/qnx6
export PATH=$QNX_HOST/usr/bin:$PATH

mkdir -p "$WORK" && cd "$WORK"
if [ ! -d libjpeg-turbo-$VER ]; then
    curl -sSL -o ljt.tar.gz https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/$VER.tar.gz
    tar xzf ljt.tar.gz
fi
SRC=$WORK/libjpeg-turbo-$VER
GEN=$WORK/gen
mkdir -p "$GEN" obj
rm -f obj/*.o

cat > "$GEN/jconfig.h" <<EOF
#define JPEG_LIB_VERSION  62
#define LIBJPEG_TURBO_VERSION  $VER
#define LIBJPEG_TURBO_VERSION_NUMBER  2000006
#define MEM_SRCDST_SUPPORTED 1
#define WITH_SIMD 1
#define BITS_IN_JSAMPLE  8
#define HAVE_LOCALE_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDLIB_H 1
#define HAVE_UNSIGNED_CHAR 1
#define HAVE_UNSIGNED_SHORT 1
EOF
cat > "$GEN/jconfigint.h" <<EOF
#define BUILD  "bb10"
#undef inline
#define INLINE  __inline__ __attribute__((always_inline))
#define THREAD_LOCAL
#define PACKAGE_NAME  "libjpeg-turbo"
#define VERSION  "$VER"
#define SIZEOF_SIZE_T  4
#define HAVE_BUILTIN_CTZL
EOF

# Decompression sources only (plus the shared ones), and the ARM SIMD glue.
SOURCES="jcomapi.c jdapimin.c jdapistd.c jdatasrc.c jdcoefct.c jdcolor.c jddctmgr.c
  jdhuff.c jdicc.c jdinput.c jdmainct.c jdmarker.c jdmaster.c jdmerge.c jdphuff.c
  jdpostct.c jdsample.c jdtrans.c jerror.c jidctflt.c jidctfst.c jidctint.c
  jidctred.c jquant1.c jquant2.c jutils.c jmemmgr.c jmemnobs.c simd/arm/jsimd.c"

# -mfpu=neon defines __ARM_NEON__, which is how simd/arm/jsimd.c turns NEON on
# (its runtime check reads /proc/cpuinfo, which QNX doesn't have). softfp is
# link-compatible with the rest of the app either way.
CFLAGS="-Vgcc_ntoarmv7le -O3 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -fPIC -I$GEN -I$SRC -DNO_GETENV"
for f in $SOURCES; do
    qcc $CFLAGS -c "$SRC/$f" -o "obj/$(basename "${f%.c}").o"
done
qcc $CFLAGS -c "$SRC/simd/arm/jsimd_neon.S" -o obj/jsimd_neon.o

mkdir -p "$OUT/lib/armv7" "$OUT/include"
rm -f "$OUT/lib/armv7/libjpeg-turbo.a"
ntoarmv7-ar rcs "$OUT/lib/armv7/libjpeg-turbo.a" obj/*.o
cp "$SRC/jpeglib.h" "$SRC/jmorecfg.h" "$SRC/jerror.h" "$GEN/jconfig.h" "$OUT/include/"
cp "$SRC/LICENSE.md" "$OUT/LICENSE.md"
echo "built $OUT/lib/armv7/libjpeg-turbo.a"
