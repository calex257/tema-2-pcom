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
#include <unordered_map>
#include <netinet/in.h>
#include <algorithm>
#include <stdlib.h>
#include <netdb.h>
#include <netinet/tcp.h>

#include "common.h"
#include "debug.h"

#define SF_YES 1
#define SF_NO 0

#define MAX_UDP_LEN 1600

#define EXITRC 1
#define UNKNOWN_COMMAND -1

struct epoll_event events[1000];
int udp_socket = -1, tcp_socket = -1;
int epoll_instance;
const int yes = 1;
const int no = 0;
char inbuf[MAX_UDP_LEN];
char outbuf[BUFSIZ];
// struct tcp_client* clients[100];
std::vector<struct tcp_client*> clients;
int client_count = 0;
std::vector<struct connection*> conns;
std::unordered_map<std::string, struct tcp_client*> cls;
std::unordered_map<std::string, struct topic> topics;

void cleanup();
void main_loop();
void print_clients();
void create_udp_socket(char* port);
void bind_to_addr(int sock, char* addr, char* port);
void create_tcp_socket(char* port);
void handle_incoming_message();
int handle_input(char* buf, int maxlen);

struct tcp_client* new_client()
{
    struct tcp_client* nc = (struct tcp_client*)calloc(1, sizeof(*nc));
    nc->sockfd = -1;
    nc->addr_len = sizeof(struct sockaddr_in);
    return nc;
}

struct tcp_client* find_client_by_fd(int fd)
{
    for (int i = 0;i < client_count;i++) {
        if (clients[i]->sockfd == fd) {
            return clients[i];
        }
    }
    return NULL;
}

/**
 * @brief
 * Trimite mesajul primit de la un client udp
 * la clientii conectati. Daca topicul mesajului
 * nu exista, este instantiat, iar daca exista
 * clienti abonati cu sf pe 1, mesajul este stocat.
 *
 * @param msg
 * Header-ul protocolului propriu pentru mesajul care
 * trebuie primit.
 * @param udp_hdr
 * Structura care contine adresa si portul unui client udp
 */
void send_to_clients(struct message_header* msg, struct udp_header* udp_hdr)
{
    int buflen = msg->msg_len - sizeof(*msg) - sizeof(*udp_hdr);
    struct udp_message* msg_begin = (struct udp_message*)inbuf;
    struct udp_content content;
    content.act_len = buflen - sizeof(*msg_begin);
    memcpy(content.data, msg_begin + 1, content.act_len);
    char topstr[51] = { 0 };
    memcpy(topstr, msg_begin->topic, 50);
    if (topics.count(std::string(topstr)) == 0) {
        struct topic new_topic { {}, { }, 0 };
        topics.emplace(std::string(topstr), new_topic);
        // return;
    }
    auto& topi = topics.at(std::string(topstr));
    if (topi.sf_count != 0) {
        topi.messages.push_back({
            *udp_hdr,
            *msg_begin,
            content
            });
    }
    for (auto& sub : topi.subs) {
        if (sub.client_s->is_connected) {
            sub.last_msg_received = topi.messages.size() - 1;
            int sck = sub.client_s->sockfd;
            send(sck, msg, sizeof(*msg), 0);
            send(sck, udp_hdr, sizeof(*udp_hdr), 0);
            send(sck, inbuf, buflen, 0);
        }
    }
}

/**
 * @brief
 * Trateaza un mesaj primit de la un client udp.
 */
void handle_incoming_message()
{
    struct sockaddr_in sender;
    socklen_t sender_len = sizeof(sender);
    struct message_header new_msg;

    memset(&new_msg, 0, sizeof(new_msg));
    new_msg.flags = MSG_NEW;
    int rc = recvfrom(udp_socket, inbuf, MAX_UDP_LEN, 0, (struct sockaddr*)&sender, &sender_len);
    CHECK(rc < 0, "recvfrom", FATAL);
    struct udp_header udp_data;
    memset(&udp_data, 0, sizeof(udp_data));

    // adresa si portul se transmit as is
    udp_data.addr = sender.sin_addr.s_addr;
    udp_data.port = sender.sin_port;
    new_msg.msg_len = sizeof(new_msg) + sizeof(udp_data) + rc;

    // trimite mesajul la toti clientii abonati
    send_to_clients(&new_msg, &udp_data);
    DWRITE(inbuf, rc);
}

void epoll_remove(int fd)
{
    epoll_ctl(epoll_instance, EPOLL_CTL_DEL, fd, NULL);
}

