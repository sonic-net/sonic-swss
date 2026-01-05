#include "recorder.h"
#include "timestamp.h"
#include "logger.h"
#include <cstring>

using namespace swss;

const std::string Recorder::DEFAULT_DIR = ".";
const std::string Recorder::REC_START = "|recording started";
const std::string Recorder::SWSS_FNAME = "swss.rec";
const std::string Recorder::SAIREDIS_FNAME = "sairedis.rec";
const std::string Recorder::RESPPUB_FNAME = "responsepublisher.rec";
const std::string Recorder::RETRY_FNAME = "retry.rec";

Recorder& Recorder::Instance()
{
    static Recorder m_recorder;
    return m_recorder;
}


RetryRec::RetryRec() 
{
    /* Set Default values */
    setRecord(true);
    setRotate(false);
    setLocation(Recorder::DEFAULT_DIR);
    setFileName(Recorder::RETRY_FNAME);
    setName("Retry");
}


SwSSRec::SwSSRec() 
{
    /* Set Default values */
    setRecord(true);
    setRotate(false);
    setLocation(Recorder::DEFAULT_DIR);
    setFileName(Recorder::SWSS_FNAME);
    setName("SwSS");
}


ResPubRec::ResPubRec() 
{
    /* Set Default values */
    setRecord(false);
    setRotate(false);
    setLocation(Recorder::DEFAULT_DIR);
    setFileName(Recorder::RESPPUB_FNAME);
    setName("Response Publisher");
}


SaiRedisRec::SaiRedisRec() 
{
    /* Set Default values */
    setRecord(true);
    setRotate(false);
    setLocation(Recorder::DEFAULT_DIR);
    setFileName(Recorder::SAIREDIS_FNAME);
    setName("SaiRedis");
}


void RecWriter::startRec(bool exit_if_failure)
{
    if (!isRecord())
    {
        return ;
    }

    fname = getLoc() + "/" + getFile();
    record_ofs.open(fname, std::ofstream::out | std::ofstream::app);
    if (!record_ofs.is_open())
    {
        SWSS_LOG_ERROR("%s Recorder: Failed to open recording file %s: error %s", getName().c_str(), fname.c_str(), strerror(errno));
        if (exit_if_failure)
        {
            exit(EXIT_FAILURE);
        }
        else
        {
            setRecord(false);
        }
    }
    record_ofs << swss::getTimestamp() << Recorder::REC_START << std::endl;
    SWSS_LOG_NOTICE("%s Recorder: Recording started at %s", getName().c_str(), fname.c_str());
}


RecWriter::~RecWriter()
{
    if (record_ofs.is_open())
    {
        record_ofs.close();      
    }
}


void RecWriter::record(const std::string& val)
{
    if (!isRecord())
    {
        return ;
    }
    if (isRotate())
    {
        setRotate(false);
        logfileReopen();
    }
    record_ofs << swss::getTimestamp() << "|" << val << std::endl;
}


void RecWriter::logfileReopen()
{
    /*
     * On log rotate we will use the same file name, we are assuming that
     * logrotate daemon move filename to filename.1 and we will create new
     * empty file here.
     */
    record_ofs.close();
    record_ofs.open(fname, std::ofstream::out | std::ofstream::app);

    if (!record_ofs.is_open())
    {
        SWSS_LOG_ERROR("%s Recorder: Failed to open file %s: %s", getName().c_str(), fname.c_str(), strerror(errno));
        return;
    }
    SWSS_LOG_INFO("%s Recorder: LogRotate request handled", getName().c_str());
}
