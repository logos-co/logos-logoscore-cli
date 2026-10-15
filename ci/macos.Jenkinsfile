#!/usr/bin/env groovy

library 'status-jenkins-lib@v1.10.1'

def isPRBuild = utils.isPRBuild()

pipeline {
  agent { label "macos && ${getArch()} && nix" }

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
    booleanParam(
      name: 'RELEASE',
      description: 'Decides whether release credentials are used.',
      defaultValue: params.RELEASE ?: false
    )
    string(
      name: 'BRANCH',
      description: 'Git branch to build.',
      defaultValue: params.BRANCH ?: 'master'
    )
  }

  environment {
    PLATFORM = "macos/${getArch()}"
    ARTIFACT = "pkg/${utils.pkgFilename(name: 'logos-logoscore-cli', type: 'macos', ext: 'tar.gz', arch: getArch())}"
  }

  stages {
    stage('Build') {
      steps { script {
        nix.flake('cli-bundle-dir')
      } }
    }

    stage('Smoke Test') {
      steps {
        sh 'result/bin/logoscore --version'
      }
    }

    stage('Sign & Notarize') {
      steps {
        script {
          logos.codesign(
            dirPath: 'result',
            outputPath: 'signed',
            mode: 'both',
            timeout: '30m'
          )
        }
      }
    }

    stage('Package') {
      steps {
        sh "./scripts/create-tarball.sh --dir signed --output ${env.ARTIFACT}"
      }
    }

    stage('Archive') {
      steps { script {
        archiveArtifacts(artifacts: env.ARTIFACT, allowEmptyArchive: false)
        jenkins.setBuildDesc(TARGZ: "${env.BUILD_URL}artifact/${env.ARTIFACT}")
      } }
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
  def tokens = Thread.currentThread().getName().split('/')
  for (def arch in ['x86_64', 'aarch64']) {
    if (tokens.contains(arch)) { return arch }
  }
}