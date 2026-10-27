#!/usr/bin/env groovy

library 'status-jenkins-lib@v1.10.3'

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

  environment {
    PLATFORM = 'windows/x86_64'
    ARTIFACT = 'pkg/logosctl-x86_64-windows.zip'
  }

  stages {
    stage('Build') {
      steps { script {
        nix.flake('packages.x86_64-windows.ctl-bundle-dir')
      } }
    }

    stage('Package') {
      steps { script {
        sh '''
          rm -rf logosctl-x86_64-windows pkg
          mkdir -p logosctl-x86_64-windows pkg
          cp -rL result/. logosctl-x86_64-windows/
          chmod -R u+w logosctl-x86_64-windows
          ls logosctl-x86_64-windows/bin/*.exe
        '''
        nix.develop("zip -qr ${env.ARTIFACT} logosctl-x86_64-windows")
      } }
    }

    stage('Archive') {
      steps { script {
        archiveArtifacts(artifacts: env.ARTIFACT, allowEmptyArchive: false)
        env.PKG_URL = "${env.BUILD_URL}artifact/${env.ARTIFACT}"
        jenkins.setBuildDesc(Windows: env.PKG_URL)
      } }
    }
  }

  post {
    success { script { github.notifyPR(true) } }
    failure { script { github.notifyPR(false) } }
    cleanup {
      cleanWs(disableDeferredWipeout: true)
      dir(env.WORKSPACE_TMP) { deleteDir() }
    }
  }
}