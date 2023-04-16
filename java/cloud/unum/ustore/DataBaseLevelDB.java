package cloud.unum.ustore;

public class DataBaseLevelDB extends DataBase {
    public static void init() {
        loadLibrary("leveldb");
    }
}
