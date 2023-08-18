import java.io.File;
import java.util.Arrays;

import org.junit.Test;

import cloud.unum.ustore.DataBaseLevelDB;

public class DataBaseLevelDBTest {
    static {
        DataBaseLevelDB.init();
    }

    public static void deleteDirectoryFiles(String path) {
        File directory = new File(path);
        if (!directory.isDirectory())
            return;
        
        for (File f : directory.listFiles())
            f.delete();
    }

    @Test
    public void test() {
        String path = System.getenv("USTORE_TEST_PATH");
        if(path == null || path.trim().isEmpty())
            path = "./tmp/";

        deleteDirectoryFiles(path);
        String config = String.format("{\"version\": \"1.0\", \"directory\": \"%s\"}", path);
        DataBaseLevelDB.Context ctx = new DataBaseLevelDB.Context(config);

        ctx.put(42, "hey".getBytes());
        assert Arrays.equals(ctx.get(42), "hey".getBytes()) : "Received wrong value";

        ctx.close();
        System.out.println("Success!");
    }
}
