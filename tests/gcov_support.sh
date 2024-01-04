#!/bin/bash
## This script is to enable the gcov support of the SONiC source codes
work_dir=$(pwd)

GCNO_LIST_FILE="gcno_file_list.txt"
TMP_GCDA_FILE_LIST="tmp_gcda_file_list.txt"
ALLMERGE_DIR=AllMergeReport
GCOV_OUTPUT=${work_dir}/gcov_output
HTML_FILE_PREFIX="GCOVHTML_"
INFO_FILE_PREFIX="GCOVINFO_"
BASE_COV_FILE="${GCOV_OUTPUT}/gcno_only.info"

# overload pushd and popd to reduce log output
pushd()
{
    command pushd "$@" > /dev/null
}
popd()
{
    command popd > /dev/null
}

# reset compiling environment
gcov_support_clean()
{
    find /tmp/gcov -name "$INFO_FILE_PREFIX*" -exec "rm" "-rf" "{}" ";"
    find /tmp/gcov -name "$HTML_FILE_PREFIX*" -exec "rm" "-rf" "{}" ";"
    find /tmp/gcov -name "*.gcno" -exec "rm" "-rf" "{}" ";"
    find /tmp/gcov -name "*.gcda" -exec "rm" "-rf" "{}" ";"
    find /tmp/gcov -name $TMP_GCDA_FILE_LIST -exec "rm" "-rf" "{}" ";"
    rm /tmp/gcov/info_err_list
    rm /tmp/gcov/gcda_dir_list.txt
}

# verify whether the info file generated is valid
verify_info_file()
{
    local file=$1
    local valid_line_count
    valid_line_count=$(grep --count "FN:" "${file}")

    if [ "$valid_line_count" -lt 1 ] ;then
        echo "${file}" >> info_err_list
        rm "${file}"
    fi
}

generate_single_tracefile()
{
    local gcno_dir=$1
    local output_file=${gcno_dir}/$2
    local tmp_file=${gcno_dir}/tmp.info
    echo "Generating ${output_file}"
    # Always generate baseline tracefile with zero coverage for all GCNO files
    # This ensures that the final coverage report includes all instrumented files even if they were not run during testing
    lcov --initial --capture --directory "${gcno_dir}" --output-file "${output_file}" &>/dev/null
    GCDA_COUNT=$(find "${gcno_dir}" -name "*.gcda" | wc -l)
    if [ "$GCDA_COUNT" -ge 1 ]; then
        lcov --capture --directory "${gcno_dir}" --output-file "${tmp_file}" &>/dev/null
        lcov --add-tracefile "${output_file}" --add-tracefile "${tmp_file}" --output-file "${output_file}" &>/dev/null
        rm "${tmp_file}"
    fi
    verify_info_file "${output_file}"
    echo "Done generating ${output_file}"
}

# generate gcov base info and html report for specified range files
generate_archive_tracefile()
{
    local gcno_dir=$1
    local output_file=$2
    local tmp_file=${gcno_dir}/tmp.info
    fastcov -l -X -e "/usr/" "tests/" -o "${output_file}" -d "${gcno_dir}" &>/dev/null
    # Always generate baseline tracefile with zero coverage for all GCNO files
    # This ensures that the final coverage report includes all instrumented files even if they were not run during testing
    # lcov --initial --capture --directory "${gcno_dir}" --output-file "${output_file}" &>/dev/null
    # GCDA_COUNT=$(find "${gcno_dir}" -name "*.gcda" | wc --lines)
    # if [ "$GCDA_COUNT" -ge 1 ]; then
        # lcov --capture --directory "${gcno_dir}" --output-file "${tmp_file}" &>/dev/null
        # lcov --add-tracefile "${output_file}" --add-tracefile "${tmp_file}" --output-file "${output_file}" &>/dev/null
        # rm "${tmp_file}"
    # fi
}

lcov_merge_all()
{
    local source_dir=$1
    info_files=$(find "${GCOV_OUTPUT}" -name "*.info")
    while IFS= read -r info_file; do
        if [ ! -f "total.info" ]; then
            # lcov -o total.info -a "${info_file}"
            fastcov -l -o total.info -C "${info_file}"
        else
            fastcov -l -o total.info -C total.info "${info_file}"
            # lcov -o total.info -a total.info -a "${info_file}"
        fi
    done <<< "$info_files"
    free -h
    df -h

    echo "Generating cobertura report"

    # Remove unit test files and system libraries
    # lcov -o total.info -r total.info "*tests/*"
    # lcov -o total.info -r total.info "/usr/*"
    set -x
    timeout -v 5s python lcov_cobertura.py -h
    timeout -v 5m python lcov_cobertura.py total.info --output coverage.xml --demangle --base-dir "${source_dir}"
    echo "Done generating report"

    mkdir -p gcov_output/${ALLMERGE_DIR}
    cp coverage.xml gcov_output/${ALLMERGE_DIR}
}

