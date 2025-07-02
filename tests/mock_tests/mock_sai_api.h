#ifndef MOCK_SAI_API_H
#define MOCK_SAI_API_H
#include "mock_orchagent_main.h"
#include <gmock/gmock.h>

/*
To mock a particular SAI API:
1. At the top of the test CPP file using the mock, call DEFINE_SAI_API_MOCK or DEFINE_SAI_GENERIC_API_MOCK
   for each SAI API you want to mock.
2. At the top of the test CPP file using the mock, call EXTERN_MOCK_FNS.
3. In the SetUp method of the test class, call INIT_SAI_API_MOCK for each SAI API you want to mock.
4. In the SetUp method of the test class, call MockSaiApis.
5. In the TearDown method of the test class, call RestoreSaiApis.
6. After RestoreSaiApis, call DEINIT_SAI_API_MOCK
*/

using ::testing::Return;
using ::testing::NiceMock;

#define EXTERN_MOCK_FNS                         \
    extern std::set<void (*)()> apply_mock_fns; \
    extern std::set<void (*)()> remove_mock_fns;

EXTERN_MOCK_FNS

#define CREATE_PARAMS(sai_entry_type) _In_ const sai_##sai_entry_type##_entry_t *sai_entry_type##_entry, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list
#define REMOVE_PARAMS(sai_entry_type) _In_ const sai_##sai_entry_type##_entry_t *sai_entry_type##_entry
#define CREATE_BULK_PARAMS(sai_entry_type) _In_ uint32_t object_count, _In_ const sai_##sai_entry_type##_entry_t *sai_entry_type##_entry, _In_ const uint32_t *attr_count, _In_ const sai_attribute_t **attr_list, _In_ sai_bulk_op_error_mode_t mode, _Out_ sai_status_t *object_statuses
#define REMOVE_BULK_PARAMS(sai_entry_type) _In_ uint32_t object_count, _In_ const sai_##sai_entry_type##_entry_t *sai_entry_type##_entry, _In_ sai_bulk_op_error_mode_t mode, _In_ sai_status_t *object_statuses
#define SET_BULK_ATTR_PARAMS(sai_entry_type) _In_ uint32_t object_count, _In_ const sai_##sai_entry_type##_entry_t *sai_entry_type##__entry, _In_ const sai_attribute_t *attr_list, _In_ sai_bulk_op_error_mode_t mode, _Out_ sai_status_t *object_statuses
#define CREATE_ARGS(sai_entry_type) sai_entry_type##_entry, attr_count, attr_list
#define REMOVE_ARGS(sai_entry_type) sai_entry_type##_entry
#define CREATE_BULK_ARGS(sai_entry_type) object_count, sai_entry_type##_entry, attr_count, attr_list, mode, object_statuses
#define REMOVE_BULK_ARGS(sai_entry_type) object_count, sai_entry_type##_entry, mode, object_statuses
#define SET_BULK_ATTR_ARGS(sai_entry_type) object_count, sai_entry_type##__entry, attr_list, mode, object_statuses
#define GENERIC_CREATE_PARAMS(sai_object_type) _Out_ sai_object_id_t *sai_object_type##_id, _In_ sai_object_id_t switch_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list
#define GENERIC_REMOVE_PARAMS(sai_object_type) _In_ sai_object_id_t sai_object_type##_id
#define GENERIC_BULK_CREATE_PARAMS(sai_object_type) _In_ sai_object_id_t switch_id, _In_ uint32_t object_count, _In_ const uint32_t *attr_count, _In_ const sai_attribute_t **attr_list, _In_ sai_bulk_op_error_mode_t mode, _Out_ sai_object_id_t *object_id, _Out_ sai_status_t *object_statuses
#define GENERIC_BULK_REMOVE_PARAMS(sai_object_type) _In_ uint32_t object_count, _In_ const sai_object_id_t *object_id, _In_ sai_bulk_op_error_mode_t mode, _Out_ sai_status_t *object_statuses
#define GENERIC_CREATE_ARGS(sai_object_type) sai_object_type##_id, switch_id, attr_count, attr_list
#define GENERIC_REMOVE_ARGS(sai_object_type) sai_object_type##_id
#define GENERIC_BULK_CREATE_ARGS(sai_object_type) switch_id, object_count, attr_count, attr_list, mode, object_id, object_statuses
#define GENERIC_BULK_REMOVE_ARGS(sai_object_type) object_count, object_id, mode, object_statuses

