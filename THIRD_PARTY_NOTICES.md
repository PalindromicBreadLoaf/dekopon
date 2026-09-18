# Third-party notices

Dekopon itself is GPL-2.0-or-later (`license.txt`). Bundled dependencies keep their own
licences. This file records additional extensions that change the resulting license of 
the binary.

## lsfg-vk: GPL-3.0-or-later

`externals/lsfg-vk/` is a vendored subset of [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk).

Its licence is GPL-3.0-or-later, thus when building a new binary, the result is only distributable
as **GPL-3.0-or-later**. Any `dekopon.nro` built with `ENABLE_LSFG=ON` must therefore be GPL-3.0-or-later.
Build with `-DENABLE_LSFG=OFF` for a GPL-2.0-or-later binary.
