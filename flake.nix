{
  description = "SocketCAN C++ library dev environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.05";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
      in {
        ########################################################################
        # Dev shell: nix develop
        ########################################################################
        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            gcc
            cmake
            gdb
            pkg-config
            gtest
            linuxHeaders
          ];

          CXX = "g++";

          shellHook = ''
            export PS1='\[\e[36m\][nix-socketcan]\[\e[0m\] '"$PS1"
          '';
        };

        ########################################################################
        # Package: nix build .#default  (build library only)
        ########################################################################
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "socketcan";
          version = "0.1.0";

          src = self;

          nativeBuildInputs = [
            pkgs.cmake
            pkgs.gcc
            pkgs.pkg-config
          ];

          buildInputs = [
            pkgs.linuxHeaders
          ];

          configurePhase = ''
            cmake -S . -B build \
              -DCMAKE_BUILD_TYPE=Release \
              -DCMAKE_INSTALL_PREFIX=$out \
              -DBUILD_TESTING=OFF
          '';

          buildPhase = ''
            cmake --build build --config Release
          '';

          installPhase = ''
            cmake --install build
          '';
        };

        ########################################################################
        # Package: nix build .#tests  (build tests only)
        ########################################################################
        packages.tests = pkgs.stdenv.mkDerivation {
          pname = "socketcan-tests";
          version = "0.1.0";

          src = self;

          # IMPORTANT: avoid RPATH sanitization so test binary is allowed
          dontFixup = true;
          dontPatchELF = true;

          nativeBuildInputs = [
            pkgs.cmake
            pkgs.gcc
            pkgs.pkg-config
          ];

          buildInputs = [
            pkgs.linuxHeaders
            pkgs.gtest
          ];

          configurePhase = ''
            cmake -S . -B build \
              -DCMAKE_BUILD_TYPE=Debug \
              -DBUILD_TESTING=ON
          '';

          buildPhase = ''
            cmake --build build --config Debug
          '';

          installPhase = ''
            mkdir -p $out
            cp build/tests/socketcan_tests $out/
          '';
        };
      });
}
