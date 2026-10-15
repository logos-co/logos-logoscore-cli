#!/usr/bin/env groovy

library 'status-jenkins-lib@v1.10.1'

pipeline {
  agent {
    docker {
      label 'linuxcontainer'
      image 'harbor.status.im/infra/ci-build-containers:linux-base-1.0.2'
      args '--volume=/nix:/nix ' +
           '--volume=/etc/nix:/etc/nix '
    }
  }

  parameters {
    string(
      name: 'BRANCH',
      description: 'Git branch to build.',
      defaultValue: params.BRANCH ?: 'master'
    )
  }

  options {
    timestamps()
    ansiColor('xterm')
    disableConcurrentBuilds()
    disableRestartFromStage()
    /* mingw Qt/boost closure is large; generous if the cache misses. */
    timeout(time: 90, unit: 'MINUTES')
    buildDiscarder(logRotator(
      numToKeepStr: '10',
      daysToKeepStr: '30',
      artifactNumToKeepStr: '10',
    ))
    copyArtifactPermission('/logos/logoscore-cli/*')
  }

  stages {
    stage('Build') {
      steps { script {
        nix.flake('packages.x86_64-windows.ctl-bundle-dir')
      } }
    }

    stage('Package') {
      steps {
        /* zip/unzip from the flake's locked nixpkgs. cp -L dereferences the
         * DLL symlinks; the count check catches a bundle that didn't. */
        sh '''#!/usr/bin/env bash
          set -euo pipefail
          nix shell --inputs-from . nixpkgs#zip nixpkgs#unzip --command bash -euo pipefail <<'EOF'
            dst="logosctl-x86_64-windows"
            src=$(readlink -f result)
            staged=$(find -L "$src" -type f | wc -l | tr -d ' ')
            [ "$staged" -gt 0 ] || { echo "Bundle is empty"; exit 1; }

            rm -rf "$dst" && mkdir "$dst"
            cp -rL "$src"/. "$dst/"
            chmod -R u+w "$dst"
            mkdir -p pkg
            zip -qr "pkg/$dst.zip" "$dst"

            zipped=$(unzip -l "pkg/$dst.zip" | tail -1 | awk '{print $2}')
            echo "bundle $staged file(s) -> zip $zipped entr(ies)"
            [ "$zipped" -ge "$staged" ] || { echo "Zip is missing files (undereferenced symlinks?)"; exit 1; }

            n_exe=$(find "$dst" -name '*.exe' | wc -l | tr -d ' ')
            [ "$n_exe" -gt 0 ] || { echo "No .exe in bundle"; exit 1; }
EOF
        '''
      }
    }

    stage('Archive') {
      steps {
        archiveArtifacts(artifacts: 'pkg/*', allowEmptyArchive: false)
      }
    }
  }

  post {
    cleanup {
      cleanWs(disableDeferredWipeout: true)
      dir(env.WORKSPACE_TMP) { deleteDir() }
    }
  }
}