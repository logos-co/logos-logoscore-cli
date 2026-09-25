{
  description = "Logos logosctl CLI - headless module runtime";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
    # On the runtime-control branches (logos-liblogos#227 and the PRs under it) until they merge.
    logos-cpp-sdk.url = "github:logos-co/logos-cpp-sdk/feat/runtime-delegate-export";
    logos-protocol.url = "github:logos-co/logos-protocol/feat/plain-local-inproc";
    logos-liblogos.url = "github:logos-co/logos-liblogos/feat/embedded-core-service";
    # liblogos and the CLI must share one instance of the plain protocol
    # runtime: that library owns the process-wide credential registry.
    logos-cpp-sdk.inputs.logos-protocol.follows = "logos-protocol";
    logos-liblogos.inputs.logos-protocol.follows = "logos-protocol";

    # ONE logos-package-manager for the whole tree. This repo stages
    # ${liblogosPortable}/lib/*.dll into ctl/bin/, which includes
    # libpackage_manager_lib, while the package_manager module ships its own
    # copy in ctl/modules-pkg/package_manager/. PE import tables carry DLL BASE
    # NAMES and the format has no rpath, so Windows resolves a plugin's
    # dependency from the EXECUTABLE'S directory: liblogos's copy wins, whatever
    # the module was built against.
    #
    # Left unpinned, the two drift and the loser is decided at dlopen on one
    # platform only. That is not hypothetical -- the module started calling
    # nodeResolvedToAnInstalledPackage(DependencyStatus) while liblogos still
    # pinned an lgpm two commits before it existed, and package_manager stopped
    # loading with "undefined symbol". A follows makes a future skew an
    # EVALUATION-time disagreement instead.
    #
    # nix-bundle-logos-module-install follows too: it RUNS lgpm at build time to
    # produce the installed tree, so its pin decides which installer builds the
    # bundle, and it was the most stale of the three.
    logos-package-manager.url = "github:logos-co/logos-package-manager";
    logos-liblogos.inputs.logos-package-manager.follows = "logos-package-manager";
    logos-package-manager-module.inputs.logos-package-manager.follows = "logos-package-manager";
    nix-bundle-logos-module-install.inputs.logos-package-manager.follows = "logos-package-manager";
    logos-capability-module.url = "github:logos-co/logos-capability-module/feat/token-authority";
    logos-modules-state-module.url = "github:logos-co/logos-modules-state-module/chore/qt-remote-plain";
    logos-package-manager-module.url = "github:logos-co/logos-package-manager-module/chore/qt-remote-plain";
    logos-package-downloader-module.url = "github:logos-co/logos-package-downloader-module/chore/qt-remote-plain";
    # The integration suites' test modules, as source only: the flake takes this
    # one back as an input, and that cycle unrolled this lock to 15k nodes.
    logos-test-modules-src = { url = "github:logos-co/logos-test-modules"; flake = false; };
    nix-bundle-logos-module-install.url = "github:logos-co/nix-bundle-logos-module-install";
    # Payloads ship their own libiconv (nix-bundle-lgx#17); drop once the installer relocks.
    nix-bundle-lgx.url = "github:logos-co/nix-bundle-lgx/fix/ship-libiconv";
    nix-bundle-logos-module-install.inputs.nix-bundle-lgx.follows = "nix-bundle-lgx";
    nix-bundle-dir.url = "github:logos-co/nix-bundle-dir";
    nix-bundle-appimage.url = "github:logos-co/nix-bundle-appimage";
  };

  outputs = { self, nixpkgs, logos-nix, logos-cpp-sdk, logos-protocol, logos-liblogos, logos-package-manager, logos-capability-module, logos-modules-state-module, logos-package-manager-module, logos-package-downloader-module, logos-test-modules-src, nix-bundle-logos-module-install, nix-bundle-lgx, nix-bundle-dir, nix-bundle-appimage }:
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
          { name = "logos-capability-module"; commit = revOf logos-capability-module; }
          { name = "logos-modules-state-module"; commit = revOf logos-modules-state-module; }
          { name = "logos-package-manager-module"; commit = revOf logos-package-manager-module; }
          { name = "logos-package-downloader-module"; commit = revOf logos-package-downloader-module; }
        ];
      };
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
        cppSdk = logos-cpp-sdk.packages.${system}.logos-cpp-include;
        protocolPkg = logos-protocol.packages.${system}.logos-protocol-plain;
        liblogos = logos-liblogos.packages.${system}.logos-liblogos;
        liblogosLib = logos-liblogos.packages.${system}.logos-liblogos-lib;
        liblogosPortable = logos-liblogos.packages.${system}.portable;
        capabilityModuleLib = logos-capability-module.packages.${system}.lib;
        modulesStateModuleLib = logos-modules-state-module.packages.${system}.lib;
        packageManagerModuleLib = logos-package-manager-module.packages.${system}.lib;
        packageManagerModuleLibPortable = logos-package-manager-module.packages.${system}.lib-portable;
        packageDownloaderModuleLib = logos-package-downloader-module.packages.${system}.lib;
        installDev = nix-bundle-logos-module-install.bundlers.${system}.dev;
        installPortable = nix-bundle-logos-module-install.bundlers.${system}.portable;
        dirBundler = nix-bundle-dir.bundlers.${system}.qtCliApp;
        appBundler = nix-bundle-appimage.lib.${system}.mkAppImage;
      });

      # forAllSystems plus the "x86_64-windows" pseudo-system. Not just
      # logos-nix.lib.forAllTargets, which supplies only { system, pkgs } while
      # this flake threads a dozen per-system dependencies through.
      #
      # Applied to `packages` ONLY. `checks` would have to execute PE test
      # binaries on the Linux builder, and a cross devShell offers no way to
      # run what it produces.
      #
      # Nix attribute values are lazy, so the entries below that have no
      # Windows target are never forced as long as the Windows branch of
      # `packages` does not name them. As of this commit that set is exactly
      # two: `packageDownloaderModuleLib` (see the Windows branch of `packages`
      # for the one blocker that keeps modules-pkg/ off Windows) and
      # `appBundler` (nix-bundle-appimage exposes lib.{aarch64,x86_64}-linux
      # only -- verified by eval, not assumed).
      #
      # `installDev` / `installPortable` DO have an x86_64-windows bundler
      # (`nix-bundle-logos-module-install` at the rev this flake locks exposes
      # bundlers.x86_64-windows.{dev,portable}), so they are no longer in that
      # set even though nothing forces them yet.
      windowsBuildSystem = "x86_64-linux";
      forAllTargets = f:
        nixpkgs.lib.genAttrs (systems ++ [ "x86_64-windows" ]) (system: f {
          inherit system;
          pkgs =
            if system == "x86_64-windows"
            then logos-nix.lib.mkWindowsPkgs { buildSystem = windowsBuildSystem; }
            else import nixpkgs { inherit system; };
          cppSdk = logos-cpp-sdk.packages.${system}.logos-cpp-include;
          protocolPkg = logos-protocol.packages.${system}.logos-protocol-plain;
          liblogos = logos-liblogos.packages.${system}.logos-liblogos;
          liblogosLib = logos-liblogos.packages.${system}.logos-liblogos-lib;
          liblogosPortable = logos-liblogos.packages.${system}.portable;
          capabilityModuleLib = logos-capability-module.packages.${system}.lib;
          modulesStateModuleLib = logos-modules-state-module.packages.${system}.lib;
          packageManagerModuleLib = logos-package-manager-module.packages.${system}.lib;
          packageManagerModuleLibPortable = logos-package-manager-module.packages.${system}.lib-portable;
          packageDownloaderModuleLib = logos-package-downloader-module.packages.${system}.lib;
          installDev = nix-bundle-logos-module-install.bundlers.${system}.dev;
          installPortable = nix-bundle-logos-module-install.bundlers.${system}.portable;
          # Keyed by the BUILD system, not the target, and that is the whole
          # point rather than a workaround. nix-bundle-dir exposes
          # bundlers.{aarch64,x86_64}-{darwin,linux} and NOTHING for
          # x86_64-windows -- deliberately: a bundler is a program that runs on
          # the builder, so `bundlers.x86_64-windows` would be a PE the Linux
          # builder cannot execute. Whether the BUNDLED thing is a PE is a
          # separate question, and mkBundle.nix answers it from `drv.stdenv`
          # (`hostPlatform.isWindows`), which a cross derivation carries. Its
          # own comment names this exact call shape.
          #
          # So: `nix-bundle-dir.bundlers.x86_64-windows` is not a gap waiting to
          # be filled, and reaching for it is the error to avoid -- it evaluates
          # to a missing-attribute throw, which the laziness note above would
          # hide until something forced it.
          dirBundler = nix-bundle-dir.bundlers.${
            if system == "x86_64-windows" then windowsBuildSystem else system
          }.qtCliApp;
          appBundler = if system == "aarch64-linux" || system == "x86_64-linux"
            then nix-bundle-appimage.lib.${system}.mkAppImage
            else null;
        });

      # The integration suites' test modules, built by the builder capability_module
      # comes with, so none of our own is locked. ipc_new_api declares the other two.
      itModules = let
        mkModule = logos-capability-module.inputs.logos-module-builder.lib.mkLogosModule;
        mk = dir: args: mkModule ({
          src = "${logos-test-modules-src}/${dir}";
          configFile = "${logos-test-modules-src}/${dir}/metadata.json";
        } // args);
        basic = mk "test-basic-module" { };
        extlib = mk "test-extlib-module" { };
        ipcNewApi = mk "test-ipc-module-new-api" {
          flakeInputs = { test_basic_module = basic; test_extlib_module = extlib; };
        };
      in [ basic extlib ipcNewApi ];
    in
    {
      packages = forAllTargets ({ pkgs, system, cppSdk, protocolPkg, liblogos, liblogosLib, liblogosPortable, capabilityModuleLib, modulesStateModuleLib, packageManagerModuleLib, packageManagerModuleLibPortable, packageDownloaderModuleLib, installDev, installPortable, dirBundler, appBundler }:
        let
          pname = "logos-logoscore-cli";
          # VERSION is only present on release branches; dev branches use a placeholder.
          version = if builtins.pathExists ./VERSION
            then nixpkgs.lib.removeSuffix "\n" (builtins.readFile ./VERSION)
            else "0.1.0-dev";
          src = ./.;

          isWindows = pkgs.stdenv.hostPlatform.isWindows;
          # ".exe" when cross-compiling. Never hardcode the bare name: an
          # install rule or copy that names only the Unix spelling finds
          # nothing and still exits 0.
          exeExt = pkgs.stdenv.hostPlatform.extensions.executable;

          # Generated header (version + commit hashes) staged into src/ at build
          # time so main.cpp can bake it into the binary for `--version`.
          buildInfoHeader = import ./nix/build-info.nix { inherit pkgs buildInfo; };

          meta = with pkgs.lib; {
            description = "Logos logosctl headless module runtime CLI";
            platforms = platforms.unix ++ platforms.windows;
          };

          # Bundled modules (bundle + lgpm install in one step each).
          #
          # capability_module    — the auth handshake every client needs.
          # modules_state        — the module lifecycle registry liblogos feeds.
          # package_manager      — install/uninstall + the dependency graph.
          # package_downloader   — catalogs, resolution, downloads.
          #
          # These land in $out/modules and are picked up at runtime as the
          # read-only "embedded" directory (paths::bundledModulesDir(),
          # <bin>/../modules). Mirrors logos-basecamp/flake.nix so the CLI
          # and the GUI drive the same module surface.
          bundledInstallsDev = map installDev [ capabilityModuleLib modulesStateModuleLib ];
          # Kept out of modules/ deliberately: logoscore scans that directory, so
          # anything extra there changes what it reports by default. (The doc-tests
          # assert CONTAINMENT -- expect_contains, never an exact list -- so adding a
          # module here does not break them. This split is about keeping logoscore's
          # surface deliberate, not about the assertions.)
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

            # Stage the generated build-info header next to src/version_info.h.
            preConfigure = ''
              cp ${buildInfoHeader} src/logos_build_info.h
              chmod +w src/logos_build_info.h
            '';

            nativeBuildInputs = [
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
            ];

            buildInputs = [
              cppSdk
              protocolPkg
              pkgs.nlohmann_json
              pkgs.openssl
              pkgs.stduuid
              pkgs.cli11
              pkgs.fmt
              pkgs.yaml-cpp
              pkgs.spdlog
            ]
            # CMakeLists.txt skips the whole test block for a Windows host, so
            # gtest is dead weight there -- and cross-building it is a real
            # cost, not a free one.
            ++ pkgs.lib.optional (!isWindows) pkgs.gtest;

            cmakeFlags = [
              "-GNinja"
              "-DLOGOS_LIBLOGOS_ROOT=${liblogos}"
              "-DLOGOS_CPP_SDK_ROOT=${cppSdk}"
              "-DLOGOS_PROTOCOL_ROOT=${protocolPkg}"
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
            # Qt is present only for the copied logos_host_qt child. Wrapping
            # logoscore/logosctl would put Qt setup back on the front-end path.
            dontWrapQtApps = true;

            nativeBuildInputs =
              pkgs.lib.optionals pkgs.stdenv.isDarwin [ pkgs.darwin.cctools ]
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
              protocolPkg
              liblogosLib
            ];

            installPhase = ''
              runHook preInstall

              mkdir -p $out/bin $out/lib $out/modules

              cp ${build}/bin/${binName} $out/bin/
              for host in ${liblogos}/bin/logos_host*; do
                [ -f "$host" ] || continue
                cp -L "$host" $out/bin/
              done
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

          # Windows bin package.
          #
          # A separate function rather than an isWindows branch inside mkBin,
          # because almost nothing is shared: no Qt wrapper, no autoPatchelf,
          # no install_name_tool, no lib/ (PE has no rpath, so a library in
          # lib/ is a library nothing can find), and the module set is the one
          # liblogos already builds rather than the lgx-bundled one.
          #
          # stdenv, not stdenvNoCC: nixpkgs adds win-dll-link.sh to every
          # Windows-host derivation, and that hook shells out to $OBJDUMP to
          # read PE import tables. stdenvNoCC has no bintools, so $OBJDUMP is
          # unset, every objdump call fails, and the hook reports "Created 0
          # DLL link(s)" -- a silent no-op that only shows up as an .exe that
          # will not start on the target machine.
          mkBinWindows = { binName, withPkgModules }: pkgs.stdenv.mkDerivation {
            pname = "${pname}-${binName}";
            inherit version meta;

            dontUnpack = true;
            dontWrapQtApps = true;

            # Read by nix-bundle-dir when this derivation is handed to
            # `dirBundler` (see `ctl-bundle-dir` / `cli-bundle-dir` below).
            # Both entries exist because the bundler's defaults are wrong for
            # a PE, in opposite directions:
            #
            # extraDirs: the bundler carries bin/ and lib/ and nothing else
            # unless a directory is named here. modules/ (and modules-pkg/ for
            # logosctl) is the built-in module tree staged below; without
            # this line the bundle is a STRICT SUBSET of the derivation it
            # bundles -- no modules/, no message saying so, and a logosctl.exe
            # that starts and then reports no modules.
            #
            # extraClosurePaths: a PE has no rpath, and the DLLs this package
            # puts in bin/ are `cp -L` copies, so no store reference survives
            # into $out. The bundler's import sweep therefore has an EMPTY
            # closure to resolve against, and any base name win-dll-link.sh did
            # not already stage has no provider. Naming Qt here gives the sweep
            # somewhere to look. This one is insurance rather than a known gap:
            # unresolved imports are a hard build failure in the bundler
            # (pe_fail_on_unresolved), not a warning, so if it is unnecessary
            # the cost is an unused closure path and if it is necessary the
            # build says so instead of shipping an .exe that will not start.
            passthru = {
              extraDirs = [ "modules" ] ++ pkgs.lib.optional withPkgModules "modules-pkg";
              # Qt is needed only by logos_host_qt, which remains the
              # compatibility process for today's Qt plugins.
              extraClosurePaths = [
                pkgs.qt6.qtbase pkgs.qt6.qtremoteobjects
                pkgs.libjpeg.bin pkgs.sqlite.bin
              ];
            };

            # win-dll-link.sh resolves each imported DLL BASE NAME against the
            # lib/ and bin/ of every buildInput, transitively through the
            # imports of what it finds. Anything missing here is a DLL that
            # never gets staged, and the .exe then exits immediately with no
            # output at all.
            buildInputs = [
              pkgs.qt6.qtbase
              pkgs.qt6.qtremoteobjects
              cppSdk
              protocolPkg
              liblogosLib
              pkgs.yaml-cpp
              pkgs.spdlog
              pkgs.fmt
              pkgs.openssl
            ];

            installPhase = ''
              runHook preInstall

              mkdir -p $out/bin $out/modules

              exe="${buildPortable}/bin/${binName}${exeExt}"
              if [ ! -f "$exe" ]; then
                echo "Error: $exe not found" >&2
                ls -la ${buildPortable}/bin >&2 || true
                exit 1
              fi
              cp "$exe" $out/bin/
              chmod -R +w $out/bin

              # PE import tables carry DLL BASE NAMES and the format has no
              # rpath, so Windows resolves them from the EXECUTABLE'S OWN
              # DIRECTORY. liblogos ships liblogos_core.dll and
              # libpackage_manager_lib.dll under lib/, where nothing looks, and
              # win-dll-link.sh scans only $out/bin so it never sees them
              # either. Symptom when this is skipped: the .exe exits with NO
              # OUTPUT AT ALL (lgpm exited 53 exactly this way).
              #
              # Explicit `for` + `-f`, not a "safe" nullglob array: nullglob
              # only drops patterns that CONTAIN a wildcard, so a fully
              # interpolated literal path survives into the array and the guard
              # passes vacuously.
              staged=0
              for dll in ${liblogosPortable}/lib/*.dll; do
                [ -f "$dll" ] || continue
                cp -L "$dll" $out/bin/
                staged=$((staged + 1))
              done
              if [ "$staged" -eq 0 ]; then
                echo "Error: no DLLs under ${liblogosPortable}/lib; ${binName}${exeExt} would start with no output" >&2
                ls -la ${liblogosPortable}/lib >&2 || true
                exit 1
              fi

              # logos_host_qt.exe, the module-host process this CLI spawns.
              # The native packages inject LOGOS_HOST_PATH through a Qt wrapper
              # script; there is no wrapper on a PE, so the host has to sit
              # beside the CLI where the default lookup finds it.
              hosts=0
              for host in ${liblogosPortable}/bin/*.exe; do
                [ -f "$host" ] || continue
                cp -L "$host" $out/bin/
                hosts=$((hosts + 1))
              done
              if [ "$hosts" -eq 0 ]; then
                echo "Error: no module-host .exe under ${liblogosPortable}/bin" >&2
                ls -la ${liblogosPortable}/bin >&2 || true
                exit 1
              fi
              chmod -R +w $out/bin

              # Built-in modules, from the SAME install-bundler path the native
              # builds use (installPortable -> nix-bundle-lgx), NOT a hand copy.
              #
              # This used to be `cp -r ''${liblogos}/modules/.`, and that is where
              # the first shipped Windows bundle got a capability_module whose
              # manifest named **Qt6Core.dll** as the plugin. Two Unix-shaped
              # assumptions in logos-liblogos/nix/modules.nix combine: it globs
              # `lib/*.dll` -- which matches only the plugin on Unix, but on
              # Windows linkDLLsInfolder has staged 14 dependency DLLs beside it
              # -- and then picks the entry point by taking the FIRST file in
              # the directory, alphabetically Qt6Core.dll. Neither is visible
              # natively, and the build succeeded either way.
              #
              # Going through the bundler fixes both at once and is what
              # logos-basecamp already does (binBundleDir = dirBundler
              # appDistributed -> installPortable): the manifest comes from the
              # module's own metadata rather than a directory listing, and
              # nix-bundle-lgx's mkWindowsPayload strips the host-provided
              # runtime DLLs that bin/ already ships.
              modcount=0
              for m in ${modulesPortable}/modules/*; do
                [ -d "$m" ] || continue
                cp -r "$m" $out/modules/
                modcount=$((modcount + 1))
              done
              if [ "$modcount" -eq 0 ]; then
                echo "Error: no modules under ${modulesPortable}/modules" >&2
                ls -laR ${modulesPortable} >&2 || true
                exit 1
              fi
${pkgs.lib.optionalString withPkgModules ''
              mkdir -p $out/modules-pkg
              pkgcount=0
              for m in ${modulesPortable}/modules-pkg/*; do
                [ -d "$m" ] || continue
                cp -r "$m" $out/modules-pkg/
                pkgcount=$((pkgcount + 1))
              done
              if [ "$pkgcount" -eq 0 ]; then
                echo "Error: no modules under ${modulesPortable}/modules-pkg;" >&2
                echo "       ${binName}${exeExt} would start with no package backend" >&2
                exit 1
              fi
              ''}
              chmod -R +w $out/modules ${pkgs.lib.optionalString withPkgModules "$out/modules-pkg"}

              # Guard the defect class the change above removes: a manifest
              # whose "main" names something that is not that module's plugin.
              # The convention is modules/<name>/<name>_plugin.<ext>, so any
              # other value is wrong by construction. This fails the BUILD
              # rather than shipping a bundle that starts and then cannot load
              # -- which is exactly what happened, because every structural
              # check (file counts, PE counts, zip entries) passed on it.
              for man in $out/modules/*/manifest.json ${pkgs.lib.optionalString withPkgModules "$out/modules-pkg/*/manifest.json"}; do
                [ -f "$man" ] || continue
                mod=$(basename "$(dirname "$man")")
                bad=$(${pkgs.buildPackages.jq}/bin/jq -r '.main | if type=="object" then to_entries[].value else . end' "$man" \
                        | sort -u | grep -v "^''${mod}_plugin\." || true)
                if [ -n "$bad" ]; then
                  echo "Error: $man declares an entry point that is not $mod's plugin:" >&2
                  echo "$bad" >&2
                  exit 1
                fi
              done

              runHook postInstall
            '';
          };

          # Tests derivation: builds cli_tests + logosctl binary for integration tests
          tests = pkgs.stdenv.mkDerivation {
            pname = "${pname}-tests";
            inherit version src meta;

            nativeBuildInputs = [
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
            ] ++ pkgs.lib.optionals pkgs.stdenv.isDarwin [ pkgs.darwin.cctools ]
              ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.autoPatchelfHook ];

            buildInputs = [
              pkgs.nlohmann_json
              pkgs.openssl
              pkgs.stduuid
              pkgs.cli11
              pkgs.gtest
              pkgs.fmt
              pkgs.yaml-cpp
              pkgs.spdlog
              liblogosLib
              cppSdk
              protocolPkg
            ];

            cmakeFlags = [
              "-GNinja"
              "-DLOGOS_LIBLOGOS_ROOT=${liblogos}"
              "-DLOGOS_CPP_SDK_ROOT=${cppSdk}"
              "-DLOGOS_PROTOCOL_ROOT=${protocolPkg}"
            ];

            installPhase = ''
              runHook preInstall

              mkdir -p $out/bin $out/lib

              cp bin/cli_tests $out/bin/
              cp bin/unit_tests $out/bin/
              cp bin/plain_rpc_tests $out/bin/
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
          bundledInstallsPortable = map installPortable [ capabilityModuleLib modulesStateModuleLib ];
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

            # Stage the generated build-info header next to src/version_info.h.
            preConfigure = ''
              cp ${buildInfoHeader} src/logos_build_info.h
              chmod +w src/logos_build_info.h
            '';

            nativeBuildInputs = [
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
            ];

            buildInputs = [
              cppSdk
              protocolPkg
              pkgs.nlohmann_json
              pkgs.openssl
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
            ];
          };

          # Portable bin package — nix-bundle-dir handles library bundling and patching
          mkBinPortable = { binName, withPkgModules }: pkgs.stdenvNoCC.mkDerivation {
            pname = "${pname}-${binName}-portable";
            inherit version meta;

            dontUnpack = true;
            dontWrapQtApps = true;

            nativeBuildInputs =
              pkgs.lib.optionals pkgs.stdenv.isDarwin [ pkgs.darwin.cctools ]
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
              protocolPkg
              liblogosLib
            ];

            passthru = {
              extraDirs = [ "modules" ] ++ pkgs.lib.optional withPkgModules "modules-pkg";
            };

            installPhase = ''
              runHook preInstall

              mkdir -p $out/bin $out/lib $out/modules

              # The one binary this package ships, from the portable build
              cp ${buildPortable}/bin/${binName} $out/bin/
              for host in ${liblogosPortable}/bin/logos_host*; do
                [ -f "$host" ] || continue
                cp -L "$host" $out/bin/
              done

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

          # The unit, CLI and integration suites, cross-built for Windows. Windows
          # CI runs them from the manifest installed beside them; logoscore's
          # copies are not ported (see tests/CMakeLists.txt).
          testsWindows = let
            # The integration suite's modules, for the portable logosctl.exe
            # that `ctl` stages with capability_module beside it.
            itModulesInstalled = map (m: installPortable m.packages.${system}.lib) itModules;
            manifest = builtins.toFile "logosctl-tests.json" (builtins.toJSON {
              suites = [
                { name = "unit"; exe = "bin/unit_tests.exe"; timeout = 120; }
                { name = "plain_rpc"; exe = "bin/plain_rpc_tests.exe"; timeout = 120; }
                # The logosctl.exe the `ctl` target stages beside this one.
                { name = "cli"; exe = "bin/cli_tests.exe"; timeout = 120;
                  env.LOGOSCTL_BINARY = "{stage}/ctl/bin/logosctl.exe"; }
                # One process, so LoadedModuleTest shares one daemon as it does
                # on Linux and macOS.
                { name = "integration"; exe = "bin/integration_tests.exe"; isolation = "suite";
                  wine = false;
                  env = {
                    LOGOSCTL_BINARY = "{stage}/ctl/bin/logosctl.exe";
                    LOGOSCTL_TEST_MODULES_DIR = "{target}/it-modules";
                  }; }
              ];
            });
          in buildPortable.overrideAttrs (old: {
            pname = "${pname}-tests";
            cmakeFlags = old.cmakeFlags ++ [
              "-DLOGOS_BUILD_TESTS=ON"
              # Discovery would run the PE on the build machine.
              "-DCMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST"
            ];
            ninjaFlags = [
              "unit_tests" "plain_rpc_tests" "logosctl_test_child" "cli_tests" "integration_tests"
            ];
            installPhase = ''
              runHook preInstall
              mkdir -p $out/bin $out/share/logos-tests $out/it-modules
              # Their DLLs are linked in beside them by the mingw fixup hook.
              cp bin/unit_tests.exe bin/plain_rpc_tests.exe bin/logosctl_test_child.exe \
                bin/cli_tests.exe bin/integration_tests.exe $out/bin/
              for installed in ${pkgs.lib.escapeShellArgs itModulesInstalled}; do
                cp -r "$installed"/modules/. $out/it-modules/
              done
              cp ${manifest} $out/share/logos-tests/logosctl.json
              runHook postInstall
            '';
          });

          binLegacyWin = mkBinWindows { binName = "logoscore"; withPkgModules = false; };
          binCtlWin    = mkBinWindows { binName = "logosctl";  withPkgModules = true;  };
        in
        if isWindows then {
          # Windows ships the two binaries plus a bundled directory for each.
          #
          # STILL LEFT OUT, and why. Each reason below was re-checked by eval
          # against the revs this flake locks.
          #
          # modules-pkg/ USED TO BE LISTED HERE and no longer is: package_manager
          # and package_downloader now ship on Windows. The blocker was real but
          # cleared upstream -- logos-package-downloader-module gained
          # packages.x86_64-windows when it bumped logos-module-builder
          # 8e4ea1c -> 9d3b7cc (that file's systems list is a hardcoded 4-element
          # array with no x86_64-windows at the older rev). This flake was simply
          # 3 commits behind; a single-input re-lock was the whole fix.
          #
          # Two things the old entry got WRONG, recorded because both were
          # believed and acted on:
          #
          #   * Its stated consequence of shipping package_manager alone --
          #     "installs would land wherever its unset defaults point" -- is
          #     FALSE. Every one of those directories fails closed when unset.
          #     The real hazard is narrower and worse: a configured
          #     `signature_policy: require` is read and then silently DISARMED
          #     in the half-configured state.
          #
          #   * It framed daemon.cpp's first-failure `return` as a reason not to
          #     ship half. It is not a Windows fact at all -- the same early
          #     return leaves the daemon half-configured on every platform, and
          #     because package_manager is first in the list, its failure means
          #     package_downloader is never attempted even when it would load.
          #     That is a product defect with its own fix, not a packaging
          #     constraint. It is now fixed: the loop lives in
          #     src/daemon/package_bootstrap.cpp, loads each module
          #     independently, and unloads package_manager rather than let it
          #     enforce less than the session advertises.
          #
          #   * *-appimage. An AppImage is a Linux ELF runtime concatenated with
          #     a squashfs image; there is no Windows analogue. Confirmed rather
          #     than assumed: nix-bundle-appimage exposes `lib` for
          #     aarch64-linux and x86_64-linux only. Do not force this one.
          #
          #   * portable. NO LONGER LEFT OUT, and the reason it was is worth
          #     keeping because it was wrong in an instructive way. The old
          #     entry argued portable was "redundant" on Windows: a PE import
          #     table carries base names and no rpath, win-dll-link.sh stages
          #     every dependency beside the .exe, so there is no @rpath/$ORIGIN
          #     variant for a portable output to differ from.
          #
          #     All of that is true and none of it is the whole story. `portable`
          #     is not only a LAYOUT distinction -- it is a compile-time one.
          #     LGPM_PORTABLE_BUILD (logos-package-manager/CMakeLists.txt:58)
          #     decides what platformVariantsToTry() returns: without it, the
          #     binary appends "-dev" and accepts ONLY dev variants
          #     (package_manager_lib.cpp:1005-1008). So a dev-built logosctl.exe
          #     beside modules installed as `windows-x86_64` refuses all three
          #     with "installed for variant 'windows-x86_64' which is not
          #     supported on this platform". mkBinWindows therefore builds from
          #     buildPortable/liblogosPortable, matching the installPortable
          #     modules it stages.
          #
          #     buildPortable needed two Windows fixes to be usable here, both
          #     of which `build` already had: the Qt host-tool cmakeFlags, and
          #     the !isWindows guard on wrapQtAppsNoGuiHook.
          #
          # NOW SHIPPED, and what changed. `tests` is testsWindows above: the
          # suites logos-windows-ci runs from their manifest.
          #
          # `*-bundle-dir` used to be on the list above on the premise that
          # "nix-bundle-dir is an ELF/Mach-O tool".
          # That premise has expired -- bundle.sh has a full PE path (import
          # table sweep, wrong-machine DLL refusal, hard failure on an
          # unresolved import). What is still true, and is what the premise was
          # probably reaching for, is that there is no
          # `nix-bundle-dir.bundlers.x86_64-windows`; the bundler is taken from
          # the BUILD system and detects the PE target from the derivation. See
          # `dirBundler` above.
          #
          # What the bundle adds over the plain binary package, given that
          # win-dll-link.sh has already staged the imports: it re-resolves the
          # whole import closure and FAILS the build on anything it cannot
          # satisfy, where the plain package's only check is that its own two
          # copy loops moved a nonzero number of files. That is worth having on
          # a target whose signature failure is 0xC0000135 before main() with no
          # output at all.
          #
          # COVERAGE, stated plainly: NONE of these four outputs has ever been
          # built, here or in CI -- this repo's workflows have no Windows job at
          # all. x86_64-windows realises on x86_64-linux and no Linux builder
          # was reachable while this was written, so what is verified is
          # evaluation only: every drvPath below resolves. Nothing here has run
          # on Windows, and a PE bundle that builds is not a PE bundle that
          # starts.
          ctl = binCtlWin;
          cli = binLegacyWin;
          tests = testsWindows;
          ctl-bundle-dir = dirBundler binCtlWin;
          cli-bundle-dir = dirBundler binLegacyWin;
          default = binCtlWin;
        } else {
          # `cli` / `cli-*` stay logoscore, so existing consumers -- including
          # every doc-test that does `nix build github:...logos-logoscore-cli`
          # -- get exactly the tool they get today. logosctl is opt-in under
          # its own `ctl` outputs while it is being validated.
          cli = logoscoreCli;
          tests = tests;
          cli-bundle-dir = dirBundler binLegacyPort;
          ctl = logosctlCli;
          ctl-bundle-dir = dirBundler binCtlPort;
          default = logoscoreCli;
        } // pkgs.lib.optionalAttrs pkgs.stdenv.isLinux {
          cli-appimage = appBundler {
            drv = binLegacyPort;
            name = "logoscore";
            bundle = dirBundler binLegacyPort;
            desktopFile = ./assets/logoscore.desktop;
            icon = ./assets/logoscore.png;
          };
          ctl-appimage = appBundler {
            drv = binCtlPort;
            name = "logosctl";
            bundle = dirBundler binCtlPort;
            desktopFile = ./assets/logosctl.desktop;
            icon = ./assets/logosctl.png;
          };
        }
      );

      checks = forAllSystems ({ pkgs, system, liblogos, capabilityModuleLib, installDev, ... }:
        let
          testsPkg = self.packages.${system}.tests;

          # Installed as the daemon discovers modules. Without capability_module
          # every load and call blocks for about 20 s.
          itModulesDir = pkgs.symlinkJoin {
            name = "logos-logoscore-cli-it-modules";
            paths = map installDev
              ([ capabilityModuleLib ] ++ map (m: m.packages.${system}.lib) itModules);
          };

          # Each group skips itself when its modules do not load, so a skip fails here.
          mkIntegration = { name, binaryVar, modulesVar, binary, suite }:
            pkgs.runCommand "logos-logoscore-cli-${name}" { } ''
              export QT_QPA_PLATFORM=offscreen
              export QT_FORCE_STDERR_LOGGING=1
              ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
                export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
              ''}
              export ${binaryVar}=${testsPkg}/bin/${binary}
              export ${modulesVar}=${itModulesDir}/modules
              export LOGOS_HOST_PATH=${liblogos}/bin/logos_host
              mkdir -p $out
              ${testsPkg}/bin/${suite} --gtest_output=xml:$out/results.xml
              if grep -q 'result="skipped"' $out/results.xml; then
                echo "FAIL: ${suite} skipped cases:" >&2
                grep -A1 'result="skipped"' $out/results.xml >&2
                exit 1
              fi
            '';
        in rec {
          # One runner, two flavours. They are separate derivations so nix
          # builds them in parallel: while both binaries ship, a regression in
          # either should surface in the same run, and neither should wait on
          # the other.
          tests-logosctl = pkgs.runCommand "logos-logoscore-cli-tests-logosctl" {
            nativeBuildInputs = [ testsPkg ];
          } ''
            export LOGOSCTL_BINARY=${testsPkg}/bin/logosctl
            export LOGOS_HOST_PATH=${liblogos}/bin/logos_host
            mkdir -p $out
            echo "unit tests (shared code)..."
            ${testsPkg}/bin/unit_tests --gtest_output=xml:$out/unit-test-results.xml
            ${testsPkg}/bin/plain_rpc_tests --gtest_output=xml:$out/plain-rpc-test-results.xml
            echo "logosctl CLI tests..."
            ${testsPkg}/bin/cli_tests --gtest_output=xml:$out/cli-test-results.xml
          '';

          # logoscore's copy of the suites, pinning the surface people actually
          # use. Deleted along with the tool.
          tests-logoscore = pkgs.runCommand "logos-logoscore-cli-tests-logoscore" {
            nativeBuildInputs = [ testsPkg ];
          } ''
            export LOGOSCORE_BINARY=${testsPkg}/bin/logoscore
            export LOGOS_HOST_PATH=${liblogos}/bin/logos_host
            mkdir -p $out
            echo "logoscore CLI tests..."
            ${testsPkg}/bin/cli_tests_logoscore --gtest_output=xml:$out/cli-test-results.xml
          '';

          # The daemon-backed suites, against real modules.
          integration-logosctl = mkIntegration {
            name = "integration-logosctl";
            binaryVar = "LOGOSCTL_BINARY";
            modulesVar = "LOGOSCTL_TEST_MODULES_DIR";
            binary = "logosctl";
            suite = "integration_tests";
          };
          integration-logoscore = mkIntegration {
            name = "integration-logoscore";
            binaryVar = "LOGOSCORE_BINARY";
            modulesVar = "LOGOSCORE_TEST_MODULES_DIR";
            binary = "logoscore";
            suite = "integration_tests_logoscore";
          };

          # Front-end boundary gate. It asserts one shared lp_* runtime, no
          # private lp_* copies in consumers, and no Qt/full-protocol dynamic
          # dependency in logoscore, logosctl, or liblogos_core.
          # Build: nix build .#checks.<sys>.symbol-gate
          symbol-gate = import ./nix/symbol-gate.nix {
            inherit pkgs;
            appPkg = self.packages.${system}.default;
          };

          # Negative control. Plants a REAL duplicate runtime where an
          # in-process consumer goes and asserts the gate REJECTS it. Ship both
          # or neither: an absence assertion that has never been seen to fail is
          # indistinguishable from a broken one.
          symbol-gate-negative = import ./nix/symbol-gate.nix {
            inherit pkgs;
            appPkg = self.packages.${system}.default;
            negativeControl = true;
          };

          # The shipped logosctl: its bundled capability_module is the authority.
          bundled-authority = import ./nix/bundled-authority.nix {
            inherit pkgs;
            ctlPkg = self.packages.${system}.ctl;
            modulesDir = "${itModulesDir}/modules";
          };

          # Aggregate. `nix build .#checks.<sys>.tests` covers both tools, the
          # integration suites included; nix builds its dependencies concurrently.
          tests = pkgs.runCommand "logos-logoscore-cli-tests" { } ''
            mkdir -p $out
            cp -r ${tests-logosctl}/. $out/logosctl/
            cp -r ${tests-logoscore}/. $out/logoscore/
            cp -r ${integration-logosctl}/. $out/integration-logosctl/
            cp -r ${integration-logoscore}/. $out/integration-logoscore/
            cp -r ${bundled-authority}/. $out/bundled-authority/
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
            pkgs.nlohmann_json
            pkgs.openssl
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
