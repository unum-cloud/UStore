#include "com_unum_ukv_Shared.h"
#include "com_unum_ukv_DataBase_Context.h"

JNIEXPORT void JNICALL Java_com_unum_ukv_DataBase_00024Context_open(JNIEnv* env_java,
                                                                    jobject db_java,
                                                                    jstring config_java) {

    ukv_t db_ptr_c = db_ptr(env_java, db_java);
    if (db_ptr_c) {
        forward_error(env_java, "Database is already opened. Close it's current state first!");
        return;
    }

    // Temporarily copy the contents of the passed configuration string
    jboolean config_is_copy_java = JNI_FALSE;
    char const* config_c = (*env_java)->GetStringUTFChars(env_java, config_java, &config_is_copy_java);
    if ((*env_java)->ExceptionCheck(env_java))
        return;

    ukv_error_t error_c = NULL;
    ukv_open(config_c, &db_ptr_c, &error_c);

    if (config_is_copy_java == JNI_TRUE)
        (*env_java)->ReleaseStringUTFChars(env_java, config_java, config_c);

    if (forward_error(env_java, error_c))
        return;

    jfieldID db_ptr_field = find_db_field(env_java);
    (*env_java)->SetLongField(env_java, db_java, db_ptr_field, (long int)db_ptr_c);
}

JNIEXPORT jobject JNICALL Java_com_unum_ukv_DataBase_00024Context_transaction(JNIEnv* env_java, jobject db_java) {

    ukv_t db_ptr_c = db_ptr(env_java, db_java);
    ukv_txn_t txn_ptr_c = txn_ptr(env_java, db_java);
    if (!db_ptr_c) {
        forward_error(env_java, "Database is closed!");
        return NULL;
    }

    ukv_error_t error_c = NULL;
    ukv_txn_begin(db_ptr_c, 0, ukv_options_default_k, &txn_ptr_c, &error_c);
    if (forward_error(env_java, error_c))
        return NULL;

    jclass txn_class_java = (*env_java)->FindClass(env_java, "com/unum/ukv/DataBase$Transaction");
    jmethodID txn_constructor_java = (*env_java)->GetMethodID(env_java, txn_class_java, "<init>", "()V");
    jobject txn_java = (*env_java)->NewObject(env_java, txn_class_java, txn_constructor_java);

    // Initialize its properties
    jfieldID db_ptr_field = find_db_field(env_java);
    jfieldID txn_ptr_field = find_txn_field(env_java);
    (*env_java)->SetLongField(env_java, txn_java, db_ptr_field, (long int)db_ptr_c);
    (*env_java)->SetLongField(env_java, txn_java, txn_ptr_field, (long int)txn_ptr_c);

    return txn_java;
}

JNIEXPORT void JNICALL Java_com_unum_ukv_DataBase_00024Context_close_1(JNIEnv* env_java, jobject db_java) {

    ukv_t db_ptr_c = db_ptr(env_java, db_java);
    if (!db_ptr_c)
        // The DB is already closed
        return;

    // Overwrite the field first, to avoid multiple deallocations
    jfieldID db_ptr_field = find_db_field(env_java);
    (*env_java)->SetLongField(env_java, db_java, db_ptr_field, (long int)0);

    // Then actually dealloc
    ukv_free(db_ptr_c);
}

JNIEXPORT void JNICALL Java_com_unum_ukv_DataBase_00024Context_clear__(JNIEnv* env_java, jobject db_java) {

    ukv_t db_ptr_c = db_ptr(env_java, db_java);
    ukv_error_t error_c = NULL;
    if (!db_ptr_c) {
        forward_error(env_java, "Database is closed!");
        return;
    }

    ukv_collection_remove(db_ptr_c, NULL, &error_c);
    forward_ukv_error(env_java, error_c);
}