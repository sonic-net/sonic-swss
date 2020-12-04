#ifndef SWSS_OBSERVER_H
#define SWSS_OBSERVER_H

#include <list>
#include <algorithm>

using namespace std;
using namespace swss;

enum SubjectType
{
    SUBJECT_TYPE_ALL_CHANGES,
    SUBJECT_TYPE_NEXTHOP_CHANGE,
    SUBJECT_TYPE_NEIGH_CHANGE,
    SUBJECT_TYPE_FDB_CHANGE,
    SUBJECT_TYPE_LAG_MEMBER_CHANGE,
    SUBJECT_TYPE_VLAN_MEMBER_CHANGE,
    SUBJECT_TYPE_MIRROR_SESSION_CHANGE,
    SUBJECT_TYPE_INT_SESSION_CHANGE,
    SUBJECT_TYPE_PORT_CHANGE,
    SUBJECT_TYPE_BRIDGE_PORT_CHANGE,
    SUBJECT_TYPE_PORT_OPER_STATE_CHANGE,
    SUBJECT_TYPE_ISOLATION_GROUP_CHANGE,
    SUBJECT_TYPE_ISOLATION_GROUP_MEMBER_CHANGE,
    SUBJECT_TYPE_ISOLATION_GROUP_BINDING_CHANGE,
    SUBJECT_TYPE_MLAG_INTF_CHANGE,
    SUBJECT_TYPE_MLAG_ISL_CHANGE,
    SUBJECT_TYPE_FDB_FLUSH_CHANGE
};

class Observer
{
public:
    virtual void update(SubjectType, void *) = 0;
    virtual ~Observer() {}
};

class Subject
{
public:
    virtual void attach(SubjectType type, Observer *observer)
    {
        m_observers.emplace_back(type, observer);
    }

    virtual void attach(Observer *observer)
    {
        attach(SUBJECT_TYPE_ALL_CHANGES, observer);
    }

    virtual void detach(SubjectType type, Observer *observer)
    {
        pair<SubjectType, Observer *> temp(type, observer);

        auto it = find(m_observers.begin(), m_observers.end(), temp);
        if (it != m_observers.end())
        {
            m_observers.erase(it);
        }
    }

    virtual void detach(Observer *observer)
    {
        detach(SUBJECT_TYPE_ALL_CHANGES, observer);
    }

    virtual bool isObserver(SubjectType type, Observer *observer)
    {
        pair<SubjectType, Observer *> temp(type, observer);

        return m_observers.end() != find(m_observers.begin(), m_observers.end(), temp);
    }

    virtual bool isObserver(Observer *observer)
    {
        return isObserver(SUBJECT_TYPE_ALL_CHANGES, observer);
    }

    virtual bool hasObservers()
    {
        return m_observers.size() > 0;
    }

    virtual ~Subject() {}

protected:
    list<pair<SubjectType, Observer *>> m_observers;

    virtual void notify(SubjectType type, void *cntx)
    {
        for (auto &iter: m_observers)
        {
            if ((iter.first == SUBJECT_TYPE_ALL_CHANGES) || (iter.first == type))
            {
                iter.second->update(type, cntx);
            }
        }
    }
};

#endif /* SWSS_OBSERVER_H */
