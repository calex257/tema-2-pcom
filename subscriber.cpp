#include <assert.h>
#include <sys/errno.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/epoll.h>
#include <string.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <netdb.h>

#include "common.h"
#include "debug.h"

#define EXITRC 1
#define SUBRC 2
#define UNSUBRC 3
#define UNKNOWN_COMMAND -1

#define SIGN_BIT (1<<31)

struct epoll_event events[10];
int tcp_socket = -1;
int epoll_instance;
uint32_t bytes_received;
int running = 1;
uint32_t recv_msg_len;
int action;
char* client_id;
const int yes = 1;
char user_input[256] = { 0 };
char buf[BUFSIZ] = { 0 };
char auxbuf[BUFSIZ] = { 0 };
struct sockaddr_in myaddr;
socklen_t myaddrlen = sizeof(struct sockaddr_in);

const char data_names[4][14] = {
    "INT",
    "SHORT_REAL",
    "FLOAT",
    "STRING"
};

struct command_args {
    char* command_name;
    char* command_arg[2];
};

void connect_to_server(char* addr, char* port);
void create_tcp_socket();
void main_loop();
void cleanup();
void send_unsubscribe_request(struct command_args* c_args);
void send_subscribe_request(struct command_args* c_args);
void prepare_for_connection(struct message_header* hdr);
int handle_user_input(struct command_args* c_args);