#define DEFINE_SAI_API_MOCK_SPECIFY_ENTRY(sai_api_type, sai_entry_type)                                                           \
    static sai_##sai_api_type##_api_t *old_sai_##sai_api_type##_api;                                                              \
    static sai_##sai_api_type##_api_t ut_sai_##sai_api_type##_api;                                                                \
    class mock_sai_##sai_api_type##_api_t                                                                                         \
    {                                                                                                                             \
    public:                                                                                                                       \
        mock_sai_##sai_api_type##_api_t()                                                                                         \
        {                                                                                                                         \
            ON_CALL(*this, create_##sai_entry_type##_entry)                                                                       \
                .WillByDefault(                                                                                                   \
                    [this](CREATE_PARAMS(sai_entry_type)) {                                                                       \
                        return old_sai_##sai_api_type##_api->create_##sai_entry_type##_entry(CREATE_ARGS(sai_entry_type));        \
                    });                                                                                                           \
            ON_CALL(*this, remove_##sai_entry_type##_entry)                                                                       \
                .WillByDefault(                                                                                                   \
                    [this](REMOVE_PARAMS(sai_entry_type)) {                                                                       \
                        return old_sai_##sai_api_type##_api->remove_##sai_entry_type##_entry(REMOVE_ARGS(sai_entry_type));        \
                    });                                                                                                           \
            ON_CALL(*this, create_##sai_entry_type##_entries)                                                                     \
                .WillByDefault(                                                                                                   \
                    [this](CREATE_BULK_PARAMS(sai_entry_type)) {                                                                  \
                        return old_sai_##sai_api_type##_api->create_##sai_entry_type##_entries(CREATE_BULK_ARGS(sai_entry_type)); \
                    });                                                                                                           \
            ON_CALL(*this, remove_##sai_entry_type##_entries)                                                                     \
                .WillByDefault(                                                                                                   \
                    [this](REMOVE_BULK_PARAMS(sai_entry_type)) {                                                                  \
                        return old_sai_##sai_api_type##_api->remove_##sai_entry_type##_entries(REMOVE_BULK_ARGS(sai_entry_type)); \
                    });                                                                                                           \
        }                                                                                                                         \
        MOCK_METHOD3(create_##sai_entry_type##_entry, sai_status_t(CREATE_PARAMS(sai_entry_type)));                               \
        MOCK_METHOD1(remove_##sai_entry_type##_entry, sai_status_t(REMOVE_PARAMS(sai_entry_type)));                               \
        MOCK_METHOD6(create_##sai_entry_type##_entries, sai_status_t(CREATE_BULK_PARAMS(sai_entry_type)));                        \
        MOCK_METHOD4(remove_##sai_entry_type##_entries, sai_status_t(REMOVE_BULK_PARAMS(sai_entry_type)));                        \
    };                                                                                                                            \
    static mock_sai_##sai_api_type##_api_t *mock_sai_##sai_api_type##_api;                                                        \
    inline sai_status_t mock_create_##sai_entry_type##_entry(CREATE_PARAMS(sai_entry_type))                                       \
    {                                                                                                                             \
        return mock_sai_##sai_api_type##_api->create_##sai_entry_type##_entry(CREATE_ARGS(sai_entry_type));                       \
    }                                                                                                                             \
    inline sai_status_t mock_remove_##sai_entry_type##_entry(REMOVE_PARAMS(sai_entry_type))                                       \
    {                                                                                                                             \
        return mock_sai_##sai_api_type##_api->remove_##sai_entry_type##_entry(REMOVE_ARGS(sai_entry_type));                       \
    }                                                                                                                             \
    inline sai_status_t mock_create_##sai_entry_type##_entries(CREATE_BULK_PARAMS(sai_entry_type))                                \
    {                                                                                                                             \
        return mock_sai_##sai_api_type##_api->create_##sai_entry_type##_entries(CREATE_BULK_ARGS(sai_entry_type));                \
    }                                                                                                                             \
    inline sai_status_t mock_remove_##sai_entry_type##_entries(REMOVE_BULK_PARAMS(sai_entry_type))                                \
    {                                                                                                                             \
        return mock_sai_##sai_api_type##_api->remove_##sai_entry_type##_entries(REMOVE_BULK_ARGS(sai_entry_type));                \
    }                                                                                                                             \
    inline void apply_sai_##sai_api_type##_api_mock()                                                                             \
    {                                                                                                                             \
        mock_sai_##sai_api_type##_api = new NiceMock<mock_sai_##sai_api_type##_api_t>();                                          \
                                                                                                                                  \
        old_sai_##sai_api_type##_api = sai_##sai_api_type##_api;                                                                  \
        ut_sai_##sai_api_type##_api = *sai_##sai_api_type##_api;                                                                  \
        sai_##sai_api_type##_api = &ut_sai_##sai_api_type##_api;                                                                  \
                                                                                                                                  \
        sai_##sai_api_type##_api->create_##sai_entry_type##_entry = mock_create_##sai_entry_type##_entry;                         \
        sai_##sai_api_type##_api->remove_##sai_entry_type##_entry = mock_remove_##sai_entry_type##_entry;                         \
        sai_##sai_api_type##_api->create_##sai_entry_type##_entries = mock_create_##sai_entry_type##_entries;                     \
        sai_##sai_api_type##_api->remove_##sai_entry_type##_entries = mock_remove_##sai_entry_type##_entries;                     \
    }                                                                                                                             \
    inline void remove_sai_##sai_api_type##_api_mock()                                                                            \
    {                                                                                                                             \
        sai_##sai_api_type##_api = old_sai_##sai_api_type##_api;                                                                  \
        delete mock_sai_##sai_api_type##_api;                                                                                     \
    }

/*
The same as DEFINE_SAI_API_MOCK but with addition definitions for set_<sai_api_type>_entries_attribute.
This is required since some sai_api do not support this function call yet.
*/
#define DEFINE_SAI_API_MOCK_SPECIFY_ENTRY_WITH_SET(sai_api_type, sai_entry_type)                                                  \
    static sai_##sai_api_type##_api_t *old_sai_##sai_api_type##_api;                                                              \
    static sai_##sai_api_type##_api_t ut_sai_##sai_api_type##_api;                                                                \
    class mock_sai_##sai_api_type##_api_t                                                                                         \
    {                                                                                                                             \
    public:                                                                                                                       \
        mock_sai_##sai_api_type##_api_t()                                                                                         \
        {                                                                                                                         \
            ON_CALL(*this, create_##sai_entry_type##_entry)                                                                       \
                .WillByDefault(                                                                                                   \
                    [this](CREATE_PARAMS(sai_entry_type)) {                                                                       \
                        return old_sai_##sai_api_type##_api->create_##sai_entry_type##_entry(CREATE_ARGS(sai_entry_type));        \
                    });                                                                                                           \
            ON_CALL(*this, remove_##sai_entry_type##_entry)                                                                       \
                .WillByDefault(                                                                                                   \
                    [this](REMOVE_PARAMS(sai_entry_type)) {                                                                       \
                        return old_sai_##sai_api_type##_api->remove_##sai_entry_type##_entry(REMOVE_ARGS(sai_entry_type));        \
                    });                                                                                                           \
            ON_CALL(*this, create_##sai_entry_type##_entries)                                                                     \
                .WillByDefault(                                                                                                   \
                    [this](CREATE_BULK_PARAMS(sai_entry_type)) {                                                                  \
                        return old_sai_##sai_api_type##_api->create_##sai_entry_type##_entries(CREATE_BULK_ARGS(sai_entry_type)); \
                    });                                                                                                           \
            ON_CALL(*this, remove_##sai_entry_type##_entries)                                                                     \
                .WillByDefault(                                                                                                   \
                    [this](REMOVE_BULK_PARAMS(sai_entry_type)) {                                                                  \
                        return old_sai_##sai_api_type##_api->remove_##sai_entry_type##_entries(REMOVE_BULK_ARGS(sai_entry_type)); \
                    });                                                                                                           \
            ON_CALL(*this, set_##sai_entry_type##_entries_attribute)                                                              \
                .WillByDefault(                                                                                                   \
                    [this](SET_BULK_ATTR_PARAMS(sai_entry_type)) {                                                                \
                        return old_sai_##sai_api_type##_api->set_##sai_entry_type##_entries_attribute(                            \
                                                                                             SET_BULK_ATTR_ARGS(sai_entry_type)); \
                    });                                                                                                           \
        }                                                                                                                         \
        MOCK_METHOD3(create_##sai_entry_type##_entry, sai_status_t(CREATE_PARAMS(sai_entry_type)));                               \
        MOCK_METHOD1(remove_##sai_entry_type##_entry, sai_status_t(REMOVE_PARAMS(sai_entry_type)));                               \
        MOCK_METHOD6(create_##sai_entry_type##_entries, sai_status_t(CREATE_BULK_PARAMS(sai_entry_type)));                        \
        MOCK_METHOD4(remove_##sai_entry_type##_entries, sai_status_t(REMOVE_BULK_PARAMS(sai_entry_type)));                        \
        MOCK_METHOD5(set_##sai_entry_type##_entries_attribute, sai_status_t(SET_BULK_ATTR_PARAMS(sai_entry_type)));               \
    };                                                                                                                            \
    static mock_sai_##sai_api_type##_api_t *mock_sai_##sai_api_type##_api;                                                        \
    inline sai_status_t mock_create_##sai_entry_type##_entry(CREATE_PARAMS(sai_entry_type))                                       \
    {                                                                                                                             \
        return mock_sai_##sai_api_type##_api->create_##sai_entry_type##_entry(CREATE_ARGS(sai_entry_type));                       \
    }                                                                                                                             \
    inline sai_status_t mock_remove_##sai_entry_type##_entry(REMOVE_PARAMS(sai_entry_type))                                       \
    {                                                                                                                             \
        return mock_sai_##sai_api_type##_api->remove_##sai_entry_type##_entry(REMOVE_ARGS(sai_entry_type));                       \
    }                                                                                                                             \
    inline sai_status_t mock_create_##sai_entry_type##_entries(CREATE_BULK_PARAMS(sai_entry_type))                                \
    {                                                                                                                             \
        return mock_sai_##sai_api_type##_api->create_##sai_entry_type##_entries(CREATE_BULK_ARGS(sai_entry_type));                \
    }                                                                                                                             \
    inline sai_status_t mock_remove_##sai_entry_type##_entries(REMOVE_BULK_PARAMS(sai_entry_type))                                \
    {                                                                                                                             \
        return mock_sai_##sai_api_type##_api->remove_##sai_entry_type##_entries(REMOVE_BULK_ARGS(sai_entry_type));                \
    }                                                                                                                             \
    inline sai_status_t mock_set_##sai_entry_type##_entries_attribute(SET_BULK_ATTR_PARAMS(sai_entry_type))                       \
    {                                                                                                                             \
        return mock_sai_##sai_api_type##_api->set_##sai_entry_type##_entries_attribute(SET_BULK_ATTR_ARGS(sai_entry_type));       \
    }                                                                                                                             \
    inline void apply_sai_##sai_api_type##_api_mock()                                                                             \
    {                                                                                                                             \
        mock_sai_##sai_api_type##_api = new NiceMock<mock_sai_##sai_api_type##_api_t>();                                          \
                                                                                                                                  \
        old_sai_##sai_api_type##_api = sai_##sai_api_type##_api;                                                                  \
        ut_sai_##sai_api_type##_api = *sai_##sai_api_type##_api;                                                                  \
        sai_##sai_api_type##_api = &ut_sai_##sai_api_type##_api;                                                                  \
                                                                                                                                  \
        sai_##sai_api_type##_api->create_##sai_entry_type##_entry = mock_create_##sai_entry_type##_entry;                         \
        sai_##sai_api_type##_api->remove_##sai_entry_type##_entry = mock_remove_##sai_entry_type##_entry;                         \
        sai_##sai_api_type##_api->create_##sai_entry_type##_entries = mock_create_##sai_entry_type##_entries;                     \
        sai_##sai_api_type##_api->remove_##sai_entry_type##_entries = mock_remove_##sai_entry_type##_entries;                     \
        sai_##sai_api_type##_api->set_##sai_entry_type##_entries_attribute = mock_set_##sai_entry_type##_entries_attribute;       \
    }                                                                                                                             \
    inline void remove_sai_##sai_api_type##_api_mock()                                                                            \
    {                                                                                                                             \
        sai_##sai_api_type##_api = old_sai_##sai_api_type##_api;                                                                  \
        delete mock_sai_##sai_api_type##_api;                                                                                     \
    }

#define DEFINE_SAI_API_MOCK_MATCH_ENTRY(sai_api_type) DEFINE_SAI_API_MOCK_SPECIFY_ENTRY(sai_api_type, sai_api_type)

/* Overload DEFINE_SAI_API_MOCK by number of arguments to account for entry types that do not match the API name
 * 1. If one argument is provided, assume the entry type matches API name (e.g. sai_neighbor_api_t and sai_neighbor_entry_t)
 * 2. If two arguments are provided, use the second argument as the entry type (e.g. sai_dash_outbound_ca_to_pa_api_t and sai_outbound_ca_to_pa_entry_t)
 */
#define GET_MOCK_MACRO(_1, _2, NAME, ...) NAME
#define DEFINE_SAI_API_MOCK(...) \
    GET_MOCK_MACRO(__VA_ARGS__, DEFINE_SAI_API_MOCK_SPECIFY_ENTRY, DEFINE_SAI_API_MOCK_MATCH_ENTRY)(__VA_ARGS__)

/* DEFINE_SAI_GENERIC_API_MOCK will be deprecated.
 * Please use DEFINE_SAI_GENERIC_APIS_MOCK which supports one or multiple sai_object_type to be mocked in one sai api class.
 */
#define DEFINE_SAI_GENERIC_API_MOCK(sai_api_name, sai_object_type)                                                           \
    static sai_##sai_api_name##_api_t *old_sai_##sai_api_name##_api;                                                         \
    static sai_##sai_api_name##_api_t ut_sai_##sai_api_name##_api;                                                           \
    class mock_sai_##sai_api_name##_api_t                                                                                    \
    {                                                                                                                        \
    public:                                                                                                                  \
        mock_sai_##sai_api_name##_api_t()                                                                                    \
        {                                                                                                                    \
            ON_CALL(*this, create_##sai_object_type)                                                                         \
                .WillByDefault(                                                                                              \
                    [this](GENERIC_CREATE_PARAMS(sai_object_type)) {                                                         \
                        return old_sai_##sai_api_name##_api->create_##sai_object_type(GENERIC_CREATE_ARGS(sai_object_type)); \
                    });                                                                                                      \
            ON_CALL(*this, remove_##sai_object_type)                                                                         \
                .WillByDefault(                                                                                              \
                    [this](GENERIC_REMOVE_PARAMS(sai_object_type)) {                                                         \
                        return old_sai_##sai_api_name##_api->remove_##sai_object_type(GENERIC_REMOVE_ARGS(sai_object_type)); \
                    });                                                                                                      \
        }                                                                                                                    \
        MOCK_METHOD4(create_##sai_object_type, sai_status_t(GENERIC_CREATE_PARAMS(sai_object_type)));                        \
        MOCK_METHOD1(remove_##sai_object_type, sai_status_t(GENERIC_REMOVE_PARAMS(sai_object_type)));                        \
    };                                                                                                                       \
    static mock_sai_##sai_api_name##_api_t *mock_sai_##sai_api_name##_api;                                                   \
    inline sai_status_t mock_create_##sai_object_type(GENERIC_CREATE_PARAMS(sai_object_type))                                \
    {                                                                                                                        \
        return mock_sai_##sai_api_name##_api->create_##sai_object_type(GENERIC_CREATE_ARGS(sai_object_type));                \
    }                                                                                                                        \
    inline sai_status_t mock_remove_##sai_object_type(GENERIC_REMOVE_PARAMS(sai_object_type))                                \
    {                                                                                                                        \
        return mock_sai_##sai_api_name##_api->remove_##sai_object_type(GENERIC_REMOVE_ARGS(sai_object_type));                \
    }                                                                                                                        \
    inline void apply_sai_##sai_api_name##_api_mock()                                                                        \
    {                                                                                                                        \
        mock_sai_##sai_api_name##_api = new NiceMock<mock_sai_##sai_api_name##_api_t>();                                     \
                                                                                                                             \
        old_sai_##sai_api_name##_api = sai_##sai_api_name##_api;                                                             \
        ut_sai_##sai_api_name##_api = *sai_##sai_api_name##_api;                                                             \
        sai_##sai_api_name##_api = &ut_sai_##sai_api_name##_api;                                                             \
                                                                                                                             \
        sai_##sai_api_name##_api->create_##sai_object_type = mock_create_##sai_object_type;                                  \
        sai_##sai_api_name##_api->remove_##sai_object_type = mock_remove_##sai_object_type;                                  \
    }                                                                                                                        \
    inline void remove_sai_##sai_api_name##_api_mock()                                                                       \
    {                                                                                                                        \
        sai_##sai_api_name##_api = old_sai_##sai_api_name##_api;                                                             \
        delete mock_sai_##sai_api_name##_api;                                                                                \
    }

/* Helper macros to iterate over multiple sai_object_type inputs */
#define FOR_EACH_1(action, api_name, x) action(api_name, x)
#define FOR_EACH_2(action, api_name, x, ...) action(api_name, x) FOR_EACH_1(action, api_name, __VA_ARGS__)

#define GET_FOR_EACH_MACRO(_1,_2,NAME,...) NAME
#define FOR_EACH(action, api_name, ...) \
    GET_FOR_EACH_MACRO(__VA_ARGS__, FOR_EACH_2, FOR_EACH_1)(action, api_name, __VA_ARGS__)

#define DEFINE_ON_CALL_DEFAULTS(sai_api_name, sai_object_type) \
    ON_CALL(*this, create_##sai_object_type).WillByDefault([this](GENERIC_CREATE_PARAMS(sai_object_type)) { \
        return old_sai_##sai_api_name##_api->create_##sai_object_type(GENERIC_CREATE_ARGS(sai_object_type)); \
    }); \
    ON_CALL(*this, remove_##sai_object_type).WillByDefault([this](GENERIC_REMOVE_PARAMS(sai_object_type)) { \
        return old_sai_##sai_api_name##_api->remove_##sai_object_type(GENERIC_REMOVE_ARGS(sai_object_type)); \
    }); \
    ON_CALL(*this, set_##sai_object_type##_attribute).WillByDefault([this](sai_object_id_t oid, const sai_attribute_t *attr) { \
        return old_sai_##sai_api_name##_api->set_##sai_object_type##_attribute(oid, attr); \
    });
#define DEFINE_MOCK_METHODS(sai_api_name, sai_object_type) \
    MOCK_METHOD4(create_##sai_object_type, sai_status_t(GENERIC_CREATE_PARAMS(sai_object_type))); \
    MOCK_METHOD1(remove_##sai_object_type, sai_status_t(GENERIC_REMOVE_PARAMS(sai_object_type))); \
    MOCK_METHOD2(set_##sai_object_type##_attribute, sai_status_t(sai_object_id_t, const sai_attribute_t *));
#define DEFINE_WRAPPER_FUNCTIONS(sai_api_name, sai_object_type) \
    inline sai_status_t mock_create_##sai_object_type(GENERIC_CREATE_PARAMS(sai_object_type)) { \
        return mock_sai_##sai_api_name##_api->create_##sai_object_type(GENERIC_CREATE_ARGS(sai_object_type)); \
    } \
    inline sai_status_t mock_remove_##sai_object_type(GENERIC_REMOVE_PARAMS(sai_object_type)) { \
        return mock_sai_##sai_api_name##_api->remove_##sai_object_type(GENERIC_REMOVE_ARGS(sai_object_type)); \
    } \
    inline sai_status_t mock_set_##sai_object_type##_attribute(sai_object_id_t oid, const sai_attribute_t *attr) { \
        return mock_sai_##sai_api_name##_api->set_##sai_object_type##_attribute(oid, attr); \
    }
#define APPLY_MOCK_FUNCTIONS(sai_api_name, sai_object_type) \
    sai_##sai_api_name##_api->create_##sai_object_type = mock_create_##sai_object_type; \
    sai_##sai_api_name##_api->remove_##sai_object_type = mock_remove_##sai_object_type; \
    sai_##sai_api_name##_api->set_##sai_object_type##_attribute = mock_set_##sai_object_type##_attribute;

#define DEFINE_SAI_GENERIC_APIS_MOCK(sai_api_name, ...) \
    static sai_##sai_api_name##_api_t *old_sai_##sai_api_name##_api; \
    static sai_##sai_api_name##_api_t ut_sai_##sai_api_name##_api; \
    class mock_sai_##sai_api_name##_api_t { \
    public: \
        mock_sai_##sai_api_name##_api_t() { \
            FOR_EACH(DEFINE_ON_CALL_DEFAULTS, sai_api_name, __VA_ARGS__) \
        } \
        FOR_EACH(DEFINE_MOCK_METHODS, sai_api_name, __VA_ARGS__) \
    }; \
    static mock_sai_##sai_api_name##_api_t *mock_sai_##sai_api_name##_api; \
    FOR_EACH(DEFINE_WRAPPER_FUNCTIONS, sai_api_name, __VA_ARGS__) \
    inline void apply_sai_##sai_api_name##_api_mock() { \
        mock_sai_##sai_api_name##_api = new NiceMock<mock_sai_##sai_api_name##_api_t>(); \
        old_sai_##sai_api_name##_api = sai_##sai_api_name##_api; \
        ut_sai_##sai_api_name##_api = *sai_##sai_api_name##_api; \
        sai_##sai_api_name##_api = &ut_sai_##sai_api_name##_api; \
        FOR_EACH(APPLY_MOCK_FUNCTIONS, sai_api_name, __VA_ARGS__) \
    } \
    inline void remove_sai_##sai_api_name##_api_mock() { \
        sai_##sai_api_name##_api = old_sai_##sai_api_name##_api; \
        delete mock_sai_##sai_api_name##_api; \
    }

#define DEFINE_SAI_GENERIC_API_OBJECT_BULK_MOCK(sai_api_name, sai_object_type)                                                       \
    static sai_##sai_api_name##_api_t *old_sai_##sai_api_name##_api;                                                                 \
    static sai_##sai_api_name##_api_t ut_sai_##sai_api_name##_api;                                                                   \
    class mock_sai_##sai_api_name##_api_t                                                                                            \
    {                                                                                                                                \
    public:                                                                                                                          \
        mock_sai_##sai_api_name##_api_t()                                                                                            \
        {                                                                                                                            \
            ON_CALL(*this, create_##sai_object_type)                                                                                 \
                .WillByDefault(                                                                                                      \
                    [this](GENERIC_CREATE_PARAMS(sai_object_type)) {                                                                 \
                        return old_sai_##sai_api_name##_api->create_##sai_object_type(GENERIC_CREATE_ARGS(sai_object_type));         \
                    });                                                                                                              \
            ON_CALL(*this, remove_##sai_object_type)                                                                                 \
                .WillByDefault(                                                                                                      \
                    [this](GENERIC_REMOVE_PARAMS(sai_object_type)) {                                                                 \
                        return old_sai_##sai_api_name##_api->remove_##sai_object_type(GENERIC_REMOVE_ARGS(sai_object_type));         \
                    });                                                                                                              \
            ON_CALL(*this, create_##sai_object_type##s)                                                                              \
                .WillByDefault(                                                                                                      \
                    [this](GENERIC_BULK_CREATE_PARAMS(sai_object_type)) {                                                            \
                        return old_sai_##sai_api_name##_api->create_##sai_object_type##s(GENERIC_BULK_CREATE_ARGS(sai_object_type)); \
                    });                                                                                                              \
            ON_CALL(*this, remove_##sai_object_type##s)                                                                              \
                .WillByDefault(                                                                                                      \
                    [this](GENERIC_BULK_REMOVE_PARAMS(sai_object_type)) {                                                            \
                        return old_sai_##sai_api_name##_api->remove_##sai_object_type##s(GENERIC_BULK_REMOVE_ARGS(sai_object_type)); \
                    });                                                                                                              \
        }                                                                                                                            \
        MOCK_METHOD4(create_##sai_object_type, sai_status_t(GENERIC_CREATE_PARAMS(sai_object_type)));                                \
        MOCK_METHOD1(remove_##sai_object_type, sai_status_t(GENERIC_REMOVE_PARAMS(sai_object_type)));                                \
        MOCK_METHOD7(create_##sai_object_type##s, sai_status_t(GENERIC_BULK_CREATE_PARAMS(sai_object_type)));                        \
        MOCK_METHOD4(remove_##sai_object_type##s, sai_status_t(GENERIC_BULK_REMOVE_PARAMS(sai_object_type)));                        \
    };                                                                                                                               \
    static mock_sai_##sai_api_name##_api_t *mock_sai_##sai_api_name##_api;                                                           \
    inline sai_status_t mock_create_##sai_object_type(GENERIC_CREATE_PARAMS(sai_object_type))                                        \
    {                                                                                                                                \
        return mock_sai_##sai_api_name##_api->create_##sai_object_type(GENERIC_CREATE_ARGS(sai_object_type));                        \
    }                                                                                                                                \
    inline sai_status_t mock_remove_##sai_object_type(GENERIC_REMOVE_PARAMS(sai_object_type))                                        \
    {                                                                                                                                \
        return mock_sai_##sai_api_name##_api->remove_##sai_object_type(GENERIC_REMOVE_ARGS(sai_object_type));                        \
    }                                                                                                                                \
    inline sai_status_t mock_create_##sai_object_type##s(GENERIC_BULK_CREATE_PARAMS(sai_object_type))                                \
    {                                                                                                                                \
        return mock_sai_##sai_api_name##_api->create_##sai_object_type##s(GENERIC_BULK_CREATE_ARGS(sai_object_type));                \
    }                                                                                                                                \
    inline sai_status_t mock_remove_##sai_object_type##s(GENERIC_BULK_REMOVE_PARAMS(sai_object_type))                                \
    {                                                                                                                                \
        return mock_sai_##sai_api_name##_api->remove_##sai_object_type##s(GENERIC_BULK_REMOVE_ARGS(sai_object_type));                \
    }                                                                                                                                \
    inline void apply_sai_##sai_api_name##_api_mock()                                                                                \
    {                                                                                                                                \
        mock_sai_##sai_api_name##_api = new NiceMock<mock_sai_##sai_api_name##_api_t>();                                             \
                                                                                                                                     \
        old_sai_##sai_api_name##_api = sai_##sai_api_name##_api;                                                                     \
        ut_sai_##sai_api_name##_api = *sai_##sai_api_name##_api;                                                                     \
        sai_##sai_api_name##_api = &ut_sai_##sai_api_name##_api;                                                                     \
                                                                                                                                     \
        sai_##sai_api_name##_api->create_##sai_object_type = mock_create_##sai_object_type;                                          \
        sai_##sai_api_name##_api->remove_##sai_object_type = mock_remove_##sai_object_type;                                          \
        sai_##sai_api_name##_api->create_##sai_object_type##s = mock_create_##sai_object_type##s;                                    \
        sai_##sai_api_name##_api->remove_##sai_object_type##s = mock_remove_##sai_object_type##s;                                    \
    }                                                                                                                                \
    inline void remove_sai_##sai_api_name##_api_mock()                                                                               \
    {                                                                                                                                \
        sai_##sai_api_name##_api = old_sai_##sai_api_name##_api;                                                                     \
        delete mock_sai_##sai_api_name##_api;                                                                                        \
    }

// Stores pointers to mock apply/remove functions to avoid needing to manually call each function
#define INIT_SAI_API_MOCK(sai_api_type)                          \
    apply_mock_fns.insert(&apply_sai_##sai_api_type##_api_mock); \
    remove_mock_fns.insert(&remove_sai_##sai_api_type##_api_mock);

/*
    Call this after RestoreSaiApis to clear the mock_fns
    Required when same SAI_API is being mocked in multiple files eg: acl API in multiple tests
*/
#define DEINIT_SAI_API_MOCK(sai_api_type)                       \
    apply_mock_fns.erase(&apply_sai_##sai_api_type##_api_mock); \
    remove_mock_fns.erase(&remove_sai_##sai_api_type##_api_mock);

void MockSaiApis();
void RestoreSaiApis();
#endif