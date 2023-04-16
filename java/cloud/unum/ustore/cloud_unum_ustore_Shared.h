
#ifndef _Included_cloud_unum_ustore_Shared
#define _Included_cloud_unum_ustore_Shared

#include <jni.h>
#include "ustore/ustore.h"

#ifdef __cplusplus
extern "C" {
#endif

jfieldID find_db_field(JNIEnv* env_java);

jfieldID find_txn_field(JNIEnv* env_java);

ustore_database_t db_ptr(JNIEnv* env_java, jobject txn_java);

ustore_transaction_t txn_ptr(JNIEnv* env_java, jobject txn_java);

ustore_collection_t collection_ptr(JNIEnv* env_java, ustore_database_t db_ptr, jstring name_java);

/**
 * @return true  If error was detected.
 * @return false If no error appeared.
 */
bool forward_error(JNIEnv* env_java, char const* error_c);
bool forward_ustore_error(JNIEnv* env_java, ustore_error_t error_c);

#ifdef __cplusplus
}
#endif
#endif
