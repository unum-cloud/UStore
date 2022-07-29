kill $(lsof -t -i:8080)
cmake . && make && ./build/bin/ukv_beast_server 0.0.0.0 8080 1
