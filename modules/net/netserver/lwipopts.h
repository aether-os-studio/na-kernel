#pragma once

#define NO_SYS 0
#define SYS_LIGHTWEIGHT_PROT 1
#define LWIP_COMPAT_MUTEX 0

#define SYS_ARCH_INC(var, val)                                                 \
    __atomic_fetch_add(&(var), (__typeof__(var))(val), __ATOMIC_RELAXED)
#define SYS_ARCH_DEC(var, val)                                                 \
    __atomic_fetch_sub(&(var), (__typeof__(var))(val), __ATOMIC_RELAXED)
#define SYS_ARCH_GET(var, ret)                                                 \
    do {                                                                       \
        (ret) = __atomic_load_n(&(var), __ATOMIC_ACQUIRE);                     \
    } while (0)

#define MEM_ALIGNMENT 8
#define MEM_LIBC_MALLOC 0
#define MEMP_MEM_MALLOC 0
#define MEM_SIZE (8 * 1024 * 1024)

#define LWIP_NETCONN 1
#define LWIP_SOCKET 0
#define LWIP_NETCONN_SEM_PER_THREAD 1
#define LWIP_NETCONN_FULLDUPLEX 1
#define LWIP_NETCONN_THREAD_SEM_GET() naos_lwip_thread_sem_get()
#define LWIP_NETCONN_THREAD_SEM_ALLOC() naos_lwip_thread_sem_alloc()
#define LWIP_NETCONN_THREAD_SEM_FREE() naos_lwip_thread_sem_free()
#define LWIP_NETIF_API 1
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NETIF_STATUS_CALLBACK 1

#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_IPV4 1
#define LWIP_IPV6 1
#define LWIP_ICMP 1
#define LWIP_RAW 1
#define LWIP_UDP 1
#define LWIP_TCP 1
#define LWIP_DNS 1
#define LWIP_DNS_ADDRTYPE_DEFAULT LWIP_DNS_ADDRTYPE_IPV4
#define LWIP_DHCP 1
#define DNS_MAX_SERVERS 2
#define LWIP_DHCP_MAX_DNS_SERVERS 2
#define LWIP_AUTOIP 1
#define LWIP_NETBUF_RECVINFO 1
/* Avoid unstable IPv6 fragment/reassembly paths while DNS is brought up. */
#define LWIP_IPV6_REASS 0
#define LWIP_IPV6_FRAG 0

#define LWIP_SOCKET_SELECT 0
#define LWIP_SOCKET_POLL 0

#define LWIP_SO_RCVTIMEO 1
#define LWIP_SO_SNDTIMEO 1
#define LWIP_SO_RCVBUF 1
#define LWIP_SO_LINGER 1
#define SO_REUSE 1
#define SO_REUSE_RXTOALL 1
#define LWIP_TCP_KEEPALIVE 1
#define IP_SOF_BROADCAST 1
#define IP_SOF_BROADCAST_RECV 1

#define LWIP_RANDOMIZE_INITIAL_LOCAL_PORTS 1
#define LWIP_SINGLE_NETIF 0

#define TCPIP_THREAD_STACKSIZE 0
#define TCPIP_THREAD_PRIO 0
#define TCPIP_MBOX_SIZE 1024
#define DEFAULT_UDP_RECVMBOX_SIZE 256
#define DEFAULT_TCP_RECVMBOX_SIZE 256
#define DEFAULT_ACCEPTMBOX_SIZE 64

#define MEMP_NUM_TCPIP_MSG_API 256
#define MEMP_NUM_TCPIP_MSG_INPKT 1024
#define MEMP_NUM_NETCONN 256
#define MEMP_NUM_TCP_PCB 256
#define MEMP_NUM_TCP_PCB_LISTEN 32
#define MEMP_NUM_TCP_SEG 2048
#define MEMP_NUM_UDP_PCB 128
#define MEMP_NUM_RAW_PCB 16
#define MEMP_NUM_NETBUF 256
#define MEMP_NUM_PBUF 1024
#define PBUF_POOL_SIZE 2048
#define PBUF_POOL_BUFSIZE 1700

#define TCP_MSS 1460
#define LWIP_WND_SCALE 1
#define TCP_RCV_SCALE 2
#define TCP_WND (128 * TCP_MSS)
#define TCP_SND_BUF (64 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * TCP_SND_BUF + TCP_MSS - 1) / TCP_MSS)
#define TCP_OOSEQ_MAX_BYTES (64 * TCP_MSS)
#define TCP_OOSEQ_MAX_PBUFS 64

/* Keep packet ingestion out of application-thread core-lock contention. */
#define LWIP_TCPIP_CORE_LOCKING_INPUT 0

#define LWIP_STATS 0
#define LWIP_DEBUG 0
