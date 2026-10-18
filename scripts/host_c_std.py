"""Host-toolchain fixes for native (simulator) builds.

GCC 15 defaults C to C23, where `bool`/`true`/`false` are keywords; the
QRCode library's C header still typedefs them and no longer compiles. C++
sources keep the -std set in build_flags.

Linux builds hash with OpenSSL (see ClippingsUpload.cpp), which needs an
explicit -lcrypto; macOS uses CommonCrypto, which links automatically.
"""

import sys

Import("env")

env.Append(CFLAGS=["-std=gnu17"])

if not sys.platform.startswith("darwin"):
    env.Append(LIBS=["crypto"])