/**
 * @brief
 * Primeste o noua conexiune, o adauga la epoll si
 * marcheaza socket-ul cu TCP_NODELAY
 */
void accept_incoming_connection()
{
    int fd = accept(tcp_socket, NULL, NULL);
    add_to_epollin(epoll_instance, fd);
    int rc = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    CHECK(rc < 0, "setsockopt", FATAL);
}

/**
 * @brief
 * Parcurge lista de clienti si cauta un client dupa id
 *
 * @param curr_client
 * Clientul cautat
 * @return
 * - -2 daca acel client este deja conectat
 * - indexul in lista daca clientul a mai fost conectat
 *  inainte si s-a reconectat intre timp
 * - -1 daca nu a mai fost gasit in lista clientul
 */
int client_is_connected(struct tcp_client* curr_client)
{
    for (int i = 0; i < client_count;i++) {
        if (strcmp(curr_client->id, clients[i]->id) == 0 && curr_client->sockfd != clients[i]->sockfd) {
            if (clients[i]->is_connected) {
                return -2;
            }
            else {
                return i;
            }
        }
    }
    return -1;
}

/**
 * @brief
 * Transmite clientului care incearca sa se
 * conecteze ca acest lucru nu este permis.
 *
 * @param fd
 * Socketul pe care se transmite mesajul prin
 * care se refuza conexiunea
 */
void refuse_connection(int fd)
{
    struct message_header reject;
    memset(&reject, 0, sizeof(struct message_header));
    reject.flags = CONN_NO;
    reject.msg_len = sizeof(struct message_header);
    int rc = send(fd, &reject, sizeof(struct message_header), 0);
    CHECK(rc < 0, "send", FATAL);
    close(fd);
    epoll_remove(fd);
}

/**
 * @brief
 * Trimite un mesaj salvat in vectorul de mesaje
 * clientului curr_client.
 *
 * @param curr_client
 * Clientul curent
 * @param msg
 * Mesajul de trimis
 */
void send_queued_message(struct tcp_client* curr_client, const struct msg_to_send& msg)
{
    struct message_header mh;
    memset(&mh, 0, sizeof(mh));
    mh.flags = MSG_NEW;
    mh.msg_len = msg.content.act_len + sizeof(struct message_header) + sizeof(struct udp_header) + sizeof(struct udp_message);
    send(curr_client->sockfd, &mh, sizeof(mh), 0);
    send(curr_client->sockfd, &msg.hdr, sizeof(msg.hdr), 0);
    send(curr_client->sockfd, &msg.umsg, sizeof(msg.umsg), 0);
    send(curr_client->sockfd, &msg.content.data, msg.content.act_len, 0);
}

/**
 * @brief
 * Se parcurg toate topicurile si se cauta clientul
 * curent in lista lor de subscriberi. Daca acesta
 * este gasit, se verifica daca are sf sau nu.
 *
 * @param curr_client
 * Clientul curent
 */
void send_missing_messages(struct tcp_client* curr_client)
{
    for (auto& topic : topics) {
        auto sub = std::find_if(topic.second.subs.begin(), topic.second.subs.end(),
            [&curr_client](const struct subscriber& sub) {
                return strcmp(curr_client->id, sub.client_s->id) == 0;
            });
        // daca clientul nu este abonat la topic se trece la urmatorul
        if (sub == topic.second.subs.end()) {
            continue;
        }
        // daca are sf pe 1 si mai are de primit mesaje, se trimit mesajele respective
        if (sub->sf == SF_YES) {
            if (topic.second.messages.size() - sub->last_msg_received > 1) {
                for (uint32_t i = sub->last_msg_received + 1; i < topic.second.messages.size(); i++) {
                    send_queued_message(curr_client, topic.second.messages[i]);
                }
            }
        }
    }
}

/**
 * @brief
 * Functie folosita la debug
 */
void print_clients()
{
    for (int i = 0;i < client_count;i++) {
        fprintf(stderr, "CLIENT: %s, is_conn: %d, fd: %d\n", clients[i]->id,
            clients[i]->is_connected, clients[i]->sockfd);
    }
}

/**
 * @brief
 * Gestioneaza un mesaj prin care clientul vrea sa
 * se conecteze(logheze).
 *
 * @param curr_client
 * Clientul curent care este tratat.
 */
