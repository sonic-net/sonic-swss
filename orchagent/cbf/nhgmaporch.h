#pragma once

#include "orch.h"
#include "dbconnector.h"
#include <unordered_map>

using namespace std;

/*
 * Structure describing a NHG map entry in the NHG map map.
 */
struct NhgMapEntry
{
    /* The next hop group map SAI ID. */
    sai_object_id_t id;

    /* Number of external objects referencing this next hop group map. */
    uint32_t ref_count;

    /* The largest group index referenced by the map. */
    int largest_nh_index;

    explicit NhgMapEntry(sai_object_id_t id, uint32_t _ref_count, int _largest_nh_index);
};

class NhgMapOrch : public Orch
{
public:
    NhgMapOrch(swss::DBConnector *db, const string &table_name);

    void doTask(Consumer &consumer) override;

    /*
     * Return the SAI ID for the map indexed by "index".  If it  does not exist, return SAI_NULL_OBJECT_ID.
    */
    sai_object_id_t getMapId(const string &key) const;
    int getLargestNhIndex(const string &key) const;

    /*
     * Increase / Decrease reference counter for a map.
     */
    void incRefCount(const string &key);
    void decRefCount(const string &key);

    /*
     * Get the maximum number of FC classes supported by the switch.
     */
    static sai_uint8_t getMaxNumFcs();

private:
    /*
     * Map of synced NHG maps over SAI.
     */
    unordered_map<string, NhgMapEntry> m_syncdMaps;

    /*
     * Maximum number of NHG maps supported by the switch.
     */
    static uint64_t m_max_nhg_map_count;

    /*
     * Extract the NHG map from the FV tuples
    */
    static pair<bool, unordered_map<sai_uint32_t, sai_int32_t>> getMap(const vector<swss::FieldValueTuple> &values);
};
