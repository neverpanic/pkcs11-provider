## Build Prerequisites

This package requires the following:
- OpenSSL 3.0.7+ libraries and development headers
- OpenSSL tools (for testing)
- NSS softoken, tools and development headers (for testing)
- a C compiler that supports at least C11 semantics
- meson
- pkg-config
- p11-kit, p11-kit-server, p11-kit-devel, opensc and softhsm (for testing)

The usual command to build are:
- meson setup _build
- meson compile -C _build
- meson test -C _build

