package cloud.unum.ukv;


public class DataBaseUMem extends DataBase {
    public static void init() {
        loadLibrary("umem");
    }
}

