#!/usr/bin/env groovy

library 'status-jenkins-lib@v1.10.1'

urls = [:]

pipeline {
  agent {
    docker {
      label 'linuxcontainer'
      image 'harbor.status.im/infra/ci-build-containers:linux-base-1.0.2'
      args '--volume=/nix:/nix ' +
           '--volume=/etc/nix:/etc/nix '
    }
  }

  options {
    timestamps()
    ansiColor('xterm')
    disableConcurrentBuilds()
    disableRestartFromStage()
    timeout(time: 120, unit: 'MINUTES')
    buildDiscarder(logRotator(
      numToKeepStr: '10',
      daysToKeepStr: '30',
      artifactNumToKeepStr: '10',
    ))
    copyArtifactPermission('/logos/logoscore-cli/*')
  }

  parameters {
    booleanParam(
      name: 'PUBLISH',
      description: 'Publish a GitHub pre-release tagged with the VERSION file.',
      defaultValue: utils.isReleaseBuild() || (params.PUBLISH ?: false)
    )
    string(
      name: 'BRANCH',
      description: 'Git branch to build.',
      defaultValue: params.BRANCH ?: 'master'
    )
  }

  environment {
    GH_USER = 'logos-co'
    GH_NAME = 'logos-logoscore-cli'
  }

  stages {
    stage('Version') {
      steps { script {
        env.VERSION = fileExists('VERSION') ? readFile('VERSION').trim() : ''
        if (params.PUBLISH && !env.VERSION) {
          error('PUBLISH needs a non-empty VERSION file (release/** branches only).')
        }
        echo "Version: ${env.VERSION ?: '<none, not publishing>'}"
      } }
    }

    stage('Build') {
      parallel {
        stage('Linux/x86_64') { steps { script {
          getArtifacts('Linux', build(
            job: 'logos/logoscore-cli/systems/linux/x86_64/package',
            parameters: [string(name: 'BRANCH', value: params.BRANCH)],
          ))
        } } }
        stage('Linux/aarch64') { steps { script {
          getArtifacts('Linux-ARM', build(
            job: 'logos/logoscore-cli/systems/linux/aarch64/package',
            parameters: [string(name: 'BRANCH', value: params.BRANCH)],
          ))
        } } }
        stage('macOS/aarch64') { steps { script {
          getArtifacts('macOS', build(
            job: 'logos/logoscore-cli/systems/macos/aarch64/package',
            parameters: [
              string(name: 'BRANCH', value: params.BRANCH),
              booleanParam(name: 'RELEASE', value: params.PUBLISH),
            ],
          ))
        } } }
        stage('Windows/x86_64') { steps { script {
          getArtifacts('Windows', build(
            job: 'logos/logoscore-cli/systems/windows/x86_64/package',
            parameters: [string(name: 'BRANCH', value: params.BRANCH)],
          ))
        } } }
      }
    }

    stage('Publish Release') {
      when { expression { params.PUBLISH } }
      steps { script {
        def files = findFiles(glob: 'pkg/logosctl-*')*.path
        if (files.size() != 4) {
          error("Expected 4 release artifacts, found ${files.size()}: ${files}")
        }
        github.upsertRelease(
          user:    env.GH_USER,
          repo:    env.GH_NAME,
          version: env.VERSION,
          name:    "Pre-release ${env.VERSION}",
          branch:  env.GIT_COMMIT,
          desc:    readFile('ci/RELEASE_NOTES.md').replace('@TAG@', env.VERSION),
          files:   files,
        )
        echo "Published ${env.VERSION}"
      } }
    }
  }

  post {
    always { script {
      archiveArtifacts(artifacts: 'pkg/*', allowEmptyArchive: true)
      if (urls) { jenkins.setBuildDesc(urls) }
    } }
    cleanup {
      cleanWs(disableDeferredWipeout: true)
      dir(env.WORKSPACE_TMP) { deleteDir() }
    }
  }
}

def getArtifacts(key, childBuild) {
  jenkins.copyArts(childBuild)
  urls[key] = childBuild.absoluteUrl
  jenkins.setBuildDesc(urls)
  return childBuild
}
