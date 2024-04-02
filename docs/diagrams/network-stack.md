# Stratum Original Network Stack Architecture

```mermaid
flowchart TD
    subgraph HardwareLayer["Virtio Network Device"]
        NetCard["PCI Virtio-Net Device (modern 1af4:1041)"]
        RxRing["Split Virtqueue Receive Ring"]
        TxRing["Split Virtqueue Transmit Ring"]
        NetCard <--> RxRing
        NetCard <--> TxRing
    end

    subgraph LinkLayer["Link & Resolution Layer"]
        RxRing --> EthDemux["Ethernet II Frame Demux"]
        EthDemux --> EtherType{"EtherType Check"}
        EtherType -- "0x0806 (ARP)" --> ARP["ARP Packet Handler & Dynamic Cache"]
        EtherType -- "0x0800 (IPv4)" --> IPv4["IPv4 Packet Handler"]
        TxRing <-- EthEncap["Ethernet II Frame Encapsulation"]
        ARP --> EthEncap
    end

    subgraph NetworkTransport["Network and Transport Protocols"]
        IPv4 --> IPVerify["Verify Version, IHL, Total Length, Checksum"]
        IPVerify --> IPProto{"IP Protocol"}
        IPProto -- "1 (ICMP)" --> ICMP["ICMP Echo Request Responder"]
        IPProto -- "17 (UDP)" --> UDP["UDP Datagram Port Demux"]
        IPProto -- "6 (TCP)" --> TCP["TCP State Machine Engine"]
        ICMP --> IPEncap["IPv4 Header Build & Checksum"]
        UDP --> IPEncap
        TCP --> IPEncap
        IPEncap --> EthEncap
    end

    subgraph SocketAPI["BSD Socket Subsystem (File Descriptors)"]
        TCP <--> Sockets["BSD Socket Table (SOCK_STREAM / SOCK_DGRAM)"]
        Sockets <--> Syscalls["Syscalls: socket, bind, listen, accept, connect, send, recv"]
        Syscalls <--> UserApp["Native record service"]
    end
```
