#include "cloud_unum_ustore_Shared.h"
#include "cloud_unum_ustore_DataBase_Transaction.h"

JNIEXPORT void JNICALL Java_cloud_unum_ustore_DataBase_00024Transaction_put( //
    JNIEnv* env_java,
    jobject txn_java,
    jstring collection_java,
    jlong key_java,
    jbyteArray value_java) {

    ustore_database_t db_ptr_c = db_ptr(env_java, txn_java);
    if (!db_ptr_c) {
        forward_error(env_java, "Database is closed!");
        return;
    }

    ustore_transaction_t txn_ptr_c = txn_ptr(env_java, txn_java);
    ustore_collection_t collection_ptr_c = collection_ptr(env_java, db_ptr_c, collection_java);
    if ((*env_java)->ExceptionCheck(env_java))
        return;

    // Extract the raw pointer to underlying data
    // https://docs.oracle.com/en/java/javase/13/docs/specs/jni/functions.html
    jboolean value_is_copy_java = JNI_FALSE;
    jbyte* value_ptr_java = (*env_java)->GetByteArrayElements(env_java, value_java, &value_is_copy_java);
    jsize value_len_java = (*env_java)->GetArrayLength(env_java, value_java);
    if ((*env_java)->ExceptionCheck(env_java))
        return;

    // Cast everything to our types
    ustore_key_t key_c = (ustore_key_t)key_java;
    ustore_bytes_cptr_t found_values_c = (ustore_bytes_cptr_t)value_ptr_java;
    ustore_length_t value_off_c = 0;
    ustore_length_t value_len_c = (ustore_length_t)value_len_java;
    ustore_options_t options_c = ustore_options_default_k;
    ustore_arena_t arena_c = NULL;
    ustore_error_t error_c = NULL;

    struct ustore_write_t write = {
        .db = db_ptr_c,
        .error = &error_c,
        .transaction = txn_ptr_c,
        .arena = &arena_c,
        .options = options_c,
        .tasks_count = 1,
        .collections = &collection_ptr_c,
        .keys = &key_c,
        .offsets = &value_off_c,
        .lengths = &value_len_c,
        .values = &found_values_c,
    };

    ustore_write(&write);
    ustore_arena_free(arena_c);

    if (value_is_copy_java == JNI_TRUE)
        (*env_java)->ReleaseByteArrayElements(env_java, value_java, value_ptr_java, 0);
    forward_ustore_error(env_java, error_c);
}

JNIEXPORT jboolean JNICALL Java_cloud_unum_ustore_DataBase_00024Transaction_containsKey( //
    JNIEnv* env_java,
    jobject txn_java,
    jstring collection_java,
    jlong key_java) {

    ustore_database_t db_ptr_c = db_ptr(env_java, txn_java);
    if (!db_ptr_c) {
        forward_error(env_java, "Database is closed!");
        return JNI_FALSE;
    }

    ustore_transaction_t txn_ptr_c = txn_ptr(env_java, txn_java);
    ustore_collection_t collection_ptr_c = collection_ptr(env_java, db_ptr_c, collection_java);
    if ((*env_java)->ExceptionCheck(env_java))
        return JNI_FALSE;

    ustore_key_t key_c = (ustore_key_t)key_java;
    ustore_options_t options_c = ustore_options_default_k;
    ustore_octet_t* found_presences_c = NULL;
    ustore_arena_t arena_c = NULL;
    ustore_error_t error_c = NULL;
    struct ustore_read_t read = {
        .db = db_ptr_c,
        .error = &error_c,
        .transaction = txn_ptr_c,
        .arena = &arena_c,
        .options = options_c,
        .tasks_count = 1,
        .collections = &collection_ptr_c,
        .keys = &key_c,
        .presences = &found_presences_c,
    };

    ustore_read(&read);

    if (forward_error(env_java, error_c)) {
        ustore_arena_free(arena_c);
        return JNI_FALSE;
    }

    jboolean result = found_presences_c[0] != 0 ? JNI_TRUE : JNI_FALSE;
    ustore_arena_free(arena_c);
    return result;
}

