{
  description = "Logos logoscore CLI - headless module runtime";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
    # Direct SDK input so the CLI binary can link logos_sdk and use its
    # public symbols (e.g. logos::transportSetToJsonString) without
    # relying on the symbol surviving liblogos_core's link-time
    # dead-strip. liblogos's own SDK pin still drives transitive deps.
    logos-cpp-sdk.url = "github:logos-co/logos-cpp-sdk";
    logos-cpp-sdk.inputs.logos-protocol.follows = "logos-protocol";
    logos-protocol.url = "github:logos-co/logos-protocol";
    logos-qt-sdk.url = "github:logos-co/logos-qt-sdk";
    logos-qt-sdk.inputs.logos-protocol.follows = "logos-protocol";
    logos-qt-sdk.inputs.logos-cpp-sdk.follows = "logos-cpp-sdk";
    logos-liblogos.url = "github:logos-co/logos-liblogos";
    logos-capability-module.url = "github:logos-co/logos-capability-module";
    # Real test-module plugins (test_basic_module) used by the
    # daemon-backed integration tests in tests/test_integration.cpp.
    logos-test-modules.url = "github:logos-co/logos-test-modules";
    nix-bundle-logos-module-install.url = "github:logos-co/nix-bundle-logos-module-install";
    nix-bundle-dir.url = "github:logos-co/nix-bundle-dir";
    nix-bundle-appimage.url = "github:logos-co/nix-bundle-appimage";
  };

  outputs = { self, nixpkgs, logos-nix, logos-cpp-sdk, logos-protocol, logos-qt-sdk, logos-liblogos, logos-capability-module, logos-test-modules, nix-bundle-logos-module-install, nix-bundle-dir, nix-bundle-appimage }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      # Build info baked into the logoscore binary so `--version` reports the
      # release version, this repo's commit, and the locked commits of the SDK
      # stack. `revOf` yields the input's locked rev, a "<sha>-dirty" marker for
      # a dirty checkout, or "dirty" for a path override.
      revOf = input: input.rev or input.dirtyRev or "dirty";
      buildInfo = {
        # VERSION is only present on release branches. On master (pre-release CI
        # builds) there is no VERSION file, so fall back to a "pre-release-{sha7}"
        # string derived from self.rev. Dirty local builds lack self.rev and get
        # an empty string, which the CLI renders as "dev".
        version = if builtins.pathExists ./VERSION
          then nixpkgs.lib.removeSuffix "\n" (builtins.readFile ./VERSION)
          else if (self ? rev) then "pre-release-${builtins.substring 0 7 self.rev}" else "";
        commit = revOf self;
        commits = [
          { name = "logos-liblogos"; commit = revOf logos-liblogos; }
          { name = "logos-cpp-sdk"; commit = revOf logos-cpp-sdk; }
          { name = "logos-protocol"; commit = revOf logos-protocol; }
          { name = "logos-qt-sdk"; commit = revOf logos-qt-sdk; }
          { name = "logos-capability-module"; commit = revOf logos-capability-module; }
        ];
      };
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
        cppSdk = logos-cpp-sdk.packages.${system}.default;
        protocolPkg = logos-protocol.packages.${system}.default;
        qtSdk = logos-qt-sdk.packages.${system}.default;
        liblogos = logos-liblogos.packages.${system}.logos-liblogos;
        liblogosLib = logos-liblogos.packages.${system}.logos-liblogos-lib;
        liblogosPortable = logos-liblogos.packages.${system}.portable;
        capabilityModuleLib = logos-capability-module.packages.${system}.lib;
        installDev = nix-bundle-logos-module-install.bundlers.${system}.dev;
        installPortable = nix-bundle-logos-module-install.bundlers.${system}.portable;
        dirBundler = nix-bundle-dir.bundlers.${system}.qtApp;
        appBundler = nix-bundle-appimage.lib.${system}.mkAppImage;
      });
    in
    {
      packages = forAllSystems ({ pkgs, system, cppSdk, protocolPkg, qtSdk, liblogos, liblogosLib, liblogosPortable, capabilityModuleLib, installDev, installPortable, dirBundler, appBundler }:
        let
          pname = "logos-logoscore-cli";
          # VERSION is only present on release branches; dev branches use a placeholder.
          version = if builtins.pathExists ./VERSION
            then nixpkgs.lib.removeSuffix "\n" (builtins.readFile ./VERSION)
            else "0.1.0-dev";
          src = ./.;

          # Generated header (version + commit hashes) staged into src/ at build
          # time so main.cpp can bake it into the binary for `--version`.
          buildInfoHeader = import ./nix/build-info.nix { inherit pkgs buildInfo; };

          meta = with pkgs.lib; {
            description = "Logos logoscore headless module runtime CLI";
            platforms = platforms.unix;
          };

          # Install capability module (bundle + lgpm in one step)
          capabilityInstall = installDev capabilityModuleLib;
          modules = pkgs.runCommand "${pname}-modules-${version}"
            { inherit meta; }
            ''
              mkdir -p $out/modules

              if [ -d "${capabilityInstall}/modules" ]; then
                cp -r ${capabilityInstall}/modules/. $out/modules/
              fi

              echo "Modules directory contents:"
              ls -laR $out/modules/
            '';

          # Build the logoscore binary against logos-liblogos
          build = pkgs.stdenv.mkDerivation {
            inherit pname version src meta;

            dontWrapQtApps = true;

            # Stage the generated build-info header next to src/version_info.h.
            preConfigure = ''
              cp ${buildInfoHeader} src/logos_build_info.h
              chmod +w src/logos_build_info.h
            '';

            nativeBuildInputs = [
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
              pkgs.qt6.wrapQtAppsNoGuiHook
            ];

            buildInputs = [
              # cppSdk propagates Boost, OpenSSL, and nlohmann_json
              # through `propagatedBuildInputs` on its symlinkJoin, so
              # we don't list them explicitly here. Qt is intentionally
              # NOT propagated by the SDK (qtbase's setup-hook fires
              # `qtPreHook` which errors unless `wrapQtAppsHook` was
              # sourced first, and that ordering can't be guaranteed
              # through propagation), so we list it explicitly. CMake's
              # `find_package(logos-cpp-sdk)` then re-runs
              # `find_dependency(...)` against the propagated non-Qt
              # entries + the Qt entries at configure time and stitches
              # them into the imported target.
              pkgs.qt6.qtbase
              pkgs.qt6.qtremoteobjects
              cppSdk
              protocolPkg
              qtSdk
              pkgs.stduuid
              pkgs.cli11
              pkgs.gtest
              pkgs.fmt
            ];

            cmakeFlags = [
              "-GNinja"
              "-DLOGOS_LIBLOGOS_ROOT=${liblogos}"
              # Direct path to the SDK: CMake's find_package(logos-cpp-sdk)
              # picks up the imported target so logoscore can link
              # logos_sdk explicitly (needed for symbols like
              # logos::transportSetToJsonString which liblogos doesn't
              # itself reference and would otherwise be dead-stripped).
              "-DLOGOS_CPP_SDK_ROOT=${cppSdk}"
              "-DLOGOS_PROTOCOL_ROOT=${protocolPkg}"
              "-DLOGOS_QT_SDK_ROOT=${qtSdk}"
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
              pkgs.fmt
              liblogosLib
              # cppSdk propagates Boost, OpenSSL, nlohmann_json (but
              # not Qt) via its symlinkJoin's propagatedBuildInputs —
              # see the `build` derivation above for the rationale.
              cppSdk
              protocolPkg
              qtSdk
            ];

            cmakeFlags = [
              "-GNinja"
              "-DLOGOS_LIBLOGOS_ROOT=${liblogos}"
              "-DLOGOS_CPP_SDK_ROOT=${cppSdk}"
              "-DLOGOS_PROTOCOL_ROOT=${protocolPkg}"
              "-DLOGOS_QT_SDK_ROOT=${qtSdk}"
            ];

            installPhase = ''
              runHook preInstall

              mkdir -p $out/bin $out/lib

              cp bin/cli_tests $out/bin/
              cp bin/unit_tests $out/bin/
              cp bin/integration_tests $out/bin/
              cp bin/logoscore $out/bin/

              if [ -d ${liblogosLib}/lib ]; then
                cp -r ${liblogosLib}/lib/* $out/lib/ || true
              fi

              ${pkgs.lib.optionalString pkgs.stdenv.isDarwin ''
                for binary in $out/bin/cli_tests $out/bin/unit_tests $out/bin/integration_tests $out/bin/logoscore; do
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

          # Portable modules: bundle + install capability module (portable variant)
          capabilityInstallPortable = installPortable capabilityModuleLib;
          modulesPortable = pkgs.runCommand "${pname}-modules-portable-${version}"
            { inherit meta; }
            ''
              mkdir -p $out/modules

              if [ -d "${capabilityInstallPortable}/modules" ]; then
                cp -r ${capabilityInstallPortable}/modules/. $out/modules/
              fi

              echo "Modules directory contents:"
              ls -laR $out/modules/
            '';

          # Portable build: compile against portable liblogos
          buildPortable = pkgs.stdenv.mkDerivation {
            pname = "${pname}-portable";
            inherit version src meta;

            dontWrapQtApps = true;

            # Stage the generated build-info header next to src/version_info.h.
            preConfigure = ''
              cp ${buildInfoHeader} src/logos_build_info.h
              chmod +w src/logos_build_info.h
            '';

            nativeBuildInputs = [
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
              pkgs.qt6.wrapQtAppsNoGuiHook
            ];

            buildInputs = [
              # cppSdk propagates Boost, OpenSSL, nlohmann_json (but
              # not Qt) via its symlinkJoin's propagatedBuildInputs —
              # see the `build` derivation above for the rationale.
              pkgs.qt6.qtbase
              pkgs.qt6.qtremoteobjects
              cppSdk
              protocolPkg
              qtSdk
              pkgs.gtest
              pkgs.stduuid
              pkgs.cli11
              pkgs.fmt
            ];

            cmakeFlags = [
              "-GNinja"
              "-DLOGOS_LIBLOGOS_ROOT=${liblogosPortable}"
              "-DLOGOS_CPP_SDK_ROOT=${cppSdk}"
              "-DLOGOS_PROTOCOL_ROOT=${protocolPkg}"
              "-DLOGOS_QT_SDK_ROOT=${qtSdk}"
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

      checks = forAllSystems ({ pkgs, system, liblogos, capabilityModuleLib, installDev, ... }:
        let
          testsPkg = self.packages.${system}.tests;
          # Modules the integration daemon scans. The daemon auto-loads
          # capability_module at boot for auth — without it every
          # load-module/call blocks ~20s on capability negotiation then
          # fails (status/list-modules don't need it, so they'd still
          # pass, masking the problem). The `tests` package ships only
          # the binary, so bundle the built-in capability_module next to
          # the real test_basic_module plugin here. `.install` /
          # installDev both expose a top-level `modules/` tree;
          # symlinkJoin (lndir) merges them into one scan dir.
          testModulesInstall = pkgs.symlinkJoin {
            name = "logos-logoscore-cli-it-modules";
            paths = [
              (installDev capabilityModuleLib)
              logos-test-modules.modules.${system}.test_basic_module.install
            ];
          };
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
            # Daemon-backed integration tests (tests/test_integration.cpp)
            # read these. Absent ⇒ those tests GTEST_SKIP, so the rest of
            # the suite still runs in environments without test modules.
            export LOGOSCORE_TEST_MODULES_DIR=${testModulesInstall}/modules
            export LOGOS_HOST_PATH=${liblogos}/bin/logos_host
            mkdir -p $out
            echo "Running logos-logoscore-cli unit tests..."
            ${testsPkg}/bin/unit_tests --gtest_output=xml:$out/unit-test-results.xml
            echo "Running logos-logoscore-cli CLI tests..."
            ${testsPkg}/bin/cli_tests --gtest_output=xml:$out/cli-test-results.xml
            echo "Running logos-logoscore-cli integration tests..."
            ${testsPkg}/bin/integration_tests --gtest_output=xml:$out/integration-test-results.xml
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
            pkgs.fmt
          ];
          shellHook = ''
            export LOGOS_LIBLOGOS_ROOT="${liblogos}"
          '';
        };
      });
    };
}
