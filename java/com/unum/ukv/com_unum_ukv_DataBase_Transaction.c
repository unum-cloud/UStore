#include "com_unum_ukv_Shared.h"
#include "com_unum_ukv_DataBase_Transaction.h"

JNIEXPORT void JNICALL Java_com_unum_ukv_DataBase_00024Transaction_put( //
    JNIEnv* env_java,
    jobject txn_java,
    jstring collection_java,
    jlong key_java,
    jbyteArray value_java) {

    ukv_t db_ptr_c = db_ptr(env_java, txn_java);
    if (!db_ptr_c) {
        forward_error(env_java, "Database is closed!");
        return;
    }

    ukv_txn_t txn_ptr_c = txn_ptr(env_java, txn_java);
    ukv_collection_t collection_ptr_c = collection_ptr(env_java, db_ptr_c, collection_java);
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
    ukv_key_t key_c = (ukv_key_t)key_java;
    ukv_val_ptr_t found_values_c = (ukv_val_ptr_t)value_ptr_java;
    ukv_val_len_t value_off_c = 0;
    ukv_val_len_t value_len_c = (ukv_val_len_t)value_len_java;
    ukv_options_t options_c = ukv_options_default_k;
    ukv_arena_t arena_c = NULL;
    ukv_error_t error_c = NULL;

    ukv_write(db_ptr_c,
              txn_ptr_c,
              1,
              &collection_ptr_c,
              0,
              &key_c,
              0,
              &found_values_c,
              0,
              &value_off_c,
              0,
              &value_len_c,
              0,
              options_c,
              &arena_c,
              &error_c);
    ukv_arena_free(db_ptr_c, arena_c);

    if (value_is_copy_java == JNI_TRUE)
        (*env_java)->ReleaseByteArrayElements(env_java, value_java, value_ptr_java, 0);
    forward_ukv_error(env_java, error_c);
}

JNIEXPORT jboolean JNICALL Java_com_unum_ukv_DataBase_00024Transaction_containsKey( //
    JNIEnv* env_java,
    jobject txn_java,
    jstring collection_java,
    jlong key_java) {

    ukv_t db_ptr_c = db_ptr(env_java, txn_java);
    if (!db_ptr_c) {
        forward_error(env_java, "Database is closed!");
        return JNI_FALSE;
    }

    ukv_txn_t txn_ptr_c = txn_ptr(env_java, txn_java);
    ukv_collection_t collection_ptr_c = collection_ptr(env_java, db_ptr_c, collection_java);
    if ((*env_java)->ExceptionCheck(env_java))
        return JNI_FALSE;

    ukv_key_t key_c = (ukv_key_t)key_java;
    ukv_options_t options_c = ukv_option_read_lengths_k;
    ukv_val_len_t* found_lengths_c = NULL;
    ukv_val_ptr_t found_values_c = NULL;
    ukv_arena_t arena_c = NULL;
    ukv_error_t error_c = NULL;

    ukv_read(db_ptr_c,
             txn_ptr_c,
             1,
             &collection_ptr_c,
             0,
             &key_c,
             0,
             options_c,
             &found_lengths_c,
             &found_values_c,
             &arena_c,
             &error_c);

    if (forward_error(env_java, error_c)) {
        ukv_arena_free(db_ptr_c, arena_c);
        return JNI_FALSE;
    }

    jboolean result = found_lengths_c[0] != ukv_val_len_missing_k ? JNI_TRUE : JNI_FALSE;
    ukv_arena_free(db_ptr_c, arena_c);
    return result;
}

