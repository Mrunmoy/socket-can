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
            gtest       # For tests later
            linuxHeaders
          ];

          CXX = "g++";

          shellHook = ''
            export PS1='\[\e[36m\][nix-socketcan]\[\e[0m\] '"$PS1"
          '';
        };

        ########################################################################
        # Package: nix build .#default  (will call CMake)
        ########################################################################
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "socketcan";
          version = "0.1.0";

          # Use this git/dir as the source
          src = self;

          nativeBuildInputs = [
            pkgs.cmake
            pkgs.gcc
            pkgs.pkg-config
          ];

          buildInputs = [
            pkgs.linuxHeaders
          ];

          # CMake configure
          configurePhase = ''
            cmake -S . -B build \
              -DCMAKE_BUILD_TYPE=Release \
              -DCMAKE_INSTALL_PREFIX=$out
          '';

          # CMake build
          buildPhase = ''
            cmake --build build --config Release
          '';

          # CMake install (expects install() rules in CMakeLists.txt)
          installPhase = ''
            cmake --install build
          '';
        };
      });
}
