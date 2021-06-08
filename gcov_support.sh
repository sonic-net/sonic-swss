#!/bin/bash
## This script is to enable the gcov support of the SONiC source codes
work_dir=$(pwd)
env_home=$HOME

GCNO_LIST_FILE="gcno_file_list.txt"
GCDA_DIR_LIST="gcda_dir_list.txt"
TMP_GCDA_FILE_LIST="tmp_gcda_file_list.txt"
GCNO_ALL_TAR_GZ="gcno.tar.gz"

INFO_DIR=info
HTML_DIR=html
ALLMERGE_DIR=AllMergeReport

GCOV_OUTPUT=${work_dir}/gcov_output
GCOV_INFO_OUTPUT=${GCOV_OUTPUT}/${INFO_DIR}
GCOV_HTML_OUTPUT=${GCOV_OUTPUT}/${HTML_DIR}
GCOV_MERGE_REPORT_OUTPUT=${GCOV_OUTPUT}/${ALLMERGE_DIR}

HTML_FILE_PREFIX="GCOVHTML_"
HTML_FILE_LIST=${GCOV_OUTPUT}/htmllist
INFO_FILE_PREFIX="GCOVINFO_"
INFO_FILE_LIST=${GCOV_OUTPUT}/infolist
INFO_ERR_LIST=${work_dir}/info_err_list
CONTAINER_LIST=${work_dir}/containerlist
ALL_INFO_FILES=${work_dir}/allinfofileslist
REPE_INFO_LIST=${work_dir}/tmpinfolist
SINGLE_INFO_LIST=${work_dir}/singleinfolist

# change the env_var $HOME to a temporary one to ensure genhtml's reliability
add_new_home_for_sonic()
{
    local new_home=${work_dir}

    export HOME=${new_home}
    echo "new_home is $HOME"
}

# reset the env_var $HOME
reset_home()
{
    export HOME=${env_home}
    echo "reset to original home: $HOME"
}

# reset compiling environment
gcov_support_clean()
{
    find /tmp/gcov -name $INFO_FILE_PREFIX* | xargs rm -rf
    find /tmp/gcov -name $HTML_FILE_PREFIX* | xargs rm -rf
    find /tmp/gcov -name *.gcno | xargs rm -rf
    find /tmp/gcov -name *.gcda | xargs rm -rf
    find /tmp/gcov -name $TMP_GCDA_FILE_LIST | xargs rm -rf
    #rm $INFO_ERR_LIST
    rm /tmp/gcov/info_err_list
    #rm $GCDA_DIR_LIST
    rm /tmp/gcov/gcda_dir_list.txt
}

# verify whether the info file generated is valid
verify_info_file()
{
    local file=$1
    local path=$2
    local FILE_OK=`grep "FN:" ${file} | wc -l`
    if [ $FILE_OK -lt 1 ] ;then
        #echo ${path}/${file} >> ${INFO_ERR_LIST}
        echo ${path}/${file} >> /tmp/gcov/info_err_list
        rm ${file}
    fi
}

# search and save the dir where the lcov should be implemented
list_lcov_path()
{
    local find_gcda_file
    local gcda_dir=$1

    echo ${gcda_dir}

    TMP_FILE=${gcda_dir}/tmpgcdalist
    echo "Start searching .gcda files..."
    exec 4>$TMP_FILE
    find_gcda_file=`find ${gcda_dir} -name *.gcda`

    echo ${find_gcda_file}
    RESULT=${find_gcda_file}
    echo "$RESULT" >&4
    exec 4>&-
    # save the dirnames where the .gcda files exist
    #cat ${TMP_FILE} | xargs dirname | uniq > ${GCDA_DIR_LIST}
    cat ${TMP_FILE} | xargs dirname | uniq > ${gcda_dir}/gcda_dir_list.txt
#    rm ${TMP_FILE}
}

