package com.unum.ukv;

public class DataBaseRocksDB extends DataBase {
    public static void init() {
        loadLibrary("rocksdb");
    }
}
