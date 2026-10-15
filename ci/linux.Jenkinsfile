#!/usr/bin/env groovy

library 'status-jenkins-lib@v1.10.1'

pipeline {
  agent {
    docker {
      label getAgentLabel()
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
    timeout(time: 60, unit: 'MINUTES')
    buildDiscarder(logRotator(
      numToKeepStr: '10',
      daysToKeepStr: '30',
      artifactNumToKeepStr: '10',
    ))
    copyArtifactPermission('/logos/logoscore-cli/*')
  }

  environment {
    ARCH       = getArch()
    NIX_SYSTEM = "${getArch()}-linux"
  }

  stages {
    stage('Build') {
      steps { 
        script {
          nix.flake('ctl-appimage')
          sh '''
            src=$(find result/ -name '*.AppImage' -print -quit)
            mkdir -p pkg
            cp "$src" "logosctl-${ARCH}.AppImage"
            chmod +x "logosctl-${ARCH}.AppImage"
            tar -czf "pkg/logosctl-${ARCH}-linux.tar.gz" "logosctl-${ARCH}.AppImage"
          '''
        } 
      }
    }

    stage('Tests') {
      steps { script {
        nix.flake("checks.${env.NIX_SYSTEM}.tests")
        sh '''
          mkdir -p test-results
          for f in result/*/*.xml; do
            suite=$(basename "$(dirname "$f")")
            cp -L "$f" "test-results/linux-${ARCH}-${suite}-$(basename "$f")"
          done
        '''
      } }
      post {
        always { junit(testResults: 'test-results/*.xml', allowEmptyResults: false) }
      }
    }

    stage('Archive') {
      steps {
        archiveArtifacts(artifacts: 'pkg/*, test-results/*.xml', allowEmptyArchive: false)
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

def getArch() {
  return env.JOB_NAME.contains('aarch64') ? 'aarch64' : 'x86_64'
}

def getAgentLabel() {
  return getArch() == 'aarch64' ? 'linux-arm-container' : 'linuxcontainer'
}