JNIEXPORT jbyteArray JNICALL Java_cloud_unum_ustore_DataBase_00024Transaction_get( //
    JNIEnv* env_java,
    jobject txn_java,
    jstring collection_java,
    jlong key_java) {

    ustore_database_t db_ptr_c = db_ptr(env_java, txn_java);
    if (!db_ptr_c) {
        forward_error(env_java, "Database is closed!");
        return NULL;
    }

    ustore_transaction_t txn_ptr_c = txn_ptr(env_java, txn_java);
    ustore_collection_t collection_ptr_c = collection_ptr(env_java, db_ptr_c, collection_java);
    if ((*env_java)->ExceptionCheck(env_java))
        return NULL;

    ustore_key_t key_c = (ustore_key_t)key_java;
    ustore_options_t options_c = ustore_options_default_k;
    ustore_length_t* found_offsets_c = NULL;
    ustore_length_t* found_lengths_c = NULL;
    ustore_bytes_ptr_t found_values_c = NULL;
    ustore_arena_t arena_c = NULL;
    ustore_error_t error_c = NULL;
    struct ustore_read_t read = {
        .db = db_ptr_c,
        .error = &error_c,
        .transaction = txn_ptr_c,
        .arena = &arena_c,
        .options = options_c,
        .tasks_count = 1,
        .collections = &collection_ptr_c,
        .keys = &key_c,
        .offsets = &found_offsets_c,
        .lengths = &found_lengths_c,
        .values = &found_values_c,
    };

    ustore_read(&read);

    if (forward_ustore_error(env_java, error_c)) {
        ustore_arena_free(arena_c);
        return NULL;
    }

    // For small lookups its generally cheaper to allocate new Java buffers
    // and copy the data there:
    // https://stackoverflow.com/a/28799276
    // https://stackoverflow.com/a/4694102
    // result_java = (*env_java)->NewDirectByteBuffer(env_java, void* address, jlong capacity);
    jbyteArray result_java = NULL;
    if (found_lengths_c[0] != ustore_length_missing_k) {
        result_java = (*env_java)->NewByteArray(env_java, found_lengths_c[0]);
        if (result_java)
            (*env_java)->SetByteArrayRegion(env_java,
                                            result_java,
                                            0,
                                            found_lengths_c[0],
                                            (jbyte const*)(found_values_c + found_offsets_c[0]));
    }

    ustore_arena_free(arena_c);
    return result_java;
}

JNIEXPORT void JNICALL Java_cloud_unum_ustore_DataBase_00024Transaction_erase( //
    JNIEnv* env_java,
    jobject txn_java,
    jstring collection_java,
    jlong key_java) {

    ustore_database_t db_ptr_c = db_ptr(env_java, txn_java);
    if (!db_ptr_c) {
        forward_error(env_java, "Database is closed!");
        return;
    }

    ustore_transaction_t txn_ptr_c = txn_ptr(env_java, txn_java);
    ustore_collection_t collection_ptr_c = collection_ptr(env_java, db_ptr_c, collection_java);
    if ((*env_java)->ExceptionCheck(env_java))
        return;

    ustore_key_t key_c = (ustore_key_t)key_java;
    ustore_options_t options_c = ustore_options_default_k;
    ustore_arena_t arena_c = NULL;
    ustore_error_t error_c = NULL;

    struct ustore_write_t write = {
        .db = db_ptr_c,
        .error = &error_c,
        .transaction = txn_ptr_c,
        .arena = &arena_c,
        .options = options_c,
        .tasks_count = 1,
        .collections = &collection_ptr_c,
        .keys = &key_c,
    };

    ustore_write(&write);
    ustore_arena_free(arena_c);
    forward_ustore_error(env_java, error_c);
}

JNIEXPORT void JNICALL Java_cloud_unum_ustore_DataBase_00024Transaction_rollback( //
    JNIEnv* env_java,
    jobject txn_java) {

    ustore_database_t db_ptr_c = db_ptr(env_java, txn_java);
    if (!db_ptr_c) {
        forward_error(env_java, "Database is closed!");
        return;
    }

    ustore_transaction_t txn_ptr_c = txn_ptr(env_java, txn_java);
    if (!txn_ptr_c) {
        forward_error(env_java, "Transaction wasn't initialized!");
        return;
    }

    ustore_error_t error_c = NULL;
    struct ustore_transaction_init_t txn_init = {
        .db = db_ptr_c,
        .error = &error_c,
        .transaction = &txn_ptr_c,
    };

    ustore_transaction_init(&txn_init);
    forward_ustore_error(env_java, error_c);
}

JNIEXPORT jboolean JNICALL Java_cloud_unum_ustore_DataBase_00024Transaction_commit( //
    JNIEnv* env_java,
    jobject txn_java) {

    ustore_database_t db_ptr_c = db_ptr(env_java, txn_java);
    if (!db_ptr_c) {
        forward_error(env_java, "Database is closed!");
        return JNI_FALSE;
    }

    ustore_transaction_t txn_ptr_c = txn_ptr(env_java, txn_java);
    if (!txn_ptr_c) {
        forward_error(env_java, "Transaction wasn't initialized!");
        return JNI_FALSE;
    }

    ustore_options_t options_c = ustore_options_default_k;
    ustore_error_t error_c = NULL;
    struct ustore_transaction_commit_t txn_commit = {
        .db = db_ptr_c,
        .error = &error_c,
        .transaction = txn_ptr_c,
        .options = options_c,
    };

    ustore_transaction_commit(&txn_commit);
    return error_c ? JNI_FALSE : JNI_TRUE;
}