{
  description = "Logos logoscore CLI - headless module runtime";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
    logos-liblogos.url = "github:logos-co/logos-liblogos";
    logos-module-client.url = "github:logos-co/logos-module-client";
    logos-capability-module.url = "github:logos-co/logos-capability-module";
    logos-package-manager.url = "github:logos-co/logos-package-manager";
    nix-bundle-lgx.url = "github:logos-co/nix-bundle-lgx";
    nix-bundle-dir.url = "github:logos-co/nix-bundle-dir";
    nix-bundle-appimage.url = "github:logos-co/nix-bundle-appimage";
  };

  outputs = { self, nixpkgs, logos-nix, logos-liblogos, logos-module-client, logos-capability-module, logos-package-manager, nix-bundle-lgx, nix-bundle-dir, nix-bundle-appimage }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
        liblogos = logos-liblogos.packages.${system}.logos-liblogos;
        liblogosLib = logos-liblogos.packages.${system}.logos-liblogos-lib;
        liblogosPortable = logos-liblogos.packages.${system}.portable;
        moduleClient = logos-module-client.packages.${system}.logos-module-client;
        moduleClientLib = logos-module-client.packages.${system}.logos-module-client-lib;
        capabilityModuleLib = logos-capability-module.packages.${system}.lib;
        lgpmCli = logos-package-manager.packages.${system}.cli;
        lgpmCliPortable = logos-package-manager.packages.${system}.cli-portable;
        bundleLgx = nix-bundle-lgx.bundlers.${system}.default;
        bundleLgxPortable = nix-bundle-lgx.bundlers.${system}.portable;
        dirBundler = nix-bundle-dir.bundlers.${system}.qtApp;
        appBundler = nix-bundle-appimage.lib.${system}.mkAppImage;
      });
    in
    {
      packages = forAllSystems ({ pkgs, system, liblogos, liblogosLib, liblogosPortable, moduleClient, moduleClientLib, capabilityModuleLib, lgpmCli, lgpmCliPortable, bundleLgx, bundleLgxPortable, dirBundler, appBundler }:
        let
          pname = "logos-logoscore-cli";
          version = "0.1.0";
          src = ./.;

          meta = with pkgs.lib; {
            description = "Logos logoscore headless module runtime CLI";
            platforms = platforms.unix;
          };

          # Bundle capability module as LGX, install with lgpm
          capabilityModuleLgx = bundleLgx capabilityModuleLib;
          modules = pkgs.runCommand "${pname}-modules-${version}"
            {
              inherit meta;
              nativeBuildInputs = [ lgpmCli ];
            }
            ''
              mkdir -p $out/modules

              for lgxFile in ${capabilityModuleLgx}/*.lgx; do
                echo "Installing $(basename "$lgxFile")..."
                lgpm --modules-dir "$out/modules" install --file "$lgxFile"
              done

              echo "Modules directory contents:"
              ls -laR $out/modules/
            '';

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
              pkgs.nlohmann_json
              pkgs.stduuid
              pkgs.cli11
              pkgs.gtest
            ];

            cmakeFlags = [
              "-GNinja"
              "-DLOGOS_LIBLOGOS_ROOT=${liblogos}"
              "-DLOGOS_MODULE_CLIENT_ROOT=${moduleClient}"
            ];
          };

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

              # Copy liblogos_module_client so logoscore can link at runtime
              if [ -d ${moduleClientLib}/lib ]; then
                cp -r ${moduleClientLib}/lib/* $out/lib/
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
              pkgs.nlohmann_json
              pkgs.stduuid
              pkgs.cli11
              pkgs.gtest
              liblogosLib
              moduleClientLib
            ];

            cmakeFlags = [
              "-GNinja"
              "-DLOGOS_LIBLOGOS_ROOT=${liblogos}"
              "-DLOGOS_MODULE_CLIENT_ROOT=${moduleClient}"
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

              if [ -d ${moduleClientLib}/lib ]; then
                cp -r ${moduleClientLib}/lib/* $out/lib/ || true
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

          # Portable modules: bundle capability module with portable lgx bundler + portable lgpm
          capabilityModuleLgxPortable = bundleLgxPortable capabilityModuleLib;
          modulesPortable = pkgs.runCommand "${pname}-modules-portable-${version}"
            {
              inherit meta;
              nativeBuildInputs = [ lgpmCliPortable ];
            }
            ''
              mkdir -p $out/modules

              for lgxFile in ${capabilityModuleLgxPortable}/*.lgx; do
                echo "Installing $(basename "$lgxFile")..."
                lgpm --modules-dir "$out/modules" install --file "$lgxFile"
              done

              echo "Modules directory contents:"
              ls -laR $out/modules/
            '';

          # Portable build: compile against portable liblogos
          buildPortable = pkgs.stdenv.mkDerivation {
            pname = "${pname}-portable";
            inherit version src meta;

            dontWrapQtApps = true;

            nativeBuildInputs = [
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
            ];

            buildInputs = [
              pkgs.qt6.qtbase
              pkgs.qt6.qtremoteobjects
              pkgs.nlohmann_json
              pkgs.gtest
              pkgs.stduuid
              pkgs.cli11
            ];

            cmakeFlags = [
              "-GNinja"
              "-DLOGOS_LIBLOGOS_ROOT=${liblogosPortable}"
              "-DLOGOS_MODULE_CLIENT_ROOT=${moduleClient}"
            ];
          };

          # Portable bin package — nix-bundle-dir handles library bundling and patching
          binPortable = pkgs.stdenvNoCC.mkDerivation {
            pname = "${pname}-bin-portable";
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

            passthru = { extraDirs = [ "modules" ]; };

            qtWrapperArgs = [
              "--unset LD_LIBRARY_PATH"
            ];

            installPhase = ''
              runHook preInstall

              mkdir -p $out/bin $out/lib $out/modules

              # Binaries from portable build
              cp -r ${buildPortable}/bin/* $out/bin/
              cp -L ${liblogosPortable}/bin/logos_host $out/bin/ 2>/dev/null || true

              # Libraries — nix-bundle-dir will resolve and bundle all dependencies
              cp -L ${liblogosPortable}/lib/*.dylib $out/lib/ 2>/dev/null || true
              cp -L ${liblogosPortable}/lib/*.so $out/lib/ 2>/dev/null || true
              cp -L ${moduleClientLib}/lib/*.dylib $out/lib/ 2>/dev/null || true
              cp -L ${moduleClientLib}/lib/*.so $out/lib/ 2>/dev/null || true

              # Portable modules
              cp -r ${modulesPortable}/modules/* $out/modules/ 2>/dev/null || true

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
          cli-bundle-dir = dirBundler binPortable;
          cli-appimage = appBundler {
            drv = binPortable;
            name = "logoscore";
            bundle = dirBundler binPortable;
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
            pkgs.nlohmann_json
            pkgs.stduuid
            pkgs.cli11
            pkgs.gtest
          ];
          shellHook = ''
            export LOGOS_LIBLOGOS_ROOT="${liblogos}"
          '';
        };
      });
    };
}
