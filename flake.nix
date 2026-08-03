{
  description = "Logos logosctl CLI - headless module runtime";

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
    # liblogos is linked INTO this CLI, so its logos-protocol is the one the
    # crashing code path actually runs. Without this follows it brought its own,
    # older protocol while cpp-sdk and qt-sdk followed the root pin — three
    # protocols in one process image, and bumping the root pin alone fixed
    # nothing. Measured: 4 SIGSEGVs in 2000 client calls with the root pin
    # already on the fixed protocol.
    logos-liblogos.inputs.logos-protocol.follows = "logos-protocol";
    logos-capability-module.url = "github:logos-co/logos-capability-module";
    # Bundled alongside capability_module so the CLI can manage packages
    # itself: package_manager installs/uninstalls and owns the dependency
    # graph, package_downloader owns the catalogs and downloads. Same pair
    # Basecamp bundles (see logos-basecamp/flake.nix), so both frontends
    # drive the identical module API.
    logos-package-manager-module.url = "github:logos-co/logos-package-manager-module";
    logos-package-downloader-module.url = "github:logos-co/logos-package-downloader-module";
    # Real test-module plugins (test_basic_module) used by the
    # daemon-backed integration tests in tests/test_integration.cpp.
    logos-test-modules.url = "github:logos-co/logos-test-modules";
    nix-bundle-logos-module-install.url = "github:logos-co/nix-bundle-logos-module-install";
    nix-bundle-dir.url = "github:logos-co/nix-bundle-dir";
    nix-bundle-appimage.url = "github:logos-co/nix-bundle-appimage";
  };

  outputs = { self, nixpkgs, logos-nix, logos-cpp-sdk, logos-protocol, logos-qt-sdk, logos-liblogos, logos-capability-module, logos-package-manager-module, logos-package-downloader-module, logos-test-modules, nix-bundle-logos-module-install, nix-bundle-dir, nix-bundle-appimage }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      # Build info baked into the logosctl binary so `--version` reports the
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
          { name = "logos-package-manager-module"; commit = revOf logos-package-manager-module; }
          { name = "logos-package-downloader-module"; commit = revOf logos-package-downloader-module; }
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
        packageManagerModuleLib = logos-package-manager-module.packages.${system}.lib;
        packageManagerModuleLibPortable = logos-package-manager-module.packages.${system}.lib-portable;
        packageDownloaderModuleLib = logos-package-downloader-module.packages.${system}.lib;
        installDev = nix-bundle-logos-module-install.bundlers.${system}.dev;
        installPortable = nix-bundle-logos-module-install.bundlers.${system}.portable;
        # Both binaries here are headless: they link Qt for its object system
        # and IPC, never for a GUI. qtCliApp bundles identically to qtApp but
        # leaves bin/ as plain binaries -- no environment launcher, no hidden
        # companion ELF -- because with no platform plugin there is nothing
        # that needs XKB_CONFIG_ROOT or the portal theme.
        dirBundler = nix-bundle-dir.bundlers.${system}.qtCliApp;
        appBundler = nix-bundle-appimage.lib.${system}.mkAppImage;
      });
    in
    {
      packages = forAllSystems ({ pkgs, system, cppSdk, protocolPkg, qtSdk, liblogos, liblogosLib, liblogosPortable, capabilityModuleLib, packageManagerModuleLib, packageManagerModuleLibPortable, packageDownloaderModuleLib, installDev, installPortable, dirBundler, appBundler }:
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
            description = "Logos logosctl headless module runtime CLI";
            platforms = platforms.unix;
          };

          # Bundled modules (bundle + lgpm install in one step each).
          #
          # capability_module    — the auth handshake every client needs.
          # package_manager      — install/uninstall + the dependency graph.
          # package_downloader   — catalogs, resolution, downloads.
          #
          # These land in $out/modules and are picked up at runtime as the
          # read-only "embedded" directory (paths::bundledModulesDir(),
          # <bin>/../modules). Mirrors logos-basecamp/flake.nix so the CLI
          # and the GUI drive the same module surface.
          bundledInstallsDev = map installDev [ capabilityModuleLib ];
          # Kept out of modules/ deliberately: logoscore scans that directory
          # and its doc-tests assert the exact module list, so anything extra
          # there would change what it reports.
          pkgInstallsDev = map installDev [
            packageManagerModuleLib
            packageDownloaderModuleLib
          ];
          modules = pkgs.runCommand "${pname}-modules-${version}"
            { inherit meta; }
            ''
              mkdir -p $out/modules

              mkdir -p $out/modules-pkg

              for installed in ${pkgs.lib.escapeShellArgs bundledInstallsDev}; do
                if [ -d "$installed/modules" ]; then
                  cp -r "$installed"/modules/. $out/modules/
                fi
              done

              for installed in ${pkgs.lib.escapeShellArgs pkgInstallsDev}; do
                if [ -d "$installed/modules" ]; then
                  cp -r "$installed"/modules/. $out/modules-pkg/
                fi
              done

              echo "Modules directory contents:"
              ls -laR $out/modules/
            '';

          # Build the logosctl binary against logos-liblogos
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
              pkgs.yaml-cpp
              pkgs.spdlog
            ];

            cmakeFlags = [
              "-GNinja"
              "-DLOGOS_LIBLOGOS_ROOT=${liblogos}"
              # Direct path to the SDK: CMake's find_package(logos-cpp-sdk)
              # picks up the imported target so logosctl can link
              # logos_sdk explicitly (needed for symbols like
              # logos::transportSetToJsonString which liblogos doesn't
              # itself reference and would otherwise be dead-stripped).
              "-DLOGOS_CPP_SDK_ROOT=${cppSdk}"
              "-DLOGOS_PROTOCOL_ROOT=${protocolPkg}"
              "-DLOGOS_QT_SDK_ROOT=${qtSdk}"
            ];
          };

          # Package the logosctl binary with its runtime deps
          # One package per binary. `logoscore` and `logosctl` compile together
          # (they share everything but main.cpp) but ship separately, so
          # `.#cli` still means exactly what it means today and nobody picks up
          # the new tool by accident.
          #
          # withPkgModules: only logosctl scans modules-pkg/, so only its
          # package carries it.
          mkBin = { binName, withPkgModules }: pkgs.stdenvNoCC.mkDerivation {
            pname = "${pname}-${binName}";
            inherit version meta;

            dontUnpack = true;

            nativeBuildInputs =
              [ pkgs.qt6.wrapQtAppsNoGuiHook ]
              ++ pkgs.lib.optionals pkgs.stdenv.isDarwin [ pkgs.darwin.cctools ]
              ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.autoPatchelfHook ];

            buildInputs = [
              pkgs.qt6.qtbase
              pkgs.qt6.qtremoteobjects
              # Both binaries link these: yaml_json.cpp and the log sink are in
              # the shared sources. autoPatchelfHook resolves the binary's
              # DT_NEEDED entries against buildInputs, so leaving them out fails
              # the Linux build with "could not satisfy dependency
              # libyaml-cpp.so.0.8" -- invisibly on macOS, which does not
              # patchelf.
              pkgs.yaml-cpp
              pkgs.spdlog
            ];

            qtWrapperArgs = [
              "--unset LD_LIBRARY_PATH"
              "--set LOGOS_HOST_PATH ${liblogos}/bin/logos_host"
            ];

            installPhase = ''
              runHook preInstall

              mkdir -p $out/bin $out/lib $out/modules

              cp ${build}/bin/${binName} $out/bin/
              chmod -R +w $out/bin

              # Copy liblogos_core so logosctl can link at runtime
              if [ -d ${liblogosLib}/lib ]; then
                cp -r ${liblogosLib}/lib/* $out/lib/
                chmod -R +w $out/lib
              fi

              if [ -d ${modules}/modules ]; then
                cp -r ${modules}/modules/* $out/modules/
              fi
              ${pkgs.lib.optionalString withPkgModules ''
                mkdir -p $out/modules-pkg
                if [ -d ${modules}/modules-pkg ]; then
                  cp -r ${modules}/modules-pkg/* $out/modules-pkg/
                fi
              ''}

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

          # Tests derivation: builds cli_tests + logosctl binary for integration tests
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
              pkgs.yaml-cpp
              pkgs.spdlog
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
              cp bin/logosctl $out/bin/
              # Both binaries ship, so both are tested.
              cp bin/cli_tests_logoscore $out/bin/
              cp bin/integration_tests_logoscore $out/bin/
              cp bin/logoscore $out/bin/

              if [ -d ${liblogosLib}/lib ]; then
                cp -r ${liblogosLib}/lib/* $out/lib/ || true
              fi

              ${pkgs.lib.optionalString pkgs.stdenv.isDarwin ''
                for binary in $out/bin/*; do
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

          # Portable modules — same set, portable variants. Only the package
          # manager ships a distinct `lib-portable`; the other two are
          # variant-agnostic and rely on installPortable to make the bundle
          # self-contained (same split logos-basecamp uses).
          bundledInstallsPortable = map installPortable [ capabilityModuleLib ];
          pkgInstallsPortable = map installPortable [
            packageManagerModuleLibPortable
            packageDownloaderModuleLib
          ];
          modulesPortable = pkgs.runCommand "${pname}-modules-portable-${version}"
            { inherit meta; }
            ''
              mkdir -p $out/modules

              mkdir -p $out/modules-pkg

              for installed in ${pkgs.lib.escapeShellArgs bundledInstallsPortable}; do
                if [ -d "$installed/modules" ]; then
                  cp -r "$installed"/modules/. $out/modules/
                fi
              done

              for installed in ${pkgs.lib.escapeShellArgs pkgInstallsPortable}; do
                if [ -d "$installed/modules" ]; then
                  cp -r "$installed"/modules/. $out/modules-pkg/
                fi
              done

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
              pkgs.yaml-cpp
              pkgs.spdlog
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
          mkBinPortable = { binName, withPkgModules }: pkgs.stdenvNoCC.mkDerivation {
            pname = "${pname}-${binName}-portable";
            inherit version meta;

            dontUnpack = true;

            nativeBuildInputs =
              [ pkgs.qt6.wrapQtAppsNoGuiHook ]
              ++ pkgs.lib.optionals pkgs.stdenv.isDarwin [ pkgs.darwin.cctools ]
              ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.autoPatchelfHook ];

            buildInputs = [
              pkgs.qt6.qtbase
              pkgs.qt6.qtremoteobjects
              # Both binaries link these: yaml_json.cpp and the log sink are in
              # the shared sources. autoPatchelfHook resolves the binary's
              # DT_NEEDED entries against buildInputs, so leaving them out fails
              # the Linux build with "could not satisfy dependency
              # libyaml-cpp.so.0.8" -- invisibly on macOS, which does not
              # patchelf.
              pkgs.yaml-cpp
              pkgs.spdlog
            ];

            passthru = {
              extraDirs = [ "modules" ] ++ pkgs.lib.optional withPkgModules "modules-pkg";
            };

            qtWrapperArgs = [
              "--unset LD_LIBRARY_PATH"
            ];

            installPhase = ''
              runHook preInstall

              mkdir -p $out/bin $out/lib $out/modules

              # The one binary this package ships, from the portable build
              cp ${buildPortable}/bin/${binName} $out/bin/
              cp -L ${liblogosPortable}/bin/logos_host $out/bin/ 2>/dev/null || true

              # Libraries — nix-bundle-dir will resolve and bundle all dependencies
              cp -L ${liblogosPortable}/lib/*.dylib $out/lib/ 2>/dev/null || true
              cp -L ${liblogosPortable}/lib/*.so $out/lib/ 2>/dev/null || true

              # Portable modules
              cp -r ${modulesPortable}/modules/* $out/modules/ 2>/dev/null || true
              ${pkgs.lib.optionalString withPkgModules ''
                mkdir -p $out/modules-pkg
                cp -r ${modulesPortable}/modules-pkg/* $out/modules-pkg/ 2>/dev/null || true
              ''}

              runHook postInstall
            '';
          };

          binLegacy      = mkBin         { binName = "logoscore"; withPkgModules = false; };
          binCtl         = mkBin         { binName = "logosctl";  withPkgModules = true;  };
          binLegacyPort  = mkBinPortable { binName = "logoscore"; withPkgModules = false; };
          binCtlPort     = mkBinPortable { binName = "logosctl";  withPkgModules = true;  };

          logoscoreCli = pkgs.symlinkJoin { name = pname;            paths = [ binLegacy ]; };
          logosctlCli  = pkgs.symlinkJoin { name = "${pname}-ctl";   paths = [ binCtl ];    };
        in
        {
          # `cli` / `cli-*` stay logoscore, so existing consumers -- including
          # every doc-test that does `nix build github:...logos-logoscore-cli`
          # -- get exactly the tool they get today. logosctl is opt-in under
          # its own `ctl` outputs while it is being validated.
          cli = logoscoreCli;
          tests = tests;
          cli-bundle-dir = dirBundler binLegacyPort;
          cli-appimage = appBundler {
            drv = binLegacyPort;
            name = "logoscore";
            bundle = dirBundler binLegacyPort;
            desktopFile = ./assets/logoscore.desktop;
            icon = ./assets/logoscore.png;
          };

          ctl = logosctlCli;
          ctl-bundle-dir = dirBundler binCtlPort;
          ctl-appimage = appBundler {
            drv = binCtlPort;
            name = "logosctl";
            bundle = dirBundler binCtlPort;
            desktopFile = ./assets/logosctl.desktop;
            icon = ./assets/logosctl.png;
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
        in rec {
          # One runner, two flavours. They are separate derivations so nix
          # builds them in parallel: while both binaries ship, a regression in
          # either should surface in the same run, and neither should wait on
          # the other.
          tests-logosctl = pkgs.runCommand "logos-logoscore-cli-tests-logosctl" {
            nativeBuildInputs = [ testsPkg ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.qt6.qtbase ];
          } ''
            export QT_QPA_PLATFORM=offscreen
            export QT_FORCE_STDERR_LOGGING=1
            ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
              export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
            ''}
            export LOGOSCTL_BINARY=${testsPkg}/bin/logosctl
            # Daemon-backed integration tests read these. Absent ⇒ those tests
            # GTEST_SKIP, so the rest still runs where test modules are absent.
            export LOGOSCTL_TEST_MODULES_DIR=${testModulesInstall}/modules
            export LOGOS_HOST_PATH=${liblogos}/bin/logos_host
            mkdir -p $out
            echo "unit tests (shared code)..."
            ${testsPkg}/bin/unit_tests --gtest_output=xml:$out/unit-test-results.xml
            echo "logosctl CLI tests..."
            ${testsPkg}/bin/cli_tests --gtest_output=xml:$out/cli-test-results.xml
            echo "logosctl integration tests..."
            ${testsPkg}/bin/integration_tests --gtest_output=xml:$out/integration-test-results.xml
          '';

          # logoscore's copy of the suites, pinning the surface people actually
          # use. Deleted along with the tool.
          tests-logoscore = pkgs.runCommand "logos-logoscore-cli-tests-logoscore" {
            nativeBuildInputs = [ testsPkg ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.qt6.qtbase ];
          } ''
            export QT_QPA_PLATFORM=offscreen
            export QT_FORCE_STDERR_LOGGING=1
            ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
              export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
            ''}
            export LOGOSCORE_BINARY=${testsPkg}/bin/logoscore
            export LOGOSCORE_TEST_MODULES_DIR=${testModulesInstall}/modules
            export LOGOS_HOST_PATH=${liblogos}/bin/logos_host
            mkdir -p $out
            echo "logoscore CLI tests..."
            ${testsPkg}/bin/cli_tests_logoscore --gtest_output=xml:$out/cli-test-results.xml
            echo "logoscore integration tests..."
            ${testsPkg}/bin/integration_tests_logoscore --gtest_output=xml:$out/integration-test-results.xml
          '';

          # Aggregate. `nix build .#checks.<sys>.tests` still works and now
          # covers both tools; nix builds the two dependencies concurrently.
          tests = pkgs.runCommand "logos-logoscore-cli-tests" { } ''
            mkdir -p $out
            cp -r ${tests-logosctl}/. $out/logosctl/
            cp -r ${tests-logoscore}/. $out/logoscore/
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
            pkgs.yaml-cpp
              pkgs.spdlog
          ];
          shellHook = ''
            export LOGOS_LIBLOGOS_ROOT="${liblogos}"
          '';
        };
      });
    };
}