collect_container_gcda()
{
    local archive_dir containers

    archive_dir=$1
    mkdir -p "${archive_dir}"/gcda_archives/sonic-gcov

    containers=$(docker ps -q)

    echo "### Start collecting info files from existed containers:"
    echo "${containers}"

    while IFS= read -r line; do
        local container_id=${line}
        docker exec "${container_id}" killall5 -15
        gcda_count=$(docker exec "${container_id}" find / -name "*.gcda" 2>/dev/null | wc -l)
        if [ "${gcda_count}" -gt 0 ]; then
            echo "Found ${gcda_count} gcda files in container ${container_id}"
            mkdir -p "${archive_dir}/gcda_archives/sonic-gcov/${container_id}"
            docker exec "${container_id}" bash -c "tar -C /__w/1/s/ -zcvf /tmp/gcov/gcda_${container_id}.tar.gz ."
            docker cp "${container_id}":/tmp/gcov/ "${archive_dir}/gcda_archives/sonic-gcov/${container_id}/"
        else
            echo "No gcda archives found in container ${container_id}"
        fi
    done <<< "${containers}"
}

process_gcda_archive()
{
    local archive_path=$1
    echo "Generating tracefiles from ${archive_path}"

    local src_archive src_dir tmp_dir
    src_archive=$(basename "${archive_path}")
    src_dir=$(dirname "${archive_path}")
    tmp_dir=${src_archive//".tar.gz"/}
    dst_tracefile="${src_dir}/${tmp_dir}.info"
    container_dir="$(pwd)"

    # create a temp working directory for the GCDA archive so that GCDA files from other archives aren't overwritten
    mkdir --parents "${tmp_dir}"
    # symlink GCNO files to temp directory to save disk space
    find gcov/ -name "*.gcno" -exec "cp" "--parents" "--recursive" "--symbolic-link" "${container_dir}/{}" "${tmp_dir}/" ";"
    cp --recursive "${tmp_dir}${container_dir}/gcov/." "${tmp_dir}"
    tar --directory="${tmp_dir}" --extract --gzip --file="${src_dir}/${src_archive}"

    generate_archive_tracefile "${tmp_dir}" "${dst_tracefile}"

    rm --recursive --force "${tmp_dir}"
    echo "Done generating tracefiles from ${archive_path}"
}

combine_tracefiles()
{
    local archive src_dir tracefiles dst_file
    archive=$(basename "${1}")
    src_dir=${archive//".tar.gz"/}
    tracefiles=$(find "${src_dir}" -name "*.info")

    while IFS= read -r src_file; do
        dst_file=${src_file//${src_dir}/"gcov"}
        if [ -f "${dst_file}" ]; then
            lcov -o "${dst_file}" -a "${dst_file}" -a "${src_file}"
        else
            lcov -o "${dst_file}" -a "${src_file}"
        fi
    done <<< "$tracefiles"
}

generate_tracefiles()
{
    local container_dir_list
    container_dir_list=$(find . -maxdepth 1 -mindepth 1 -type d) 

    while IFS= read -r container_id; do
        echo "Processing container ${container_id}"
        pushd "${container_id}"

        # untar any GCNO archives
        local gcno_archives gcda_archives
        gcno_archives=$(find . -name "gcno*.tar.gz")
        if [ -z "${gcno_archives}" ]; then
            echo "No GCNO files found!"
        else
            echo "Found GCNO archives:"
            echo "${gcno_archives}"
            while IFS= read -r gcno_archive; do
                tar --directory="$(dirname "${gcno_archive}")" --gzip --extract --file="${gcno_archive}"
            done <<< "$gcno_archives"
        fi

        if [ ! -f "$BASE_COV_FILE" ]; then
            fastcov --lcov --skip-exclusion-markers --process-gcno --exclude "/usr/" "tests/" --output "${BASE_COV_FILE}" --search-directory . &>/dev/null
        fi

        gcda_archives=$(find . -name 'gcda*.tar.gz')
        if [ -z "${gcda_archives}" ]; then
            echo "No GCDA files found!"
        else
            echo "Found GCDA archives:"
            echo "${gcda_archives}"
            while IFS= read -r gcda_archive; do
                process_gcda_archive "${gcda_archive}" 
            done <<< "$gcda_archives"
            wait
        fi
        popd

        mkdir --parents "gcov_output/${container_id}"
        find "${container_id}" -name "*.info" -exec "cp" "-r" "--parents" "{}" "gcov_output/" ";"

        rm --recursive --force "${container_id}"
        echo "Done processing container ${container_id}"
    done <<< "$container_dir_list" 
    echo "Tracefile generation completed"
}

# list and save the generated .gcda files
gcov_support_collect_gcda()
{
    echo "gcov_support_collect_gcda begin"
    local gcda_count

    gcov_support_clean

    pushd /tmp/gcov
    gcno_count=$(find . -name "gcno*.tar.gz" 2>/dev/null | wc -l)
    if [ "${gcno_count}" -lt 1 ]; then
        echo "### Fail! Cannot find any gcno files, please check."
        return 1
    fi

    gcda_count=$(find . -name "gcda*.tar.gz" 2>/dev/null | wc -l)
    if [ "${gcda_count}" -lt 1 ]; then
        echo "### Cannot find any gcda files, please check."
        return 0
    fi

    popd

}

# list and save the generated .gcno files
gcov_support_collect_gcno()
{
    local find_command
    local tar_command
    local submodule_name

    find . -name "gcno*.tar.gz" > tmp_gcno.txt
    while read -r LINE ; do
        rm -f "${LINE}"
    done < tmp_gcno.txt
    rm tmp_gcno.txt

    # rename .tmp*_gcno files generated
    tmp_gcno_files=$(find . -name ".tmp_*.gcno")
    while IFS= read -r tmp_gcno;
    do
        new_gcno=${tmp_gcno//".tmp_"/}
        echo "${new_gcno}"
        mv "${tmp_gcno}" "${new_gcno}"
    done <<< "$tmp_gcno_files"

    echo " === Start collecting .gcno files... === "
    submodule_name=$1
    exec 3>$GCNO_LIST_FILE
    find_command=$(find . -name "*.gcno" -o -name "*.gcda")
    echo "${find_command}"
    if [ -z "${find_command}" ]; then
        echo "### Error! no gcno files found!"
        return 1
    fi
    RESULT=${find_command}
    echo "$RESULT" >&3
    exec 3>&-
    
    local filesize
    filesize=$(ls -l $GCNO_LIST_FILE | awk '{print $5}')
    # Empty gcno_file_list indicates the non-gcov compling mode
    if [ "${filesize}" -le 1 ]; then
        echo "empty gcno_file_list.txt"
        rm $GCNO_LIST_FILE
    else
        echo " === Output archive file... === "
        tar_command="tar -T $GCNO_LIST_FILE -zcvf gcno_$submodule_name.tar.gz"
        echo "${tar_command}"
        ${tar_command}
        # temporarily using fixed dir
        cp gcno_"${submodule_name}".tar.gz "${work_dir}/debian/${submodule_name}/tmp/gcov"
        cp ./tests/gcov_support.sh "${work_dir}/debian/${submodule_name}/tmp/gcov"
        cp ./tests/gcov_support.sh "${work_dir}/tests"
        cp ./gcovpreload/lcov_cobertura.py "${work_dir}/debian/${submodule_name}/tmp/gcov"
        mkdir -p "${work_dir}/debian/${submodule_name}/usr"
        mkdir -p "${work_dir}/debian/${submodule_name}/usr/lib"
        cp ./gcovpreload/libgcovpreload.so "${work_dir}/debian/${submodule_name}/usr/lib"
        sudo chmod 777 -R "/${work_dir}/debian/${submodule_name}/usr/lib/libgcovpreload.so"
        rm $GCNO_LIST_FILE
        echo " === Collect finished... === "
    fi
}

main()
{
    case $1 in
        collect)
            gcov_support_collect_gcno "$2"
            ;;
        collect_gcda)
            gcov_support_collect_gcda
            ;;
        generate)
            generate_tracefiles
            ;;
        merge_container_info)
            lcov_merge_all "$2"
            ;;
        collect_container_gcda)
            collect_container_gcda "$2"
            ;;
        *)
            echo "Usage:"
            echo " collect                                  collect .gcno files based on module"
            echo " collect_gcda                             collect .gcda files"
            echo " generate                                 generate gcov report in html form (all or submodule_name)"
            echo " merge_container_info [source dir]        merge info files from different containers"
            echo "                                          - source_dir is the root directory of the source code"
            echo " collect_container_gcda [archive dir]     collect GCDA archives from existing docker containers"
            echo "                                          - archive_dir is the destination directory for GCDA archives" 
    esac
}

main "$1" "$2"
exit

