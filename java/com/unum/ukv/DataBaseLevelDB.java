package com.unum.ukv;

public class DataBaseLevelDB extends DataBase {
    public static void init() {
        loadLibrary("leveldb");
    }
}
