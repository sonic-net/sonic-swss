#!/bin/bash

# download_artifact.sh <pipelineName> <branchName> <artifactName> <targetPaths>
#
# targetPaths: space separated list of target paths to download from the artifact
# e.g.
# ./download_artifact.sh "Azure.sonic-swss-common" "master" "sonic-swss-common" "/libswsscommon-dev_1.0.0_amd64.deb /libswsscommon_1.0.0_amd64.deb"

set -x -e

pipelineName=${1}
branchName=${2}
artifactName=${3}
targetPaths=${4}

queryPipelinesUrl="https://dev.azure.com/mssonic/build/_apis/pipelines"

definitions=$(curl -s "${queryPipelinesUrl}" | jq -r ".value[] | select (.name == \"${pipelineName}\").id")

queryBuildsUrl="https://dev.azure.com/mssonic/build/_apis/build/builds?definitions=${definitions}&branchName=refs/heads/${branchName}&resultFilter=succeeded&statusFilter=completed&api-version=6.0"

buildId=$(curl -s ${queryBuildsUrl} | jq -r '.value[0].id')

queryArtifactUrl="https://dev.azure.com/mssonic/build/_apis/build/builds/${buildId}/artifacts?artifactName=${artifactName}&api-version=6.0"

function download_artifact {

    target_path=${1}
    output_file=$(sed 's/.*\///' <<< ${target_path})

    download_artifact_url=$(curl -s ${queryArtifactUrl} | jq -r '.resource.downloadUrl')
    download_artifact_url=$(sed 's/zip$/file/' <<< ${download_artifact_url})
    download_artifact_url="$download_artifact_url&subPath=${target_path}"

    wget -O ${output_file} ${download_artifact_url}
}

function download_artifacts {
    target_paths_array=(${targetPaths})
    for target_path in "${target_paths_array[@]}"
    do
        download_artifact ${target_path}
    done
}

download_artifacts
