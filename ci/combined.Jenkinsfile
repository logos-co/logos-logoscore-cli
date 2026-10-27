#!/usr/bin/env groovy

library 'status-jenkins-lib@v1.10.3'

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
        env.VERSION = getVersion()
        echo "Version: ${env.VERSION ?: '<none, not publishing>'}"
      } }
    }

    stage('Build') {
      parallel {
        stage('Linux/x86_64') { steps { script {
          getArtifacts('Linux', build(
            job: 'logos/logoscore-cli/systems/linux/x86_64/package',
            parameters: [string(name: 'BRANCH', value: env.GIT_COMMIT)],
          ))
        } } }
        stage('Linux/aarch64') { steps { script {
          getArtifacts('Linux-ARM', build(
            job: 'logos/logoscore-cli/systems/linux/aarch64/package',
            parameters: [string(name: 'BRANCH', value: env.GIT_COMMIT)],
          ))
        } } }
        stage('macOS/aarch64') { steps { script {
          getArtifacts('macOS', build(
            job: 'logos/logoscore-cli/systems/macos/aarch64/package',
            parameters: [string(name: 'BRANCH', value: env.GIT_COMMIT)],
          ))
        } } }
        stage('Windows/x86_64') { steps { script {
          getArtifacts('Windows', build(
            job: 'logos/logoscore-cli/systems/windows/x86_64/package',
            parameters: [string(name: 'BRANCH', value: env.GIT_COMMIT)],
          ))
        } } }
      }
    }

    stage('Publish Release') {
      when { branch 'release/*' }
      steps { script {
        def files = findFiles(glob: 'pkg/logosctl-*')
        if (files.size() != 4) {
          error("Expected 4 release artifacts, found ${files.size()}: ${files*.path}")
        }
        checkTag()
        github.upsertRelease(
          user:    env.GH_USER,
          repo:    env.GH_NAME,
          version: env.VERSION,
          branch:  env.GIT_COMMIT,
          desc:    getReleaseNotes(),
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

/* Reads VERSION. On release/* branches it must exist and be valid semver. */
def getVersion() {
  def version = fileExists('VERSION') ? readFile('VERSION').trim() : ''
  if (!env.BRANCH_NAME?.startsWith('release/')) {
    return version
  }
  def semver = /^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$/
  if (!(version ==~ semver)) {
    error("VERSION '${version}' is missing or not semver.")
  }
  currentBuild.displayName = "${version} #${env.BUILD_NUMBER}"
  return version
}

/* upsertRelease never moves a tag. If VERSION is already tagged at another
 * commit, the release would get new binaries under an old tag. */
def checkTag() {
  def tagged = sh(
    returnStdout: true,
    script: "git ls-remote --tags origin 'refs/tags/${env.VERSION}'"
  ).trim()
  if (tagged && !tagged.startsWith(env.GIT_COMMIT)) {
    error("Tag ${env.VERSION} already exists at another commit. Bump VERSION.")
  }
}

/* Release notes template with placeholders filled, escaped for the
 * single-quoted -d argument in github.releasePublish. */
def getReleaseNotes() {
  return readFile('ci/RELEASE_NOTES.md')
    .replace('@TAG@', env.VERSION)
    .replace('@COMMIT@', env.GIT_COMMIT)
    .replace("'", "'\\''")
}