void handle_connection_request(struct tcp_client* curr_client)
{
    int payload_len = curr_client->last_msg_len - sizeof(struct message_header);
    memcpy(curr_client->id, curr_client->inbuf + sizeof(struct message_header), payload_len);
    int rc = client_is_connected(curr_client);
    if (rc == -2) {
        printf("Client %s already connected.\n", curr_client->id);
        return;
    }
    else if (rc == -1) {
        curr_client->is_connected = 1;
        char* ip_addr = inet_ntoa(curr_client->addr.sin_addr);
        printf("New client %s connected from %s:%d.\n", curr_client->id, ip_addr, curr_client->addr.sin_port);
        return;
    }
    clients[rc]->sockfd = curr_client->sockfd;
    clients[rc]->is_connected = 1;
    send_missing_messages(clients[rc]);
}

/**
 * @brief
 * Deconecteaza clientul curent si afiseaza
 * un mesaj corespunzator in terminal
 *
 * @param curr_client
 * Clientul care urmeaza a fi deconectat
 */
void handle_connection_close(struct tcp_client* curr_client)
{
    curr_client->is_connected = 0;
    epoll_remove(curr_client->sockfd);
    close(curr_client->sockfd);
    curr_client->sockfd = -1;
    printf("Client %s disconnected.\n", curr_client->id);
}

/**
 * @brief
 * Gestioneaza abonarea unui client la un topic
 * atat cu sf 1 cat si cu sf 0
 *
 * @param curr_client
 * Clientul curent
 * @param sf
 * 1 pentru sf si 0 pentru fara sf
 */
void handle_new_subscription(struct tcp_client* curr_client, int sf)
{
    int sf_inc = 0;
    struct subscriber new_sub { curr_client, 0, 0 };
    if (sf == SF_YES) {
        sf_inc = 1;
        new_sub.sf = 1;
    }
    int topic_len = curr_client->last_msg_len - sizeof(struct message_header);
    char topic_str[51] = { 0 };
    memcpy(topic_str, curr_client->inbuf + sizeof(struct message_header), topic_len);
    auto iter = topics.find(std::string(topic_str));
    if (iter == topics.end()) {
        struct topic new_topic { {}, { new_sub }, sf_inc };
        topics.emplace(std::string(topic_str), new_topic);
    }
    else {
        iter->second.subs.push_back(new_sub);
        iter->second.sf_count += sf_inc;
    }
}

/**
 * @brief
 * Gestioneaza dezabonarea unui client la un topic
 *
 * @param curr_client
 * Clientul curent
 */
void handle_unsubscription(struct tcp_client* curr_client)
{
    int topic_len = curr_client->last_msg_len - sizeof(struct message_header);
    char topic_str[51] = { 0 };
    memcpy(topic_str, curr_client->inbuf + sizeof(struct message_header), topic_len);
    auto iter = topics.find(std::string(topic_str));
    if (iter == topics.end()) {
        return;
    }
    else {
        auto topic = iter;
        auto sub = std::find_if(topic->second.subs.begin(), topic->second.subs.end(),
            [&curr_client](const struct subscriber& sub) {
                return curr_client == sub.client_s;
            });
        if (sub != topic->second.subs.end()) {
            topic->second.subs.erase(sub);
            if (sub->sf == 1) {
                topic->second.sf_count--;
            }
            if (topic->second.sf_count == 0) {
                topic->second.messages.clear();
            }
        }
    }
}

/**
 * @brief
 * Trateaza un mesaj complet primit de la un client.
 *
 * @param curr_client
 * Clientul al carui mesaj este tratat.
 */
void handle_complete_message(struct tcp_client* curr_client)
{
    /*
     * In functie de actiunea setata in mesaj
     * se decide cum sa se gestioneze mesajul.
     */
    switch (curr_client->action) {
    case CONN_REQ:
        handle_connection_request(curr_client);
        break;
    case SUB_NOSF:
        handle_new_subscription(curr_client, SF_NO);
        break;
    case SUB_SF:
        handle_new_subscription(curr_client, SF_YES);
        break;
    case UNSUB:
        handle_unsubscription(curr_client);
        break;
    case CONN_END:
        handle_connection_close(curr_client);
        break;
    }
}

/**
 * @brief
 * Citeste un numar de bytes de la un anumit client.
 * Se citeste mai intai header-ul mesajului in intregime,
 * apoi restul continutului daca exista.
 *
 * @param curr_client
 * Clientul curent
 */
