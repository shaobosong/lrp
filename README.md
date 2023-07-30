# lrp
a lightweight reverse proxy daemon

# Build
make

# Usage
```bash
lrpd [-dh] [-p port]
lrp [-dh] [-4 serv_addr] [-p serv_port] -c tcp::serv_port-[local_addr]:local_port [-c ...]
```

# Example
```bash
./lrpd -d -p 2023
./lrp -d -4 127.0.0.1 -p 2023 -c tcp::60022-:22
```  
