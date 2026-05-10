# f3

A tiny C CLI/TUI for FFF.

```sh
make
./f3
./f3 query
./f3 -g query
./f3g query
```

By default the build uses the bundled FFF checkout at `vendor/fff` and copies `libfff_c` next to `f3`. Override it with:

```sh
make FFF_DIR=/path/to/fff
```
