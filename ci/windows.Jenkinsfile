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
      steps { script {
        nix.develop('./scripts/package-windows.sh result pkg')
      } }
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