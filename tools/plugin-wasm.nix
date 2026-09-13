{ csoundSource, csoundRev, pkgs ? import <nixpkgs> {} }:
let
  libc = pkgs.pkgsCross.wasi32.libc.overrideAttrs (old: {
    NIX_CFLAGS_COMPILE = (old.NIX_CFLAGS_COMPILE or "") + " -fPIC";
    # Upstream's symbol check describes a static build. Account for the PIC
    # macros and relocation bases, while retaining the rest of that check.
    postPatch = old.postPatch + ''
      for directory in expected/wasm32-wasip1; do
        sed -i '/^#define __POINTER_WIDTH__/i #define __PIC__ 2' "$directory/predefined-macros.txt"
        sed -i '/^#define __restrict /i #define __pic__ 2' "$directory/predefined-macros.txt"
        printf '__table_base\n' >> "$directory/undefined-symbols.txt"
        sort -u -o "$directory/undefined-symbols.txt" "$directory/undefined-symbols.txt"
      done
    '';
  });
  csound = import (csoundSource + "/wasm/src/csound.nix") {
    inherit pkgs;
    moduleKind = "browser";
    gitHash = csoundRev;
  };
in pkgs.pkgsCross.wasi32.clangStdenv.mkDerivation {
  name = "naviergrain-wasm";
  src = pkgs.lib.fileset.toSource {
    root = ../.;
    fileset = pkgs.lib.fileset.unions [ ../src ../include ];
  };
  dontConfigure = true;
  dontStrip = true;
  buildPhase = ''
    for source in field particles resampler core transport provider browser pack opcode; do
      $CC -O2 -fPIC -mllvm -wasm-enable-sjlj -DBUILD_PLUGINS -DUSE_DOUBLE=1 \
        -I${csound}/include/csound -Iinclude -Isrc \
        -c src/naviergrain_$source.c -o $source.o
    done
    $CC -shared -nostdlib -nostartfiles -Wl,--experimental-pic -Wl,--no-entry \
      -Wl,--import-table -Wl,--import-memory \
      -Wl,--export=__wasm_call_ctors -Wl,--export=csound_opcode_init \
      -Wl,--export=csoundModuleInfo *.o \
      ${libc}/lib/libc.a -o naviergrain.wasm
  '';
  installPhase = ''
    mkdir -p $out/lib $out/licenses
    cp naviergrain.wasm $out/lib/
    cp ${libc.src}/LICENSE* $out/licenses/
    cp ${libc.src}/libc-top-half/musl/COPYRIGHT $out/licenses/musl-COPYRIGHT
    cp ${csoundSource}/COPYING $out/licenses/Csound-LGPL-2.1.txt
    ln -s ${csound} $out/csound
    $CC --version > $out/compiler.txt
  '';
}
