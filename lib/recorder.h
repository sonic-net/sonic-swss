#pragma once

#include <string>
#include <fstream>
#include <iostream>
#include <sstream>
#include <memory>

namespace swss {

class RecBase {
public:
    RecBase() = default;
    /* Setters */
    void setRecord(bool record)  { m_recording = record; }
    void setRotate(bool rotate)  { m_rotate = rotate; }
    void setLocation(const std::string& loc) { m_location = loc; }
    void setFileName(const std::string& name) { m_filename = name; }
    void setName(const std::string& name)  { m_name = name; }

    /* getters */
    bool isRecord()  { return m_recording; }
    bool isRotate()  { return m_rotate; }
    std::string getLoc() { return m_location; }
    std::string getFile() { return m_filename; }
    std::string getName() { return m_name; }

private:
    bool m_recording;
    bool m_rotate;
    std::string m_location;
    std::string m_filename;
    std::string m_name;
};

class RecWriter : public RecBase {
public:
    RecWriter() = default;
    virtual ~RecWriter();
    void startRec(bool exit_if_failure);
    void record(const std::string& val);

protected:
    void logfileReopen();

private:
    std::ofstream record_ofs;
    std::string fname;
};

class RetryRec : public RecWriter {
public:
    RetryRec();
};

class SwSSRec : public RecWriter {
public:
    SwSSRec();
};

/* Record Handler for Response Publisher Class */
class ResPubRec : public RecWriter {
public:
    ResPubRec();
};

class SaiRedisRec : public RecBase {
public:
    SaiRedisRec();
};

/* Interface to access recorder classes */
class Recorder {
public:
    static Recorder& Instance();
    static const std::string DEFAULT_DIR;
    static const std::string REC_START;
    static const std::string SWSS_FNAME;
    static const std::string SAIREDIS_FNAME;
    static const std::string RESPPUB_FNAME;
    static const std::string RETRY_FNAME;

    Recorder() = default;
    /* Individual Handlers */
    SwSSRec swss;
    SaiRedisRec sairedis;
    ResPubRec respub;
    RetryRec retry;
};

}
