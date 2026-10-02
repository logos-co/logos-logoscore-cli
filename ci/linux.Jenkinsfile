#!/usr/bin/env groovy

library 'status-jenkins-lib@v1.9.47'

def isPRBuild = utils.isPRBuild()

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
    booleanParam(
      name: 'RELEASE',
      description: 'Decides whether release credentials are used.',
      defaultValue: params.RELEASE ?: false
    )
  }

  options {
    timestamps()
    ansiColor('xterm')
    timeout(time: 30, unit: 'MINUTES')
    buildDiscarder(logRotator(
      numToKeepStr: '10',
      daysToKeepStr: '30',
    ))
    disableConcurrentBuilds(
      abortPrevious: isPRBuild
    )
  }

  environment {
    NIX_SYSTEM   = "${getArch()}-linux"
    CACHIX_CACHE = 'logos-co'
  }

  stages {
    stage('Build') {
      steps { script {
        nix.flake('default')
      } }
    }

    stage('Tests') {
      steps { script {
        nix.flake("checks.${env.NIX_SYSTEM}.tests")
        sh 'mkdir -p test-results && cp -L result/*.xml test-results/'
      } }
      post {
        always { junit(testResults: 'test-results/*.xml', allowEmptyResults: true) }
      }
    }

    stage('Cachix Push') {
      when { expression { !isPRBuild } }
      steps { script {
        withCredentials([
          string(credentialsId: 'cachix-auth-token', variable: 'CACHIX_AUTH_TOKEN')
        ]) {
          sh "nix build --no-link --print-out-paths '.#default' '.#tests' '.#checks.${env.NIX_SYSTEM}.tests' | cachix push ${env.CACHIX_CACHE}"
        }
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

def getArch() {
  def tokens = Thread.currentThread().getName().split('/')
  for (def arch in ['x86_64', 'aarch64']) {
    if (tokens.contains(arch)) { return arch }
  }
  return 'x86_64'
}

def getAgentLabel() {
  return getArch() == 'aarch64' ? 'linux-arm-container' : 'linuxcontainer'
}