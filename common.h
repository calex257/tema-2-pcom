#include <stdint.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <netdb.h>
#include <vector>
#include <netinet/tcp.h>

#ifndef COMMON_H
#define COMMON_H

#define min(a,b) (((a)<(b))?(a):(b))
#define max(a,b) (((a)>(b))?(a):(b))

#define BROADCAST (char*)"0.0.0.0"

enum actions {
    /* cerere de conexiune */
    CONN_REQ = 0x0000,
    /* raspuns afirmativ */
    CONN_YES = 0x0001,
    /* raspuns negativ */
    CONN_NO = 0x0002,
    /* subscribe fara sf */
    SUB_NOSF = 0x0004,
    /* subscribe cu sf */
    SUB_SF = 0x0008,
    /* unsubscribe */
    UNSUB = 0x0010,
    /* mesaj nou */
    MSG_NEW = 0x0020,
    /* continuarea unui mesaj - nefolosit */
    MSG_CONT = 0x0040,
    /* inchiderea conexiunii */
    CONN_END = 0x0080,
    /* exista continuare a datelor - nefolosit */
    HAS_MORE_DATA = 0x0100,
};

enum data_types {
    INT = 0,
    SHORT_REAL = 1,
    FLOAT = 2,
    STRING = 3,
};

struct message_header {
    char id[14];
    uint16_t flags;
    uint32_t msg_len;
};

struct udp_header {
    uint32_t addr;
    uint16_t port;
};

struct udp_message {
    char topic[50];
    uint8_t data_type;
};

struct udp_content {
    int act_len;
    char data[1550];
};

struct tcp_client {
    /* File descriptorul aferent clientului */
    int sockfd;
    /* Camp nefolosit */
    int subcount;
    /* Numarul de bytes primiti pana la un moment dat */
    int bytes_received;
    /* Actiunea in desfasurare pentru client */
    int action;
    /* 0 - deconectat, 1 - conectat */
    int is_connected;
    /* Lungimea mesajului care urmeaza citit */
    int last_msg_len;
    /* Campuri pentru adresa IP a clientului */
    struct sockaddr_in addr;
    socklen_t addr_len;
    /* ID-ul clientului */
    char id[64];
    /* Buffer-ul intern in care se retin mesajele primite de la client */
    char inbuf[256];
};

struct subscriber {
    struct tcp_client* client_s;
    int sf;
    int last_msg_received;
};

struct msg_to_send {
    struct udp_header hdr;
    struct udp_message umsg;
    struct udp_content content;
};

struct topic {
    /* Mesajele trimise pe un anumit topic */
    std::vector<msg_to_send> messages;
    /* Lista de subscriberi a unui topic */
    std::vector<subscriber> subs;
    /* Numarul de subscriberi cu SF pe 1 */
    int sf_count;
};


void add_to_epollin(int epoll_inst, int fd);


#endif