void read_from_client(struct tcp_client* curr_client)
{
    uint32_t bytes_read = recv(curr_client->sockfd, curr_client->inbuf, sizeof(struct message_header), MSG_WAITALL);
    if (bytes_read == 0 || bytes_read != sizeof(struct message_header)) {
        close(curr_client->sockfd);
        epoll_remove(curr_client->sockfd);
        return;
    }
    struct message_header* mess = (struct message_header*)curr_client->inbuf;
    curr_client->last_msg_len = mess->msg_len;
    curr_client->action = mess->flags;
    DWRITE(curr_client->inbuf, sizeof(struct message_header));
    if (curr_client->last_msg_len == sizeof(struct message_header)) {
        handle_complete_message(curr_client);
        return;
    }
    bytes_read = recv(curr_client->sockfd, curr_client->inbuf + sizeof(struct message_header), curr_client->last_msg_len - sizeof(struct message_header), MSG_WAITALL);
    perror("recv");
    if (bytes_read == 0 || bytes_read != (curr_client->last_msg_len - sizeof(struct message_header))) {
        exit(101);
    }
    handle_complete_message(curr_client);
    memset(curr_client->inbuf, 0, 256);
}

/**
 * @brief
 * Elibereaza structurile aferente clientilor si inchide socketii
 * lor. De asemenea se trimite un mesaj la fiecare client conectat
 * pentru ca acestia sa se inchida.
 */
void cleanup()
{
    struct message_header close_msg;
    memset(&close_msg, 0, sizeof(close_msg));
    close_msg.flags = CONN_END;
    close_msg.msg_len = sizeof(struct message_header);
    for (int i = 0;i < client_count;i++) {
        if (clients[i]->is_connected) {
            send(clients[i]->sockfd, &close_msg, sizeof(close_msg), 0);
            close(clients[i]->sockfd);
            free(clients[i]);
        }
    }
}

/**
 * @brief
 * Citeste prima parte dintr-un mesaj, care are mereu
 * dimensiune fixa de 20 de bytes.
 * @param fd
 * Socketul de pe care se citesc date
 */
struct message_header* read_connection_header(int fd)
{
    struct message_header* msghdr = (struct message_header*)calloc(1, sizeof(struct message_header));
    int bytes_read = recv(fd, msghdr, sizeof(struct message_header), MSG_WAITALL);
    if (bytes_read == 0) {
        close(fd);
        epoll_remove(fd);
        return NULL;
    }
    return msghdr;
}

void handle_new_connection(int fd)
{
    struct message_header* msg = read_connection_header(fd);
    // daca mesajul nu a fost primit cum trebuie
    // sau daca nu este un connection request nu il tratez
    if (msg == NULL || msg->flags != CONN_REQ) {
        return;
    }
    int id_found = 0;
    // cautam in lista de clienti un client cu acelasi id
    // precum cel care doreste sa se conecteze
    for (int i = 0; i < client_count;i++) {
        if (strcmp(clients[i]->id, msg->id) == 0) {
            id_found = 1;
            // daca are acelasi id si e deja conectat
            // nu permitem conexiunea
            if (clients[i]->is_connected) {
                epoll_remove(fd);
                printf("Client %s already connected.\n", msg->id);
                refuse_connection(fd);
                close(fd);
                break;
            }
            // altfel actualizam fd-ul asociat si ii dam voie sa se conecteze
            else {
                clients[i]->addr_len = sizeof(struct sockaddr);
                getpeername(fd, (struct sockaddr*)&clients[i]->addr, &clients[i]->addr_len);
                char* ip_addr = inet_ntoa(clients[i]->addr.sin_addr);
                printf("New client %s connected from %s:%d.\n", clients[i]->id, ip_addr, ntohs(clients[i]->addr.sin_port));
                clients[i]->is_connected = 1;
                clients[i]->sockfd = fd;
                send_missing_messages(clients[i]);
                break;
            }
        }
    }
    // daca nu a fost gasit inseamna ca trebuie creat un client nou
    if (!id_found) {
        clients.push_back(NULL);
        clients[client_count] = new_client();
        clients[client_count]->is_connected = 1;
        clients[client_count]->bytes_received = 0;
        getpeername(fd, (struct sockaddr*)&clients[client_count]->addr, &clients[client_count]->addr_len);
        clients[client_count]->sockfd = fd;
        memcpy(clients[client_count]->id, msg->id, strlen(msg->id));
        char* ip_addr = inet_ntoa(clients[client_count]->addr.sin_addr);
        printf("New client %s connected from %s:%d.\n", clients[client_count]->id, ip_addr, ntohs(clients[client_count]->addr.sin_port));

        client_count++;
    }
    free(msg);
}

/**
 * @brief
 * Bucla principala a programului in care astept
 * evenimentele returnate de epoll si la tratez
 * corespunzator dupa tipul lor.
 */