# generate gcov base info and html report for specified range files
lcov_genhtml_report()
{
    local gcda_file_range=$1
    list_lcov_path ${gcda_file_range}

    #cat ${gcda_file_range}/gcda_dir_list.txt
    while read line
    do
        local fullpath=$line
        local infoname=${INFO_FILE_PREFIX}${fullpath##*/}.info
        htmldirname=${HTML_FILE_PREFIX}${fullpath##*/}
        
        echo ${fullpath}

        pushd ${fullpath}
        GCDA_COUNT=`find -name "*.gcda" | wc -l`
        echo "gcda count: $GCDA_COUNT"
        if [ $GCDA_COUNT -ge 1 ]; then
            echo "Executing lcov -c -d . -o ${infoname}"
            lcov -c -d . -o ${infoname}
            if [ "$?" != "0" ]; then
                echo "lcov fail!"
                rm ${infoname}
            fi
            #verify_info_file ${infoname} ${fullpath}
        fi

        # generate html report
        local SOURCE_CODE_COUNT=`find . -name "*\.[c|cpp]" | wc -l`
        if [ $SOURCE_CODE_COUNT -lt 1 ]; then
            genhtml -o ${htmldirname} -t ${fullpath##*/} --no-source *.info
        else
            echo "Executing genhtml..."
            genhtml -o ${htmldirname} -t ${fullpath##*/} *.info
        fi

        popd
    done < ${gcda_file_range}/gcda_dir_list.txt

    #cd ${gcda_file_range}
}

# generate html reports for all eligible submodules
lcov_genhtml_all()
{
    local container_id

    container_id=$1

    echo " === Start generating all gcov reports === "
    lcov_genhtml_report ${container_id}/gcov
} 

lcov_merge_all()
{
    local project_c_source
    local all_info_files

    # check c/cpp source files
    project_c_source=`find / -name "*\.[c|cpp]" 2>/dev/null | wc -l`
    
    cp -rf common_work $1
    cd gcov_output/
    if [ ! -d ${ALLMERGE_DIR} ]; then
        mkdir -p ${ALLMERGE_DIR}
    fi

    all_info_files=`find . -name *\.info`

    if [ ${project_c_source} -lt 1 ]; then
        echo "############# build reports without sources ###############"
        genhtml -o $ALLMERGE_DIR --no-source ${all_info_files}
    else
        echo "############# build reports with sources ##################"
        genhtml -o $ALLMERGE_DIR ${all_info_files}
    fi

    find . -name *.info > infolist
    while read line
    do
        if [ ! -f "total.info" ]; then
            lcov -o total.info -a ${line}
        else
            lcov -o total.info -a total.info -a ${line}
        fi
    done < infolist

    lcov --extract total.info '*sonic-gcov/*' -o total.info
    python $1/common_work/gcov/lcov_cobertura.py total.info -o coverage.xml

    sed -i "s#../common_work#$1/common_work#" coverage.xml
    cp coverage.xml ${ALLMERGE_DIR}

    cd ../
}

get_info_file()
{
    local container_id

    container_id=$1
    echo "### Start collecting info files generated by lcov"
    #find -name "${INFO_FILE_PREFIX}*" > ${INFO_FILE_LIST}
    pushd ${container_id}
    mkdir -p gcov/gcov_output
    find -name "${INFO_FILE_PREFIX}*" > gcov/gcov_output/infolist

    while read line
    do
        local info_file=${line}
        local FromFullpath=${line%/*}
        local ToFullpath

        if [ ! -d gcov/gcov_output/${INFO_DIR} ]; then
            mkdir -p gcov/gcov_output/${INFO_DIR}
        fi
        pushd gcov/gcov_output/${INFO_DIR}
        mkdir -p ${FromFullpath}
        #mkdir -p ${info_file}
        popd
        #ToFullpath=/tmp/gcov/${INFO_DIR}/${FromFullpath#*/}
        ToFullpath=gcov/gcov_output/${INFO_DIR}/${info_file}
        #ToFullpath=gcov_output/${INFO_DIR}/${info_file}
        cp -r ${FromFullpath} ${ToFullpath}
    done < gcov/gcov_output/infolist

    popd
}

get_html_file()
{
    local container_id

    container_id=$1
    echo "### Start collecting html files generated by genhtml"
    pushd ${container_id}
    #find -name "${HTML_FILE_PREFIX}*" > ${HTML_FILE_LIST}
    find -name "${HTML_FILE_PREFIX}*" > gcov/gcov_output/htmllist

    while read line
    do
        local html_report=${line}
        local FromFullpath=${line%/*}
        local ToFullpath

        if [ ! -d gcov/gcov_output/${HTML_DIR} ]; then
            mkdir -p gcov/gcov_output/${HTML_DIR}
        fi
        pushd gcov/gcov_output/${HTML_DIR}
        #mkdir -p ${FromFullpath}
        mkdir -p ${html_report}
        popd
        ToFullpath=gcov/gcov_output/${HTML_DIR}/${html_report}
        cp -r ${html_report} ${ToFullpath}
    done < gcov/gcov_output/htmllist

    popd
}

gcov_set_environment()
{
    local build_dir

    build_dir=$1
    #echo "docker kill begin"
    #docker kill --signal "TERM" $(docker ps -q -a)
    #echo "docker kill done"
    #sleep 30
    #echo "sleep done"
    #docker start $(docker ps -q -a)
    #echo "docker start done"

    mkdir -p ${build_dir}/gcov_tmp
    mkdir -p ${build_dir}/gcov_tmp/sonic-gcov
    # mkdir -p ${build_dir}/gcov_tmp/gcov_output
    # mkdir -p ${build_dir}/gcov_tmp/gcov_output/info

    docker ps -q > ${CONTAINER_LIST}

    echo "### Start collecting info files from existed containers"

    for line in $(cat ${CONTAINER_LIST})
    do
        local container_id=${line}
        echo ${container_id}
        echo "script_count"
        script_count=`docker exec -i ${container_id} find / -name gcov_support.sh | wc -l`
        echo ${script_count}
        if [ ${script_count} -gt 0 ]; then
            #docker exec -i ${container_id} chmod 777 /etc/ld.so.conf.d/libgcov_preload.so
            #docker exec -i ${container_id} export LD_PRELOAD="/etc/ld.so.conf.d/libgcov_preload.so"
            docker exec -i ${container_id} killall5 -15
            docker exec -i ${container_id} /tmp/gcov/gcov_support.sh collect_gcda
        fi
        gcda_count=`docker exec -i ${container_id} find / -name *.gcda | wc -l`
        if [ ${gcda_count} -gt 0 ]; then
            echo "find gcda in "
            echo ${container_id}
            mkdir -p ${build_dir}/gcov_tmp/sonic-gcov/${container_id}
            pushd ${build_dir}/gcov_tmp/sonic-gcov/${container_id}
            docker cp ${container_id}:/tmp/gcov/ .
            cp gcov/gcov_support.sh ${build_dir}/gcov_tmp/sonic-gcov
            cp gcov/lcov_cobertura.py ${build_dir}/gcov_tmp/sonic-gcov
            # gcov/gcov_support.sh generate ${build_dir}/gcov_tmp/${container_id}
            popd
        fi
    done

    echo "cat list"
    cat ${CONTAINER_LIST}

    cd ${build_dir}/gcov_tmp/
    tar -zcvf sonic-gcov.tar.gz sonic-gcov/
    rm -rf sonic-gcov
    cd ../../
    rm ${CONTAINER_LIST}
}

gcov_merge_info()
{
    rm -rf gcov_output
    mkdir -p gcov_output
    mkdir -p gcov_output/info

    touch tmpinfolist
    find -name *.info > allinfofileslist

    while read line
    do
        local info_file_name=${line##*/}
        echo ${info_file_name}
        local info_repeat_times=`grep ${info_file_name} allinfofileslist | wc -l`
        if [ ${info_repeat_times} -gt 1 ]; then
            grep ${info_file_name} tmpinfolist > /dev/null
            if [ $? -ne 0 ]; then
                echo ${info_file_name} >> tmpinfolist
            fi
        fi
    done < allinfofileslist

    #echo ${REPE_INFO_LIST} | while read line
    echo tmpinfolist | while read line
    do
        local info_name=${line%.*}
        #find -name ${info_name} > ${SINGLE_INFO_LIST}
        echo "singleinfolistelement:"
        echo ${info_name}
        find -name "${info_name}*" > singleinfolist
        while read line
        do
            if [ ! -f "total_${info_name}.info" ]; then
                lcov -o total_${info_name}.info -a ${line}
            else
                lcov -o total_${info_name}.info -a total_${info_name}.info -a ${line}
            fi
            mv total_${info_name}.info gcov_output/info/
        done < singleinfolist
    done < tmpinfolist

    echo allinfofileslist | while read line
    do
        cp -r ${line} gcov_output/info/
    done < allinfofileslist

    lcov_merge_all $1
    tar_gcov_output
}

tar_gcov_output()
{
    local time_stamp

    time_stamp=$(date "+%Y%m%d%H%M")
    tar -czvf ${time_stamp}_SONiC_gcov_report.tar.gz gcov_output
}

collect_merged_report()
{
    local container_id

    container_id=$1

    get_info_file ${container_id}
    get_html_file ${container_id}
}

gcov_support_generate_report()
{
    ls -F | grep "/$" > container_dir_list
    sed -i '/gcov_output/d' container_dir_list
    sed -i "s#\/##g" container_dir_list

    #for same code path
    mkdir -p common_work

    cat container_dir_list
    while read line
    do
        local container_id=${line}
        echo ${container_id}
        cp -rf ${container_id}/* common_work
        #tar -zxvf swss.tar.gz -C ${container_id}
        tar -zxvf swss.tar.gz -C common_work/gcov

	    #lcov_genhtml_all ${container_id}
        ls -lh common_work/*
        lcov_genhtml_all common_work
        if [ "$?" != "0" ]; then
            echo "###lcov operation fail.."
            return 0
        fi

        cp -rf common_work/*  ${container_id}/*

        rm -rf common_work/*
        
        # collect gcov output
        # collect_merged_report ${container_id}
    done < container_dir_list

    # generate report with code
    mkdir -p common_work/gcov
    tar -zxvf swss.tar.gz -C common_work/gcov

    echo "### Make info generating completed !!"
}

# list and save the generated .gcda files
gcov_support_collect_gcda()
{
    echo "gcov_support_collect_gcda begin"
    local gcda_files_count
    local gcda_count

    pushd /
    # check whether .gcda files exist
    gcda_files_count=`find \. -name "*\.gcda" 2>/dev/null | wc -l`
    if [ ${gcda_files_count} -lt 1 ]; then
        echo "### no gcda files found!"
        return 0
    fi

    #CODE_PREFFIX=`find -name "*.gcda" | head -1 | cut -d "/" -f2`
    CODE_PREFFIX=/__w/1/s/

    pushd ${CODE_PREFFIX}
    tar -zcvf /tmp/gcov/gcda.tar.gz *
    popd

    popd
    echo "### collect gcda done!"

    gcov_support_clean

    #add_new_home_for_sonic
    pushd /tmp/gcov
    gcno_count=`find -name gcno*.tar.gz 2>/dev/null | wc -l`
    if [ ${gcno_count} -lt 1 ]; then
        echo "### Fail! Cannot find any gcno files, please check."
        return -1
    fi

    gcda_count=`find -name gcda*.tar.gz 2>/dev/null | wc -l`
    if [ ${gcda_count} -lt 1 ]; then
        echo "### Cannot find any gcda files, please check."
        return 0
    fi

    # echo "### Extract .gcda and .gcno files..."
    # tar -zxvf $GCNO_ALL_TAR_GZ
    find -name gcda*.tar.gz > tmp_gcda.txt
    while read LINE ; do
        echo ${LINE}
        echo ${LINE#*.}
        #tar -zxvf ${LINE#*.}
        tar -zxvf ${LINE}
    done < tmp_gcda.txt
    rm tmp_gcda.txt

    find -name gcno*.tar.gz > tmp_gcno.txt
    while read LINE ; do
        echo ${LINE}
        echo ${LINE%%.*}
        #submodule_name=`echo ${LINE%%.*} | awk -F _ '{print $2}' | awk -F . '{print $1}'`
        #cp ${LINE#*/} /tmp/gcov/src/sonic-${submodule_name}
        #pushd /tmp/gcov/src/sonic-${submodule_name}/
        #tar -zxvf ${LINE#*.}
        tar -zxvf ${LINE}
        #popd
    done < tmp_gcno.txt
    rm tmp_gcno.txt

    #popd

    # remove old output dir
    #rm -rf ${work_dir}/gcov_output
    rm -rf /tmp/gcov/gcov_output
    #mkdir -p $GCOV_OUTPUT
    mkdir -p /tmp/gcov/gcov_output

    #lcov_genhtml_all
    # if [ "$?" != "0" ]; then
    #     echo "###lcov operation fail.."
    #     return -1
    # fi

    # collect gcov output
    #collect_merged_report
    #reset_home
    echo "### Make /tmp/gcov dir completed !!"
    popd

}

# list and save the generated .gcno files
gcov_support_collect_gcno()
{
    local find_command
    local tar_command
    local submodule_name

    find gcno*.tar.gz > tmp_gcno.txt
    while read LINE ; do
        rm -f ${LINE}
    done < tmp_gcno.txt
    rm tmp_gcno.txt

    # rename .tmp*_gcno files generated
    for tmp_gcno in `find -name .tmp_*.gcno`
    do
        new_gcno=`echo ${tmp_gcno} | sed 's/.tmp_//g'`
        echo ${new_gcno}
        mv ${tmp_gcno} ${new_gcno}
    done

    echo " === Start collecting .gcno files... === "
    submodule_name=$1
    exec 3>$GCNO_LIST_FILE
    find_command=`find -name *.gcno`
    echo "${find_command}"
    if [ -z "${find_command}" ]; then
        echo "### Error! no gcno files found!"
        return -1
    fi
    RESULT=${find_command}
    echo "$RESULT" >&3
    exec 3>&-
    
    local filesize=`ls -l $GCNO_LIST_FILE | awk '{print $5}'`
    # Empty gcno_file_list indicates the non-gcov compling mode
    if [ ${filesize} -le 1 ]; then
        echo "empty gcno_file_list.txt"
        rm $GCNO_LIST_FILE
    else
        echo " === Output archive file... === "
        tar_command="tar -T $GCNO_LIST_FILE -zcvf gcno_$submodule_name.tar.gz"
        echo "${tar_command}"
        ${tar_command}
        # temporarily using fixed dir
        cp gcno_$submodule_name.tar.gz ${work_dir}/debian/$submodule_name/tmp/gcov
        cp gcov_support.sh ${work_dir}/debian/$submodule_name/tmp/gcov
        cp lcov_cobertura.py ${work_dir}/debian/$submodule_name/tmp/gcov
        mkdir -p ${work_dir}/debian/$submodule_name/usr
        mkdir -p ${work_dir}/debian/$submodule_name/usr/lib
        cp libgcov_preload.so ${work_dir}/debian/$submodule_name/usr/lib/
        rm $GCNO_LIST_FILE
        echo " === Collect finished... === "
    fi
}

main()
{
    case $1 in
        collect)
            gcov_support_collect_gcno $2
            ;;
        collect_gcda)
            gcov_support_collect_gcda
            ;;
        generate)
            gcov_support_generate_report
            ;;
        tar_output)
            tar_gcov_output
            ;;
        merge_container_info)
            gcov_merge_info $2
            ;;
        set_environment)
            gcov_set_environment $2
            ;;
        *)
            echo "Usage:"
            echo " collect               collect .gcno files based on module"
            echo " collect_gcda          collect .gcda files"
            echo " collect_gcda_files    collect .gcda files in a docker"
            echo " generate              generate gcov report in html form (all or submodule_name)"
            echo " tar_output            tar gcov_output forder"
            echo " merge_container_info  merge homonymic info files from different container"
            echo " set_environment       set environment ready for report generating in containers"
    esac
}

main $1 $2
exit

