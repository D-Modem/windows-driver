# Building

We build the driver with the toolchain of
[clang-retro](https://github.com/clang-retro), not with a Microsoft DDK.
One `dmodem.sys` operates on 2000, XP, 98 SE and ME.

```sh
make test                              # the tests of the shared code (cc, ASan/UBSan)
make driver CLANG_DOS=/opt/clang-dos   # dmodem.sys
make check  CLANG_DOS=/opt/clang-dos   # the imports of dmodem.sys: 2000, 98 SE, ME
make inf DMODEM=../D-Modem             # the modem INF files, from the INF of D-Modem
make inf-check DMODEM=../D-Modem       # make sure that these INF files agree with the INF of D-Modem
make engine DMODEM=../D-Modem          # dmodem-svc.exe (CMake), and its imports: 98, 2000
make engine-test DMODEM=../D-Modem     # the loopback line, on this machine
```

The engine needs a D-Modem checkout (`DMODEM`) that has
D-Modem/D-Modem#148 (libdmodem on 98 to XP).

CI (`.github/workflows/ci.yml`) does all of these steps with the
`setup-toolchain` action of clang-retro. It uploads the driver, the
engine and the INF files.
