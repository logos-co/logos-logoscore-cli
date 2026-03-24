{
  description = "Logos logoscore CLI - headless module runtime";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
    logos-liblogos.url = "github:logos-co/logos-liblogos";
    logos-capability-module.url = "github:logos-co/logos-capability-module";
    nix-bundle-dir.url = "github:logos-co/nix-bundle-dir";
    nix-bundle-appimage.url = "github:logos-co/nix-bundle-appimage";
  };

  outputs = { self, nixpkgs, logos-nix, logos-liblogos, logos-capability-module, nix-bundle-dir, nix-bundle-appimage }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
        liblogos = logos-liblogos.packages.${system}.logos-liblogos;
        liblogosLib = logos-liblogos.packages.${system}.logos-liblogos-lib;
        capabilityModule = logos-capability-module.packages.${system}.default;
        dirBundler = nix-bundle-dir.bundlers.${system}.qtApp;
        appBundler = nix-bundle-appimage.lib.${system}.mkAppImage;
      });
    in
    {
      packages = forAllSystems ({ pkgs, system, liblogos, liblogosLib, capabilityModule, dirBundler, appBundler }:
        let
          pname = "logos-logoscore-cli";
          version = "0.1.0";
          src = ./.;

          meta = with pkgs.lib; {
            description = "Logos logoscore headless module runtime CLI";
            platforms = platforms.unix;
          };

          # Build the logoscore binary against logos-liblogos
          build = pkgs.stdenv.mkDerivation {
            inherit pname version src meta;

            dontWrapQtApps = true;

            nativeBuildInputs = [
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
            ];

            buildInputs = [
              pkgs.qt6.qtbase
              pkgs.qt6.qtremoteobjects
              pkgs.gtest
            ];

            cmakeFlags = [
              "-GNinja"
              "-DLOGOS_LIBLOGOS_ROOT=${liblogos}"
            ];
          };

          # Bundle modules (capability_module) so logoscore can load it by default
          modules = pkgs.runCommand "${pname}-modules-${version}"
            { inherit meta; }
            ''
              mkdir -p $out/modules/capability_module

              if [ -d ${capabilityModule}/lib ]; then
                for lib in ${capabilityModule}/lib/*.dylib ${capabilityModule}/lib/*.so; do
                  if [ -f "$lib" ]; then
                    cp "$lib" $out/modules/capability_module/
                  fi
                done
              fi

              pluginFile=""
              for f in $out/modules/capability_module/*; do
                if [ -f "$f" ]; then
                  pluginFile="$(basename "$f")"
                  break
                fi
              done

              if [ -z "$pluginFile" ]; then
                echo "Error: No capability_module library found"
                exit 1
              fi

              platform=""
              arch=""
              case "$(uname -s)" in
                Linux)  platform="linux" ;;
                Darwin) platform="darwin" ;;
              esac
              case "$(uname -m)" in
                x86_64)        arch="x86_64" ;;
                aarch64|arm64) arch="aarch64" ;;
              esac

              # Dev build: use -dev suffix variant keys
              cat > $out/modules/capability_module/manifest.json <<EOF
              {
                "name": "capability_module",
                "version": "1.0.0",
                "main": {
                  "$platform-$arch-dev": "$pluginFile",
                  "$platform-amd64-dev": "$pluginFile",
                  "$platform-arm64-dev": "$pluginFile",
                  "$platform-x86_64-dev": "$pluginFile",
                  "$platform-aarch64-dev": "$pluginFile"
                }
              }
              EOF
            '';

          # Package the logoscore binary with its runtime deps
          bin = pkgs.stdenvNoCC.mkDerivation {
            pname = "${pname}-bin";
            inherit version meta;

            dontUnpack = true;

            nativeBuildInputs =
              [ pkgs.qt6.wrapQtAppsNoGuiHook ]
              ++ pkgs.lib.optionals pkgs.stdenv.isDarwin [ pkgs.darwin.cctools ]
              ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.autoPatchelfHook ];

            buildInputs = [
              pkgs.qt6.qtbase
              pkgs.qt6.qtremoteobjects
            ];

            qtWrapperArgs = [
              "--unset LD_LIBRARY_PATH"
              "--set LOGOS_BUNDLED_MODULES_DIR ${modules}/modules"
              "--set LOGOS_HOST_PATH ${liblogos}/bin/logos_host"
            ];

            installPhase = ''
              runHook preInstall

              mkdir -p $out/bin $out/lib $out/modules

              cp -r ${build}/bin/* $out/bin/
              chmod -R +w $out/bin

              # Copy liblogos_core so logoscore can link at runtime
              if [ -d ${liblogosLib}/lib ]; then
                cp -r ${liblogosLib}/lib/* $out/lib/
                chmod -R +w $out/lib
              fi

              if [ -d ${modules}/modules ]; then
                cp -r ${modules}/modules/* $out/modules/
              fi

              ${pkgs.lib.optionalString pkgs.stdenv.isDarwin ''
                for binary in $out/bin/*; do
                  if [ -f "$binary" ] && [ -x "$binary" ]; then
                    for dylib in $out/lib/*.dylib; do
                      if [ -f "$dylib" ]; then
                        libname=$(basename $dylib)
                        install_name_tool -change "@rpath/$libname" "$out/lib/$libname" "$binary" 2>/dev/null || true
                      fi
                    done
                  fi
                done
              ''}

              runHook postInstall
            '';
          };

          # Tests derivation: builds cli_tests + logoscore binary for integration tests
          tests = pkgs.stdenv.mkDerivation {
            pname = "${pname}-tests";
            inherit version src meta;

            dontWrapQtApps = true;

            nativeBuildInputs = [
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
              pkgs.qt6.wrapQtAppsNoGuiHook
            ] ++ pkgs.lib.optionals pkgs.stdenv.isDarwin [ pkgs.darwin.cctools ]
              ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.autoPatchelfHook ];

            buildInputs = [
              pkgs.qt6.qtbase
              pkgs.qt6.qtremoteobjects
              pkgs.gtest
              liblogosLib
            ];

            cmakeFlags = [
              "-GNinja"
              "-DLOGOS_LIBLOGOS_ROOT=${liblogos}"
            ];

            installPhase = ''
              runHook preInstall

              mkdir -p $out/bin $out/lib

              cp bin/cli_tests $out/bin/
              cp bin/unit_tests $out/bin/
              cp bin/logoscore $out/bin/

              if [ -d ${liblogosLib}/lib ]; then
                cp -r ${liblogosLib}/lib/* $out/lib/ || true
              fi

              ${pkgs.lib.optionalString pkgs.stdenv.isDarwin ''
                for binary in $out/bin/cli_tests $out/bin/unit_tests $out/bin/logoscore; do
                  for dylib in $out/lib/*.dylib; do
                    if [ -f "$dylib" ]; then
                      libname=$(basename $dylib)
                      install_name_tool -change "@rpath/$libname" "$out/lib/$libname" "$binary" 2>/dev/null || true
                    fi
                  done
                done
              ''}

              runHook postInstall
            '';
          };

          logoscoreCli = pkgs.symlinkJoin {
            name = pname;
            paths = [ bin ];
          };
        in
        {
          cli = logoscoreCli;
          tests = tests;
          cli-bundle-dir = dirBundler bin;
          cli-appimage = appBundler {
            drv = bin;
            name = "logoscore";
            bundle = dirBundler bin;
            desktopFile = ./assets/logoscore.desktop;
            icon = ./assets/logoscore.png;
          };
          default = logoscoreCli;
        }
      );

      checks = forAllSystems ({ pkgs, system, ... }:
        let
          testsPkg = self.packages.${system}.tests;
        in {
          tests = pkgs.runCommand "logos-logoscore-cli-tests" {
            nativeBuildInputs = [ testsPkg ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.qt6.qtbase ];
          } ''
            export QT_QPA_PLATFORM=offscreen
            export QT_FORCE_STDERR_LOGGING=1
            ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
              export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
            ''}
            export LOGOSCORE_BINARY=${testsPkg}/bin/logoscore
            mkdir -p $out
            echo "Running logos-logoscore-cli unit tests..."
            ${testsPkg}/bin/unit_tests --gtest_output=xml:$out/unit-test-results.xml
            echo "Running logos-logoscore-cli CLI tests..."
            ${testsPkg}/bin/cli_tests --gtest_output=xml:$out/cli-test-results.xml
          '';
        }
      );

      devShells = forAllSystems ({ pkgs, liblogos, ... }: {
        default = pkgs.mkShell {
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
          ];
          buildInputs = [
            pkgs.qt6.qtbase
            pkgs.qt6.qtremoteobjects
            pkgs.gtest
          ];
          shellHook = ''
            export LOGOS_LIBLOGOS_ROOT="${liblogos}"
          '';
        };
      });
    };
}
