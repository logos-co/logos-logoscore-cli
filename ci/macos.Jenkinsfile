#!/usr/bin/env groovy

library 'status-jenkins-lib@v1.10.3'

def isPRBuild = utils.isPRBuild()

pipeline {
  agent { label "macos && aarch64 && nix" }

  options {
    timestamps()
    ansiColor('xterm')
    timeout(time: 45, unit: 'MINUTES')
    buildDiscarder(logRotator(
      numToKeepStr: '10',
      daysToKeepStr: '30',
      artifactNumToKeepStr: '1',
    ))
    disableConcurrentBuilds(
      abortPrevious: isPRBuild
    )
    copyArtifactPermission('/logos/logoscore-cli/*')
  }

  parameters {
    string(
      name: 'BRANCH',
      description: 'Git branch to build.',
      defaultValue: params.BRANCH ?: 'master'
    )
  }

  environment {
    PLATFORM = "macos/aarch64"
    ARTIFACT = "pkg/logosctl-aarch64-macos.tar.gz"
  }

  stages {
    stage('Build') {
      steps { script {
        nix.flake('ctl-bundle-dir')
      } }
    }

    stage('Smoke Test') {
      steps {
        sh 'result/bin/logosctl --version'
      }
    }

    stage('Sign & Notarize') {
      steps { script {
        logos.codesign(
          dirPath: 'result',
          outputPath: 'signed',
          mode: 'both',
          timeout: '30m'
        )
      } }
    }

    stage('Package') {
      steps {
        sh "./scripts/create-tarball.sh --dir signed --output ${env.ARTIFACT}"
      }
    }

    stage('Archive') {
      steps { script {
        archiveArtifacts(artifacts: env.ARTIFACT, allowEmptyArchive: false)
        env.PKG_URL = "${env.BUILD_URL}artifact/${env.ARTIFACT}"
        jenkins.setBuildDesc(TARGZ: env.PKG_URL)
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
