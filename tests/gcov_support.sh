#!/bin/bash
## This script is to enable the gcov support of the SONiC source codes
work_dir=$(pwd)

GCNO_LIST_FILE="gcno_file_list.txt"
TMP_GCDA_FILE_LIST="tmp_gcda_file_list.txt"
ALLMERGE_DIR=AllMergeReport
GCOV_OUTPUT=${work_dir}/gcov_output
HTML_FILE_PREFIX="GCOVHTML_"
INFO_FILE_PREFIX="GCOVINFO_"
MAX_PARALLEL_JOBS=8

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
    valid_line_count=$(grep -c "FN:" "${file}")

    if [ "$valid_line_count" -lt 1 ] ;then
        echo "${file}" >> info_err_list
        rm "${file}"
    fi
}

generate_single_tracefile()
{
    local gcda_dir=$1
    local output_file=${gcda_dir}/$2
    echo "Generating ${output_file}"
    lcov -c -d "${gcda_dir}" -o "${output_file}" &>/dev/null
    verify_info_file "${output_file}"
    echo "Done generating ${output_file}"
}

# generate gcov base info and html report for specified range files
generate_archive_tracefiles()
{
    local gcda_file_range=$1

    # find all directories that contain GCDA files
    local gcda_dir_list
    gcda_dir_list=$(find "${gcda_file_range}" -name "*.gcda" -exec "dirname" "{}" ";" | uniq)

    # for each directory containg GCDA files, use lcov to generate a tracefile
    while IFS= read -r line; do
        local fullpath=$line
        local infoname=${INFO_FILE_PREFIX}${fullpath##*/}.info

        GCDA_COUNT=$(find "${fullpath}" -name "*.gcda" | wc -l)
        if [ "$GCDA_COUNT" -ge 1 ]; then
            generate_single_tracefile "${fullpath}" "${infoname}" &
        fi
        # limit max # of parallel jobs to half the # of CPU cores
        [ "$( jobs | wc -l )" -ge "$MAX_PARALLEL_JOBS" ] && wait 
    done <<< "${gcda_dir_list}"
    wait
}

lcov_merge_all()
{
    info_files=$(find "${GCOV_OUTPUT}" -name "*.info")
    while IFS= read -r info_file; do
        if [ ! -f "total.info" ]; then
            lcov -o total.info -a "${info_file}"
        else
            lcov -o total.info -a total.info -a "${info_file}"
        fi
    done <<< "$info_files"

    # Remove unit test files and system libraries
    lcov -o total.info -r total.info "*tests/*"
    lcov -o total.info -r total.info "/usr/*"

    python lcov_cobertura.py total.info -o coverage.xml

    sed -i "s#\.\./s/##" coverage.xml
    sed -i "s#\.\.\.s\.##" coverage.xml

    pushd gcov_output/
    if [ ! -d ${ALLMERGE_DIR} ]; then
        mkdir -p ${ALLMERGE_DIR}
    fi

    cp ../coverage.xml ${ALLMERGE_DIR}
    popd
}

gcov_set_environment()
{
    local build_dir containers

    build_dir=$1
    mkdir -p "${build_dir}"/gcda_archives/sonic-gcov

    containers=$(docker ps -q)

    echo "### Start collecting info files from existed containers:"
    echo "${containers}"

    while IFS= read -r line; do
        local container_id=${line}
        gcda_count=$(docker exec "${container_id}" find / -name "gcda*.tar.gz" 2>/dev/null | wc -l)
        if [ "${gcda_count}" -gt 0 ]; then
            echo "Found ${gcda_count} gcda archives in container ${container_id}"
            mkdir -p "${build_dir}/gcda_archives/sonic-gcov/${container_id}"
            docker cp "${container_id}":/tmp/gcov/. "${build_dir}/gcda_archives/sonic-gcov/${container_id}/"
        else
            echo "No gcda archives found in container ${container_id}"
        fi
    done <<< "${containers}"
}

process_gcda_archive()
{
    local archive_path=$1
    echo "Generating tracefiles from ${archive_path}"

    local src_archive src_dir dst_dir
    src_archive=$(basename "${archive_path}")
    src_dir=$(dirname "${archive_path}")
    dst_dir=${src_archive//".tar.gz"/}

    # create a temp working directory for the GCDA archive so that GCDA files from other archives aren't overwritten
    mkdir -p "${dst_dir}"
    cp -r "${src_dir}/." "${dst_dir}"
    tar -C "${dst_dir}" -zxf "${dst_dir}/${src_archive}"

    generate_archive_tracefiles "${dst_dir}"
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
                tar -C "$(dirname "${gcno_archive}")" -zxf "${gcno_archive}"
            done <<< "$gcno_archives"
        fi

        gcda_archives=$(find . -name 'gcda*.tar.gz')
        if [ -z "${gcda_archives}" ]; then
            echo "No GCDA files found!"
        else
            echo "Found GCDA archives:"
            echo "${gcda_archives}"
            # process each GCDA archive in parallel. since each archive is extracted to a separate directory, there is no risk of overwriting files
            while IFS= read -r gcda_archive; do
                process_gcda_archive "${gcda_archive}" &
                # limit max # of parallel jobs to half the # of CPU cores
                [ "$( jobs | wc -l )" -ge "$MAX_PARALLEL_JOBS" ] && wait 
            done <<< "$gcda_archives"
            wait
        fi

        mkdir -p "gcov_output/${container_id}"
        find "${container_id}" -name "*.info" -exec "cp" "-r" "--parents" "{}" "gcov_output/" ";"

        rm -rf "${container_id}"
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
            lcov_merge_all
            ;;
        set_environment)
            gcov_set_environment "$2"
            ;;
        *)
            echo "Usage:"
            echo " collect               collect .gcno files based on module"
            echo " collect_gcda          collect .gcda files"
            echo " generate              generate gcov report in html form (all or submodule_name)"
            echo " merge_container_info  merge homonymic info files from different container"
            echo " set_environment       set environment ready for report generating in containers"
    esac
}

main "$1" "$2"
exit

