#pragma once

#include <functional>
#include <memory>

#include "saitypes.h"

// Spy C functin pointer to std::function to access closure
// Internal using static `spy` function pointer to invoke std::function `fake`
// To make sure the convert work for multiple function in the same or different API table.
// The caller shall passing <n/objtype> to create unique SaiSpyFunction class.
//
// Almost use cases will like the follow, pass n=0 and objecttype/offset to use.
//     auto x = SpyOn<0, SAI_OBJECT_TYPE_ACL_TABLE>(&acl_api_1.get()->create_acl_table);
//     auto y = SpyOn<0, SAI_OBJECT_TYPE_ACL_ENTRY>(&acl_api_1.get()->create_acl_entry);
// or
//     auto x = SpyOn<0, offsetof(sai_acl_api_t, create_acl_table)>(&acl_api.get()->create_acl_table);
//     auto x = SpyOn<0, offsetof(sai_acl_api_t, create_acl_entry)>(&acl_api.get()->create_acl_entry);
//
// or pass n=api_api for different API table
//    auto x = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_TABLE>(&acl_api.get()->create_acl_table);
//    auto y = SpyOn<SAI_API_SWITCH, SAI_OBJECT_TYPE_SWITCH>(&switch_api.get()->create_switch);
//
// The rest rare case is spy same function in different API table. Using differnt n value for that.
//     auto x = SpyOn<0, SAI_OBJECT_TYPE_ACL_TABLE>(&acl_api_1.get()->create_acl_table);
//     auto y = SpyOn<1, SAI_OBJECT_TYPE_ACL_TABLE>(&acl_api_2.get()->create_acl_table);
//
template <int n, int objtype, typename R, typename... arglist>
struct SaiSpyFunctor
{

    using original_fn_t = R (*)(arglist...);
    using original_fn_ptr_t = R (**)(arglist...);

    original_fn_t original_fn;
    static std::function<R(arglist...)> fake;

    SaiSpyFunctor(original_fn_ptr_t fn_ptr) :
        original_fn(*fn_ptr)
    {
        *fn_ptr = spy;
    }

    void callFake(std::function<sai_status_t(arglist...)> fn)
    {
        fake = fn;
    }

    static sai_status_t spy(arglist... args)
    {
        // TODO: pass this into fake. Inside fake() it can call original_fn
        return fake(args...);
    }
};

template <int n, int objtype, typename R, typename... arglist>
std::function<R(arglist...)> SaiSpyFunctor<n, objtype, R, arglist...>::fake;

// create entry
template <int n, int objtype>
std::shared_ptr<SaiSpyFunctor<n, objtype, sai_status_t, sai_object_id_t *, sai_object_id_t, uint32_t, const sai_attribute_t *>>
    SpyOn(sai_status_t (**fn_ptr)(sai_object_id_t *, sai_object_id_t, uint32_t, const sai_attribute_t *))
{
    using SaiSpyCreateFunctor = SaiSpyFunctor<n, objtype, sai_status_t, sai_object_id_t *, sai_object_id_t, uint32_t, const sai_attribute_t *>;

    return std::make_shared<SaiSpyCreateFunctor>(fn_ptr);
}

// create entry without input oid
template <int n, int objtype>
std::shared_ptr<SaiSpyFunctor<n, objtype, sai_status_t, sai_object_id_t *, uint32_t, const sai_attribute_t *>>
    SpyOn(sai_status_t (**fn_ptr)(sai_object_id_t *, uint32_t, const sai_attribute_t *))
{
    using SaiSpyCreateFunctor = SaiSpyFunctor<n, objtype, sai_status_t, sai_object_id_t *, uint32_t, const sai_attribute_t *>;

    return std::make_shared<SaiSpyCreateFunctor>(fn_ptr);
}

// remove entry
template <int n, int objtype>
std::shared_ptr<SaiSpyFunctor<n, objtype, sai_status_t, sai_object_id_t>>
    SpyOn(sai_status_t (**fn_ptr)(sai_object_id_t))
{
    using SaiSpyRemoveFunctor = SaiSpyFunctor<n, objtype, sai_status_t, sai_object_id_t>;

    return std::make_shared<SaiSpyRemoveFunctor>(fn_ptr);
}

// set entry attribute
template <int n, int objtype>
std::shared_ptr<SaiSpyFunctor<n, objtype, sai_status_t, sai_object_id_t, const sai_attribute_t *>>
    SpyOn(sai_status_t (**fn_ptr)(sai_object_id_t, const sai_attribute_t *))
{
    using SaiSpySetAttrFunctor = SaiSpyFunctor<n, objtype, sai_status_t, sai_object_id_t, const sai_attribute_t *>;

    return std::make_shared<SaiSpySetAttrFunctor>(fn_ptr);
}

// get entry attribute
template <int n, int objtype>
std::shared_ptr<SaiSpyFunctor<n, objtype, sai_status_t, sai_object_id_t, uint32_t, sai_attribute_t *>>
    SpyOn(sai_status_t (**fn_ptr)(sai_object_id_t, uint32_t, sai_attribute_t *))
{
    using SaiSpyGetAttrFunctor = SaiSpyFunctor<n, objtype, sai_status_t, sai_object_id_t, uint32_t, sai_attribute_t *>;

    return std::make_shared<SaiSpyGetAttrFunctor>(fn_ptr);
}
