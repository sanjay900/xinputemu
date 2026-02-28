{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.11";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      packages = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          default = pkgs.stdenv.mkDerivation {
            pname = "xinputemu";
            version = "0.42";
            src = ./.;

            # Inject both 32-bit and 64-bit MinGW toolchains into the environment PATH.
            nativeBuildInputs = [
              pkgs.pkgsCross.mingw32.stdenv.cc
              pkgs.pkgsCross.mingwW64.stdenv.cc
            ];

            # Override the Makefile's hardcoded `/usr/...` paths via makeFlags.
            # Nix's cc wrappers automatically handle the inclusion of Windows SDK 
            # headers (like wbemidl.h) and libraries, so we only need to preserve 
            # local includes.
            makeFlags = [
              "GCC32=i686-w64-mingw32-gcc"
              "GCC64=x86_64-w64-mingw32-gcc"
              "INCLUDE_DIR32=-Idumbxinputemu"
              "INCLUDE_DIR64=-Idumbxinputemu"
              "LIB_DIR32="
              "LIB_DIR64="
            ];

            installPhase = ''
              runHook preInstall

              mkdir -p $out
              cp -r build/32 $out/
              cp -r build/64 $out/
              
              if [ -f setup_dumbxinputemu.verb ]; then
                cp setup_dumbxinputemu.verb $out/
              fi

              runHook postInstall
            '';
          };
        }
      );
    };
}
