#ifndef SWSS_THRESHOLDORCH_H
#define SWSS_THRESHOLDORCH_H

#include <string>
#include <map>
#include <unordered_map>
#include "orch.h"
#include "observer.h"
#include "portsorch.h"
#include "tam.h"

#define CLEAR_NOTIFICATION 0

#define THRESHOLD_ORCH_SAI_TAM_TRANSPORT_SRC_PORT     7070 
#define THRESHOLD_ORCH_SAI_TAM_THRESHOLD_DST_PORT     9171
#define THRESHOLD_MIN                                 1 
#define THRESHOLD_MAX                                 100   

/* Threshold bind point . */
enum thresBindPoint_t
{
    THRES_BIND_POINT_PRIORITY_GROUP,
    THRES_BIND_POINT_QUEUE
};

/* Threshold event type */
enum thresEventType_t
{
    THRES_EVENT_TYPE_IPG_SHARED,
    THRES_EVENT_TYPE_IPG_HEADROOM,
    THRES_EVENT_TYPE_QUEUE_UNICAST,
    THRES_EVENT_TYPE_QUEUE_MULTICAST
};

/* Map to convert string to realm */
static const std::map<string, thresBindPoint_t> thresGroupString =
{
    { "priority-group", THRES_BIND_POINT_PRIORITY_GROUP,},
    { "queue", THRES_BIND_POINT_QUEUE}
};

/* Map to convert thres event type to bind point */
static const map<sai_tam_event_type_t, sai_tam_bind_point_type_t> bindPointType = 
{
    {SAI_TAM_EVENT_TYPE_IPG_SHARED, SAI_TAM_BIND_POINT_TYPE_IPG},
    {SAI_TAM_EVENT_TYPE_IPG_XOFF_ROOM, SAI_TAM_BIND_POINT_TYPE_IPG},
    {SAI_TAM_EVENT_TYPE_QUEUE_THRESHOLD, SAI_TAM_BIND_POINT_TYPE_QUEUE}
};

/* Threshold entry. */
struct thresEntry 
{
    thresEventType_t type;
    string alias;
    uint32_t index;

    bool operator<(const thresEntry &o) const
    {
        return tie(type, alias, index) < tie(o.type, o.alias, o.index);
    }

    bool operator==(const thresEntry &o) const
    {
        return ((type == o.type) && (alias == o.alias) && (index == o.index));
    }
};

/* TAM object entry. */
struct thresTamObjEntry
{
    uint32_t threshold;
    sai_tam_event_type_t type;

    bool operator<(const thresTamObjEntry &o) const
    {
        return tie(threshold, type) < tie(o.threshold, o.type);
    }

    bool operator==(const thresTamObjEntry &o) const
    {
        return (threshold == o.threshold) && (type == o.type);
    }
};

/* Bind list of TAM entry */
struct bind_list_t
{
    /* Using thresEvent type here to be able
     * to find out type of queue (unicast/multicast).
     */
    thresEventType_t type;
    string alias;
    uint32_t index;
};

/* TAM object data entry. */
struct thresTamObjData
{
    sai_object_id_t m_thresTamEvent;
    sai_object_id_t m_thresTam;

    vector<bind_list_t> bind_list;
};

/* threshold object table. */
struct thresTamThdObjData
{
    sai_object_id_t m_thresTamThd;
    uint32_t ref_cnt;
};

typedef map<thresEntry, uint32_t> thresTable; 
typedef map<thresTamObjEntry, thresTamObjData> tamObjTable;
typedef map<uint32_t, thresTamThdObjData> tamThdObjTable;

class ThresholdOrch : public Orch, public Observer, public Subject, public Tam
{
public:
    ThresholdOrch(DBConnector *db, vector<string> &tableNames, PortsOrch *port);
    ~ThresholdOrch();

    /* Orch infra to receive table updates and notifications. */
    void doTask(Consumer& consumer);
#if CLEAR_NOTIFICATION
    void doTask(NotificationConsumer& consumer);
#endif

    /* Threshold doTask */
    void doThresholdTask(Consumer& consumer);

    /* Handle updates from portsorch. */
    void update(SubjectType type, void *cntx);

    thresEventType_t getthresEventType(string thresGroup, string statType);

    /* threshold config APIs */
    bool addUpdateThresEntry(thresEventType_t type, string alias, uint32_t index, uint32_t threshold);
    bool removeThresEntry(thresEventType_t type, string alias, uint32_t index);

private:
    PortsOrch *m_portsOrch;

    /* TAM report object for proto. */
    sai_object_id_t m_tamReport;
    sai_object_id_t m_tamEventAction;
    sai_object_id_t m_tamTransport;
    sai_object_id_t m_tamCollector;

    /* thres realm tables. */
    thresTable m_thresTable;
    tamObjTable  m_thresTamTable;
    tamThdObjTable m_thresTamThdTable;

    bool addTamThdObjEntry(uint32_t threshold);
    bool deleteTamThdObjEntry(uint32_t threshold);
    bool addTamObjEntry(thresTamObjEntry tamObjEntry);
    bool deleteTamObjEntry(thresTamObjEntry tamObjEntry);
    bool thresTamObjBind(thresEventType_t thresType, string alias, uint32_t index,
                          sai_object_id_t tamObj);
    bool thresTamObjUnbind(thresEventType_t type, string alias, uint32_t index);
    sai_tam_event_type_t getTamEventType(thresEventType_t type);
    bool addBindTamEntry(thresEventType_t eventType, string alias, uint32_t index,
                                     uint32_t threshold);
    bool updateCfgThreshold(thresEventType_t eventType, string alias, uint32_t index,
                                        uint32_t cfgThreshold, uint32_t threshold);
};
#endif /* SWSS_THRESHOLDORCH_H */

