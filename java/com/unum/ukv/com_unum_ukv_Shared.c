#include "com_unum_ukv_Shared.h"

jfieldID find_db_field(JNIEnv* env_java) {
    static jfieldID db_ptr_field = NULL;
    if (!db_ptr_field) {
        // https://docs.oracle.com/javase/8/docs/technotes/guides/jni/spec/types.html
        // https://docs.oracle.com/javase/tutorial/java/nutsandbolts/datatypes.html
        jclass txn_class_java = (*env_java)->FindClass(env_java, "com/unum/ukv/DataBase$Transaction");
        db_ptr_field = (*env_java)->GetFieldID(env_java, txn_class_java, "databaseAddress", "J");
    }
    return db_ptr_field;
}

jfieldID find_txn_field(JNIEnv* env_java) {
    static jfieldID txn_ptr_field = NULL;
    if (!txn_ptr_field) {
        // https://docs.oracle.com/javase/8/docs/technotes/guides/jni/spec/types.html
        // https://docs.oracle.com/javase/tutorial/java/nutsandbolts/datatypes.html
        jclass txn_class_java = (*env_java)->FindClass(env_java, "com/unum/ukv/DataBase$Transaction");
        txn_ptr_field = (*env_java)->GetFieldID(env_java, txn_class_java, "transactionAddress", "J");
    }
    return txn_ptr_field;
}

ukv_t db_ptr(JNIEnv* env_java, jobject txn_java) {
    jfieldID db_ptr_field = find_db_field(env_java);
    long int db_ptr_java = (*env_java)->GetLongField(env_java, txn_java, db_ptr_field);
    return (ukv_t)db_ptr_java;
}

ukv_txn_t txn_ptr(JNIEnv* env_java, jobject txn_java) {
    jfieldID txn_ptr_field = find_txn_field(env_java);
    long int txn_ptr_java = (*env_java)->GetLongField(env_java, txn_java, txn_ptr_field);
    return (ukv_txn_t)txn_ptr_java;
}

ukv_collection_t collection_ptr(JNIEnv* env_java, ukv_t db_ptr, jstring name_java) {

    // We may be passing the empty name of the default collection
    if (!name_java)
        return NULL;

    // Temporarily copy the contents of the passed string
    jboolean name_is_copy_java = JNI_FALSE;
    char const* name_c = (*env_java)->GetStringUTFChars(env_java, name_java, &name_is_copy_java);
    if ((*env_java)->ExceptionCheck(env_java))
        return NULL;

    ukv_error_t error_c = NULL;
    ukv_collection_t collection_c = NULL;
    ukv_collection_open(db_ptr, name_c, NULL, &collection_c, &error_c);

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