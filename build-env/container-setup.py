import argparse
import requests
import json
import subprocess

import os
from pathlib import Path


SAIREDIS = 'sonic-sairedis'
COMMON = 'sonic-common-libs'
SWSSCOMMON = 'sonic-swsscommon'
BUILDIMAGE = 'sonic-buildimage'
VPP = 'sonic-platform-vpp'

pipeline_id_map = {
    SAIREDIS: 12,
    COMMON: 465,
    SWSSCOMMON: 9,
    BUILDIMAGE: 142,
    VPP: 1016
}

pipeline_artifact_map = {
    SAIREDIS: 'sonic-sairedis-{}',
    COMMON: 'common-lib',
    SWSSCOMMON: 'sonic-swss-common-{}',
    BUILDIMAGE: 'sonic-buildimage.vs',
    VPP: "VPP"
}

deb_files_regex = ['libswsscommon*.deb', 'libnl*.deb', 'libsai*.deb', 'syncd-vs*.deb', 'libyang_*.deb', 'libyang-*.deb', 'python3-swsscommon*.deb', '*vpp*.deb']
dash_deb_regex = ['libproto*.deb', 'libdash*.deb']

pipeline_out_file_map = {
    SAIREDIS: 'sairedis.zip',
    COMMON: 'common-lib.zip',
    SWSSCOMMON: 'swsscommon.zip',
    VPP: 'vpp.zip'
}

build_url = 'https://dev.azure.com/mssonic/build/_apis/build/builds?definitions={}&branchName=refs/heads/{}&resultFilter=succeeded&statusFilter=completed&maxBuildsPerDefinition=1&queryOrder=finishTimeDescending'
artifact_url = 'https://dev.azure.com/mssonic/build/_apis/build/builds/{}/artifacts?artifactName={}&api-version=5.1'


def get_latest_build(pipeline, branch):
    url = build_url.format(pipeline_id_map[pipeline], branch)
    print(url)
    res = requests.get(url)
    build_info = json.loads(res.content)
    if not build_info['value'] and branch == "master":
        url = build_url.format(pipeline_id_map[pipeline], "main")
        print(url)
        res = requests.get(url)
        build_info = json.loads(res.content)

    return build_info['value'][0]['id']


def get_artifact_url(pipeline, build_id, debian_version):
    if pipeline in [SAIREDIS, SWSSCOMMON]:
        artifact_name = pipeline_artifact_map[pipeline].format(debian_version)
    else:
        artifact_name = pipeline_artifact_map[pipeline]
    url = artifact_url.format(build_id, artifact_name)
    print(url)
    res = requests.get(url)
    artifact_info = json.loads(res.content)
    return artifact_info['resource']['downloadUrl']


def download_artifact(pipeline, filename, branch, debian_version):
    build_id = get_latest_build(pipeline, branch)
    download_url = get_artifact_url(pipeline, build_id, debian_version)
    print("URL: {}".format(download_url))

    with open(filename, 'wb') as out_file:
        content = requests.get(download_url, stream=True).content
        out_file.write(content)


def get_all_artifacts(dest_dir, branch, debian_version):
    for pipeline, filename in pipeline_out_file_map.items():
        print("Getting artifact {}".format(pipeline))
        dest_file = os.path.join(dest_dir, filename)
        download_artifact(pipeline, dest_file, branch, debian_version)
        print("Finished getting artifact {}".format(pipeline))


def main(branch, debian_version):
    try:
        work_dir = Path("/tmp/sonic/")
        work_dir.mkdir(parents=True, exist_ok=True)

        get_all_artifacts(str(work_dir), branch, debian_version)

        for filename in pipeline_out_file_map.values():
            print("Extracting {}".format(filename))
            if "common-lib" in filename:
                cmd = ['bash', '-c', f"unzip -l {filename} | grep -oE 'common-lib/target/debs/{debian_version}.*deb$' | xargs unzip -o -j {filename}"]
            else:
                cmd = ['unzip', '-o', '-j', filename]

            subprocess.run(cmd, cwd=work_dir, stdout=subprocess.DEVNULL)

            if "common-lib" in filename:
                cmd = ['bash', '-c', f"unzip -l {filename} | grep -oE 'common-lib/target/debs/bullseye/libproto.*deb$' | xargs unzip -o -j {filename}"]
                subprocess.run(cmd, cwd=work_dir, stdout=subprocess.DEVNULL)

        available_debs = subprocess.run(["ls", work_dir], cwd=work_dir, capture_output=True, text=True).stdout
        debs_to_install = deb_files_regex
        if 'libyang' and 'libproto' in available_debs:
            debs_to_install += dash_deb_regex

        cmd = "env VPP_INSTALL_SKIP_SYSCTL=1 dpkg -i {}".format(" ".join(debs_to_install))
        subprocess.run(cmd, cwd=work_dir, stdout=subprocess.DEVNULL)

        return
    except Exception:
        raise


if __name__ == '__main__':
    parser = argparse.ArgumentParser('SWSS Build Setup')
    parser.add_argument('-b', '--branch', default="master")
    parser.add_argument('-d', '--debian-version', default="bookworm")

    args = parser.parse_args()
    main(args.branch, args.debian_version)
