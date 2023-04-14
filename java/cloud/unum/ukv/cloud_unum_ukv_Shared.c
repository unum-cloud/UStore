#include <string.h>

#include "cloud_unum_ukv_Shared.h"

jfieldID find_db_field(JNIEnv* env_java) {
    static jfieldID db_ptr_field = NULL;
    if (!db_ptr_field) {
        // https://docs.oracle.com/javase/8/docs/technotes/guides/jni/spec/types.html
        // https://docs.oracle.com/javase/tutorial/java/nutsandbolts/datatypes.html
        jclass txn_class_java = (*env_java)->FindClass(env_java, "cloud/unum/ukv/DataBase$Transaction");
        db_ptr_field = (*env_java)->GetFieldID(env_java, txn_class_java, "databaseAddress", "J");
    }
    return db_ptr_field;
}

jfieldID find_txn_field(JNIEnv* env_java) {
    static jfieldID txn_ptr_field = NULL;
    if (!txn_ptr_field) {
        // https://docs.oracle.com/javase/8/docs/technotes/guides/jni/spec/types.html
        // https://docs.oracle.com/javase/tutorial/java/nutsandbolts/datatypes.html
        jclass txn_class_java = (*env_java)->FindClass(env_java, "cloud/unum/ukv/DataBase$Transaction");
        txn_ptr_field = (*env_java)->GetFieldID(env_java, txn_class_java, "transactionAddress", "J");
    }
    return txn_ptr_field;
}

ukv_database_t db_ptr(JNIEnv* env_java, jobject txn_java) {
    jfieldID db_ptr_field = find_db_field(env_java);
    long int db_ptr_java = (*env_java)->GetLongField(env_java, txn_java, db_ptr_field);
    return (ukv_database_t)db_ptr_java;
}

ukv_transaction_t txn_ptr(JNIEnv* env_java, jobject txn_java) {
    jfieldID txn_ptr_field = find_txn_field(env_java);
    long int txn_ptr_java = (*env_java)->GetLongField(env_java, txn_java, txn_ptr_field);
    return (ukv_transaction_t)txn_ptr_java;
}

ukv_collection_t collection_ptr(JNIEnv* env_java, ukv_database_t db_ptr, jstring name_java) {

    // We may be passing the empty name of the default collection
    if (!name_java)
        return 0;

    // Temporarily copy the contents of the passed string
    jboolean name_is_copy_java = JNI_FALSE;
    char const* name_c = (*env_java)->GetStringUTFChars(env_java, name_java, &name_is_copy_java);
    if ((*env_java)->ExceptionCheck(env_java))
        return 0;

    ukv_size_t count = 0;
    ukv_str_span_t names = NULL;
    ukv_collection_t* ids = NULL;
    ukv_error_t error_c = NULL;
    ukv_arena_t arena_c = NULL;

    // Try find collection in existing collections
    struct ukv_collection_list_t collection_list = {
        .db = db_ptr,
        .error = &error_c,
        .arena = &arena_c,
        .count = &count,
        .ids = &ids,
        .names = &names,
    };

    ukv_collection_t collection_c = 0;
    ukv_collection_list(&collection_list);
    if (error_c == NULL) {
        for (ukv_size_t i = 0, j = 0; i < count; ++i, j += strlen(names + j)) {
            if (strcmp(names + j, name_c) != 0)
                continue;
            collection_c = ids[i];
            break;
        }
    }

    // Create new collection by name `name_c`
    if (error_c == NULL && collection_c == 0) {
        struct ukv_collection_create_t collection_init = {
            .db = db_ptr,
            .error = &error_c,
            .name = name_c,
            .config = NULL,
            .id = &collection_c,
        };

        ukv_collection_create(&collection_init);
    }

    if (name_is_copy_java == JNI_TRUE)
        (*env_java)->ReleaseStringUTFChars(env_java, name_java, name_c);

    forward_error(env_java, error_c);
    return collection_c;
}

bool forward_error(JNIEnv* env_java, char const* error_c) {
    if (!error_c)
        return false;

    // Error handling in JNI:
    // https://stackoverflow.com/a/15289742
    jclass error_java = (*env_java)->FindClass(env_java, "java/lang/Error");
    if (error_java)
        (*env_java)->ThrowNew(env_java, error_java, error_c);

    return true;
}

bool forward_ukv_error(JNIEnv* env_java, ukv_error_t error_c) {
    if (forward_error(env_java, error_c)) {
        ukv_error_free(error_c);
        return true;
    }
    return false;
}