void main_loop()
{
    char input[256];
    while (1) {
        int nr_events = epoll_wait(epoll_instance, events, sizeof(events) / sizeof(struct epoll_event), -1);
        CHECK(nr_events == -1, "epoll_wait", FATAL);
        for (int i = 0; i < nr_events; i++) {
            // cazul pentru input primit de la tastatura
            if (events[i].data.fd == STDIN_FILENO) {
                int rc = handle_input(input, sizeof(input));
                if (rc == EXITRC) {
                    return;
                }
                else {
                    fprintf(stderr, "invalid command received: %s", input);
                }
            }
            // cazul pentru o noua conexiune
            else if (events[i].data.fd == tcp_socket) {
                accept_incoming_connection();
            }
            // cazul pentru mesaj primit de la client udp
            else if (events[i].data.fd == udp_socket) {
                handle_incoming_message();
            }
            // cazul pentru un mesaj primit de la client tcp
            else {
                struct tcp_client* curr_client = find_client_by_fd(events[i].data.fd);
                if (curr_client == NULL) {
                    handle_new_connection(events[i].data.fd);
                    continue;
                }
                read_from_client(curr_client);
            }
        }
    }
}

/**
 * @brief
 * Se creeaza socket-ul udp si se leaga la
 * portul specificat in argumente
 *
 * @param port
 * Portul la care se leaga socketul
 */
void create_udp_socket(char* port)
{
    udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(udp_socket < 0, "socket", FATAL);
    int yes = 1;
    int rc = setsockopt(udp_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    CHECK(rc < 0, "setsockopt", FATAL);
    bind_to_addr(udp_socket, BROADCAST, port);
}

/**
 * @brief 
 * Se creeaza socket-ul tcp si se leaga la
 * portul specificat in argumente
 *
 * @param port
 * Portul la care se leaga socketul
 */
void create_tcp_socket(char* port)
{
    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(tcp_socket < 0, "socket", FATAL);
    int rc = setsockopt(tcp_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    CHECK(rc < 0, "setsockopt", FATAL);
    rc = setsockopt(tcp_socket, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    CHECK(rc < 0, "setsockopt", FATAL);
    bind_to_addr(tcp_socket, BROADCAST, port);
    listen(tcp_socket, 10000);
}

/**
 * @brief
 * Leaga un socket de adresa IP si
 * portul specificate in parametri
 *
 * @param sock
 * Socketul pe care se va realiza bind
 * @param addr
 * Adresa IP in format numeric, e.g. "1.2.3.4"
 * @param port
 * Portul in format numeric, e.g. "7654"
 */
void bind_to_addr(int sock, char* addr, char* port)
{
    struct addrinfo hints, * results;
    memset(&hints, 0, sizeof(hints));

    /*
     * Am folosit AI_NUMERICHOST si AI_NUMERICSERV pentru
     * ca stiam dinainte ca adresa si portul vor veni in
     * format numeric.
     */
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV | AI_PASSIVE;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(addr, port, &hints, &results);
    CHECK(rc < 0, "getaddrinfo", FATAL);
    rc = bind(sock, results->ai_addr, results->ai_addrlen);
    CHECK(rc < 0, "bind", FATAL);
    freeaddrinfo(results);
}

/**
 * @brief
 * Gestioneaza primirea de input de la tastatura
 *
 * @param buf
 * buffer-ul in care se citeste comanda
 * @param maxlen
 * lungimea maxima a buffer-ului
 * @return
 * - EXITRC daca comanda primita e exit
 * - UNKNOWN_COMMAND in caz contrar
 */
int handle_input(char* buf, int maxlen)
{
    int cc = read(STDIN_FILENO, buf, maxlen - 1);
    CHECK(cc < 0, "read", FATAL);
    buf[min(cc, maxlen - 1)] = 0;
    if (strcmp(buf, "exit\n") == 0) {
        return EXITRC;
    }
    return UNKNOWN_COMMAND;
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        fprintf(stderr, "not enough arguments, must specify port number\n");
        exit(1);
    }

    epoll_instance = epoll_create1(0);
    CHECK(epoll_instance < 0, "epoll", FATAL);

    add_to_epollin(epoll_instance, STDIN_FILENO);

    create_udp_socket(argv[1]);
    add_to_epollin(epoll_instance, udp_socket);

    create_tcp_socket(argv[1]);
    add_to_epollin(epoll_instance, tcp_socket);

    setvbuf(stdout, NULL, _IONBF, BUFSIZ);
    setvbuf(stdin, NULL, _IONBF, BUFSIZ);

    main_loop();
    cleanup();

    return 0;
}