JNIEXPORT jbyteArray JNICALL Java_com_unum_ukv_DataBase_00024Transaction_get( //
    JNIEnv* env_java,
    jobject txn_java,
    jstring collection_java,
    jlong key_java) {

    ukv_t db_ptr_c = db_ptr(env_java, txn_java);
    if (!db_ptr_c) {
        forward_error(env_java, "Database is closed!");
        return NULL;
    }

    ukv_txn_t txn_ptr_c = txn_ptr(env_java, txn_java);
    ukv_collection_t collection_ptr_c = collection_ptr(env_java, db_ptr_c, collection_java);
    if ((*env_java)->ExceptionCheck(env_java))
        return NULL;

    ukv_key_t key_c = (ukv_key_t)key_java;
    ukv_options_t options_c = ukv_options_default_k;
    ukv_val_len_t* found_lengths_c = NULL;
    ukv_val_ptr_t found_values_c = NULL;
    ukv_arena_t arena_c = NULL;
    ukv_error_t error_c = NULL;

    ukv_read(db_ptr_c,
             txn_ptr_c,
             1,
             &collection_ptr_c,
             0,
             &key_c,
             0,
             options_c,
             &found_lengths_c,
             &found_values_c,
             &arena_c,
             &error_c);

    if (forward_ukv_error(env_java, error_c)) {
        ukv_arena_free(db_ptr_c, arena_c);
        return NULL;
    }

    // For small lookups its jenerally cheaper to allocate new Java buffers
    // and copy the data there:
    // https://stackoverflow.com/a/28799276
    // https://stackoverflow.com/a/4694102
    // result_java = (*env_java)->NewDirectByteBuffer(env_java, void* address, jlong capacity);
    jbyteArray result_java = NULL;
    if (found_lengths_c[0] != ukv_val_len_missing_k) {
        result_java = (*env_java)->NewByteArray(env_java, found_lengths_c[0]);
        if (result_java)
            (*env_java)->SetByteArrayRegion(env_java, result_java, 0, found_lengths_c[0], (jbyte const*)found_values_c);
    }

    ukv_arena_free(db_ptr_c, arena_c);
    return result_java;
}

JNIEXPORT void JNICALL Java_com_unum_ukv_DataBase_00024Transaction_erase( //
    JNIEnv* env_java,
    jobject txn_java,
    jstring collection_java,
    jlong key_java) {

    ukv_t db_ptr_c = db_ptr(env_java, txn_java);
    if (!db_ptr_c) {
        forward_error(env_java, "Database is closed!");
        return;
    }

    ukv_txn_t txn_ptr_c = txn_ptr(env_java, txn_java);
    ukv_collection_t collection_ptr_c = collection_ptr(env_java, db_ptr_c, collection_java);
    if ((*env_java)->ExceptionCheck(env_java))
        return;

    ukv_key_t key_c = (ukv_key_t)key_java;
    ukv_val_ptr_t found_values_c = NULL;
    ukv_val_len_t value_off_c = 0;
    ukv_val_len_t value_len_c = 0;
    ukv_options_t options_c = ukv_options_default_k;
    ukv_arena_t arena_c = NULL;
    ukv_error_t error_c = NULL;

    ukv_write(db_ptr_c,
              txn_ptr_c,
              1,
              &collection_ptr_c,
              0,
              &key_c,
              0,
              &found_values_c,
              0,
              &value_off_c,
              0,
              &value_len_c,
              0,
              options_c,
              &arena_c,
              &error_c);
    ukv_arena_free(db_ptr_c, arena_c);
    forward_ukv_error(env_java, error_c);
}

JNIEXPORT void JNICALL Java_com_unum_ukv_DataBase_00024Transaction_rollback( //
    JNIEnv* env_java,
    jobject txn_java) {

    ukv_t db_ptr_c = db_ptr(env_java, txn_java);
    if (!db_ptr_c) {
        forward_error(env_java, "Database is closed!");
        return;
    }

    ukv_txn_t txn_ptr_c = txn_ptr(env_java, txn_java);
    if (!txn_ptr_c) {
        forward_error(env_java, "Transaction wasn't initialized!");
        return;
    }

    ukv_error_t error_c = NULL;
    ukv_txn_begin(db_ptr_c, 0, ukv_options_default_k, &txn_ptr_c, &error_c);
    forward_ukv_error(env_java, error_c);
}

JNIEXPORT jboolean JNICALL Java_com_unum_ukv_DataBase_00024Transaction_commit( //
    JNIEnv* env_java,
    jobject txn_java) {

    ukv_txn_t txn_ptr_c = txn_ptr(env_java, txn_java);
    if (!txn_ptr_c) {
        forward_error(env_java, "Transaction wasn't initialized!");
        return JNI_FALSE;
    }

    ukv_options_t options_c = ukv_options_default_k;
    ukv_error_t error_c = NULL;
    ukv_txn_commit(txn_ptr_c, options_c, &error_c);
    return error_c ? JNI_FALSE : JNI_TRUE;
}