# mod-embedded-auth

The logon server inside the worldserver process. Used by the WebAssembly build of this project (one module, one set of databases); harmless otherwise (`EmbeddedAuth.Enable = 0`).
`AuthSession.cpp` and `AuthCodes.cpp` here only `#include` the authserver's own sources.

Build: link or copy this directory to `azerothcore/modules/mod-embedded-auth` and configure with `-DMODULES=static`.
