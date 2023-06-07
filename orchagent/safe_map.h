#ifndef SAFE_MAP_H_
#define SAFE_MAP_H_

#include <map>
#include <mutex>
 
template <typename K, typename V>
class SafeMap {
public:
    SafeMap() {}
 
    ~SafeMap() {}
 
    SafeMap(const SafeMap& rhs)
    {
        map_ = rhs.map_;
    }

    SafeMap& operator=(const SafeMap& rhs)
    {
        if (&rhs != this) {
            map_ = rhs.map_;
        }
 
        return *this;
    }
 
    V& operator[](const K& key)
    {
        return map_[key];
    }
 
    // when multithread calling size() return a tmp status, some threads may insert just after size() call
    int Size()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return map_.size();
    }
 
    // when multithread calling Empty() return a tmp status, some threads may insert just after Empty() call
    bool IsEmpty()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return map_.empty();
    }
 
    bool Insert(const K& key, const V& value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto ret = map_.insert(std::pair<K, V>(key, value));
        return ret.second;
    }
 
    void EnsureInsert(const K& key, const V& value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto ret = map_.insert(std::pair<K, V>(key, value));
        // find key and cannot insert
        if (!ret.second) {
            map_.erase(ret.first);
            map_.insert(std::pair<K, V>(key, value));
            return;
        }
        return;
    }
 
    bool Find(const K& key/*, V& value*/)
    {
        bool ret = false;
        std::lock_guard<std::mutex> lock(mutex_);
 
        auto iter = map_.find(key);
        if (iter != map_.end()) {
            //value = iter->second;
            ret = true;
        }
 
        return ret;
    }

    bool Find(const K& key, V& value)
    {
        bool ret = false;
        std::lock_guard<std::mutex> lock(mutex_);
 
        auto iter = map_.find(key);
        if (iter != map_.end()) {
            value = iter->second;
            ret = true;
        }
 
        return ret;
    }
 
    bool FindOldAndSetNew(const K& key, V& oldValue, const V& newValue)
    {
        bool ret = false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (map_.size() > 0) {
            auto iter = map_.find(key);
            if (iter != map_.end()) {
                oldValue = iter->second;
                map_.erase(iter);
                map_.insert(std::pair<K, V>(key, newValue));
                ret = true;
            }
        }
 
        return ret;
    }

    void Erase(const K& key)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        map_.erase(key);
    }
 
    void Clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        map_.clear();
        return;
    }

    // get map
    std::map<K, V>& GetMap() 
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return map_;
    }
 
private:
    std::mutex mutex_;
    std::map<K, V> map_;
};

#endif //SAFE_MAP_H_