void create_tcp_socket()
{
    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(tcp_socket < 0, "socket", FATAL);
    int rc = setsockopt(tcp_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    CHECK(rc < 0, "setsockopt", FATAL);
    rc = setsockopt(tcp_socket, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    CHECK(rc < 0, "setsockopt", FATAL);
}

void connect_to_server(char* addr, char* port)
{
    struct addrinfo hints, * results;
    memset(&hints, 0, sizeof(hints));

    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV | AI_PASSIVE;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(addr, port, &hints, &results);
    CHECK(rc < 0, "getaddrinfo", FATAL);
    rc = connect(tcp_socket, results->ai_addr, results->ai_addrlen);
    CHECK(rc < 0, "connect", FATAL);
    freeaddrinfo(results);
}

void prepare_for_connection(struct message_header* hdr)
{
    memset((char*)hdr, 0, sizeof(*hdr));
    hdr->msg_len = sizeof(struct message_header);
    hdr->flags = CONN_REQ;
    memcpy(hdr->id, client_id, strlen(client_id));
}

/**
 * @brief
 * Gestioneaza primirea de comenzi de la user.
 *
 * @param c_args
 * structura in care se retin argumentele trimise
 * @return
 * O valoare legata de tipul comenzii date
 */
int handle_user_input(struct command_args* c_args)
{
    int rc = read(STDIN_FILENO, user_input, sizeof(user_input) - 1);
    user_input[rc] = 0;
    c_args->command_name = strtok(user_input, " \n");
    if (c_args->command_name == NULL) {
        return UNKNOWN_COMMAND;
    }
    if (strcmp(c_args->command_name, "exit") == 0) {
        return EXITRC;
    }
    c_args->command_arg[0] = strtok(NULL, " \n");
    if (c_args->command_arg[0] == NULL) {
        return UNKNOWN_COMMAND;
    }
    if (strcmp(c_args->command_name, "unsubscribe") == 0) {
        printf("Unsubscribed from topic.\n");
        return UNSUBRC;
    }
    c_args->command_arg[1] = strtok(NULL, " \n");
    if (c_args->command_arg[1] == NULL) {
        return UNKNOWN_COMMAND;
    }
    if (strcmp(c_args->command_name, "subscribe") == 0) {
        printf("Subscribed to topic.\n");
        return SUBRC;
    }
    return UNKNOWN_COMMAND;
}

/**
 * @brief
 * Trimite serverului un mesaj de subscribe
 * pe baza comenzii primite
 *
 * @param c_args
 * Comanda primita
 */
void send_subscribe_request(struct command_args* c_args)
{
    struct message_header subscribe_message;
    memset((char*)(&subscribe_message), 0, sizeof(subscribe_message));
    subscribe_message.msg_len = sizeof(struct message_header) + strlen(c_args->command_arg[0]);
    subscribe_message.flags = (c_args->command_arg[1][0] == '0' ? SUB_NOSF : SUB_SF);
    memcpy(subscribe_message.id, client_id, strlen(client_id));
    int rc = send(tcp_socket, &subscribe_message, sizeof(subscribe_message), MSG_MORE);
    CHECK(rc < 0, "send", FATAL);
    rc = send(tcp_socket, c_args->command_arg[0], strlen(c_args->command_arg[0]), 0);
    CHECK(rc < 0, "send", FATAL);
}

/**
 * @brief
 * Trimite serverului un mesaj de subscribe
 * pe baza comenzii primite
 *
 * @param c_args
 * Comanda primita
 */
void send_unsubscribe_request(struct command_args* c_args)
{
    struct message_header unsubscribe_message;
    unsubscribe_message.msg_len = sizeof(struct message_header) + strlen(c_args->command_arg[0]);
    unsubscribe_message.flags = UNSUB;
    memcpy(unsubscribe_message.id, client_id, strlen(client_id));
    int rc = send(tcp_socket, &unsubscribe_message, sizeof(unsubscribe_message), 0);
    CHECK(rc < 0, "send", FATAL);
    usleep(10000);
    rc = send(tcp_socket, c_args->command_arg[0], strlen(c_args->command_arg[0]), 0);
    CHECK(rc < 0, "send", FATAL);
}

/**
 * @brief
 * Afiseaza un mesaj de tip int
 *
 * @param buf
 * bufferul in care se afla mesajul
 */
void disp_int(char* buf)
{
    int32_t val = ntohl(*(uint32_t*)(buf + 1));
    val = (*(uint8_t*)buf) == 0 ? val : -val;
    printf("%d\n", val);
}

/**
 * @brief
 * Afiseaza un mesaj de tip short real
 *
 * @param buf
 * bufferul in care se afla mesajul
 */
void disp_short_real(char* buf)
{
    uint16_t val = ntohs(*(uint16_t*)buf);
    printf("%.2f\n", val / 100.0);
}


/**
 * @brief
 * Afiseaza un mesaj de tip float
 *
 * @param buf
 * bufferul in care se afla mesajul
 */
void disp_float(char* buf)
{
    int32_t val = ntohl(*(uint32_t*)(buf + 1));
    float fval = (float)val;
    uint8_t expt = (*(uint8_t*)(buf + 5));
    for (int i = 0; i < expt;i++) {
        fval /= 10;
    }
    fval = (*(uint8_t*)buf) == 0 ? fval : -fval;
    printf("%.*f\n", expt, fval);
}

/**
 * @brief
 * Afiseaza un mesaj de tip string
 *
 * @param buf
 * bufferul in care se afla mesajul
 */
void disp_string(char* buf)
{
    write(STDOUT_FILENO, buf, recv_msg_len - sizeof(struct message_header) -
        sizeof(struct udp_header) - sizeof(struct udp_message));
    printf("\n");
}

/**
 * @brief
 * Imparte mesajul primit de la server in
 * ip/port udp, topic + tip de date si restul
 * continutului.
 */
void parse_server_message()
{
    struct udp_header* udp_hdr = (struct udp_header*)(buf + sizeof(struct message_header));
    struct udp_message* udp_msg = (struct udp_message*)(udp_hdr + 1);
    char* payload = (char*)(udp_msg + 1);
    printf("%s:%d - ", inet_ntoa({ udp_hdr->addr }), ntohs(udp_hdr->port));
    char topic[51] = { 0 };
    memcpy(topic, udp_msg->topic, 50);
    printf("%s - %s - ", topic, data_names[udp_msg->data_type]);
    switch (udp_msg->data_type) {
    case INT:
        disp_int(payload);
        break;
    case SHORT_REAL:
        disp_short_real(payload);
        break;
    case FLOAT:
        disp_float(payload);
        break;
    case STRING:
        disp_string(payload);
        break;
    }
}

/**
 * @brief
 * Se trateaza raspunsul in functie de tipul actiunii specificate
 * in header
 */
void handle_response()
{
    switch (action) {
    case CONN_NO:
        running = 0;
        break;
    case CONN_YES:

        break;
    case CONN_END:
        close(tcp_socket);
        epoll_ctl(epoll_instance, EPOLL_CTL_DEL, tcp_socket, NULL);
        running = 0;
        break;
    case MSG_NEW:
        parse_server_message();
        break;
    }
}

void split_message()
{
    int cont = bytes_received;
    while (cont > 0) {
        if (recv_msg_len == 0) {
            if (cont < 8) {
                bytes_received = cont;
                return;
            }
            else {
                struct message_header* mess = (struct message_header*)buf;
                recv_msg_len = mess->msg_len;
                DWRITE(buf, sizeof(struct message_header));
                action = mess->flags;
                cont -= sizeof(struct message_header);
            }
        }
        else {
            int content_length = recv_msg_len - sizeof(struct message_header);
            if (content_length <= cont) {
                DWRITE(buf, cont);
                handle_response();
                recv_msg_len = 0;
                memmove(buf, buf + recv_msg_len, cont - content_length);
                cont -= content_length;
            }
            else {
                bytes_received = cont;
                return;
            }
        }
    }
    bytes_received = 0;
}

/**
 * @brief
 * Gestioneaza primirea de bytes de la server.
 * Se primeste mai intai header-ul in intregime si apoi
 * continutul daca exista. Daca s-a primit tot continutul,
 * mesajul este gata de a fi tratat
 */
void handle_server_input()
{
    uint32_t bytes_read;
    if (recv_msg_len == 0) {
        bytes_read = recv(tcp_socket, buf, sizeof(struct message_header), MSG_WAITALL);
        struct message_header* mess = (struct message_header*)buf;
        recv_msg_len = mess->msg_len;
        DWRITE(buf, sizeof(struct message_header));
        action = mess->flags;
        if (recv_msg_len == sizeof(struct message_header)) {
            handle_response();
        }
        return;
    }
    bytes_read = recv(tcp_socket, buf + sizeof(struct message_header) + bytes_received,
        recv_msg_len - sizeof(struct message_header) - bytes_received, 0);
    if (bytes_read == 0) {
        exit(101);
    }
    bytes_received += bytes_read;
    if (bytes_received == recv_msg_len - sizeof(struct message_header)) {
        DWRITE(buf, sizeof(struct message_header));
        handle_response();
        recv_msg_len = 0;
        bytes_received = 0;
    }
}

/**
 * @brief
 * Trimite un mesaj prin care sa anunte serverul ca
 * acest client se deconecteaza
 */
void send_disconnect_msg()
{
    struct message_header disconnect_header;
    memset(&disconnect_header, 0, sizeof(disconnect_header));
    disconnect_header.flags = CONN_END;
    disconnect_header.msg_len = sizeof(disconnect_header);
    memcpy(disconnect_header.id, client_id, strlen(client_id));
    DWRITE(((char*)&disconnect_header), sizeof(disconnect_header));
    int rc = send(tcp_socket, &disconnect_header, sizeof(disconnect_header), 0);
    CHECK(rc < 0, "send", FATAL);
}

/**
 * @brief
 * Bucla principala a programului in care astept
 * evenimentele semnalate de epoll pentru a le trata
 */
void main_loop()
{
    struct message_header connection_message;
    struct command_args cmd = { NULL, {NULL, NULL} };
    prepare_for_connection(&connection_message);
    int rc = getsockname(tcp_socket, (struct sockaddr*)&myaddr, &myaddrlen);
    CHECK(rc < 0, "getsockname", FATAL);
    rc = send(tcp_socket, &connection_message, sizeof(connection_message), 0);
    CHECK(rc < 0, "send", FATAL);
    while (running) {
        int nr_events = epoll_wait(epoll_instance, events, sizeof(events) / sizeof(struct epoll_event), -1);
        for (int i = 0; i < nr_events; i++) {
            if (events[i].data.fd == STDIN_FILENO) {
                int rc = handle_user_input(&cmd);
                switch (rc) {
                case EXITRC:
                    running = 0;
                    send_disconnect_msg();
                    break;
                case SUBRC:
                    send_subscribe_request(&cmd);
                    break;
                case UNSUBRC:
                    send_unsubscribe_request(&cmd);
                    break;
                case UNKNOWN_COMMAND:
                    fprintf(stderr, "unknown command\n");
                    break;
                }
            }
            else {
                handle_server_input();
            }
        }
    }
}

void cleanup()
{
    close(tcp_socket);
}

int main(int argc, char* argv[])
{
    if (argc < 4) {
        fprintf(stderr, "not enough arguments\n");
        exit(1);
    }

    client_id = argv[1];
    if (strlen(argv[1]) > 10) {
        fprintf(stderr, "id too long\n");
        exit(1);
    }

    epoll_instance = epoll_create1(0);
    CHECK(epoll_instance < 0, "epoll", FATAL);

    add_to_epollin(epoll_instance, STDIN_FILENO);

    create_tcp_socket();
    connect_to_server(argv[2], argv[3]);
    add_to_epollin(epoll_instance, tcp_socket);

    setvbuf(stdout, NULL, _IONBF, BUFSIZ);
    setvbuf(stdin, NULL, _IONBF, BUFSIZ);

    main_loop();
    cleanup();

    return 0;
}