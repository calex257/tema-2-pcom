Calciu Alexandru 321CCa

Toate cerintele au fost implementate.

Protocolul folosit (sper ca l-am reprezentat corect)

    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                                                               |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                           id_client                           |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                                                               |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                                |            flags             |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                         message_length                        |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

Structura importanta pentru protocolul folosit este:

struct message_header {
    char id[14];
    uint16_t flags;
    uint32_t msg_len;
};

Desi id-ul are maxim 10 caractere am ales sa folosesc 14 pentru
a oferi un mic padding. Campul flags poate avea urmatoarele valori:

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

Am ales sa le fac puteri ale lui 2 cu gandul ca eventual vor
putea fi unele valori combinate cu OR pentru unele use case-uri
dar progresand in implementare am realizat ca nu este cazul.

Ideea de baza a protocolului folosit este aceea ca pentru fiecare
mesaj trimis sa stiu lungimea de la header + continut, pentru a putea
face delimitarea dintre mesaje. Am preluat aceasta idee de la protocolul
HTTP, stiind ca intr-un header HTTP exista un camp numit Content-length
si banuind ca are un scop similar pentru delimitarea mesajelor.

Am avut in minte doua cazuri:

- concatenarea a doua mesaje:
    Daca doua mesaje sunt concatenate (in bufferul din kernel de la recv
    se afla mai mult de un mesaj) citesc mai intai header-ul primului,
    apoi msg_len bytes pentru a completa mesajul si repet pentru urmatoarele
    mesaje. Nu fac asta explicit, ci ma bazez pe epoll ca va semnala fd-ul
    respectiv ca fiind ready pentru citire

- trunchierea unui mesaj:
    Daca un mesaj este prea mare sau nu poate fi citit intr-un singur apel
    de recv, citesc mai intai header-ul si ii retin lungimea, incercand apoi
    sa citesc maxim msg_len bytes pentru a completa mesajul. Atunci cand
    contorul de bytes pe care il retin este egal cu lungimea mesajului,
    mesajul poate fi gestionat ca atare.

La fiecare mesaj am trimis id-ul pentru ca m-am gandit ca ar fi util,
nu a fost neaparat cazul dar poate pentru use case-uri mai complexe ar
putea fi util.

Actiunile din flags au utilitati doar pentru cate o componenta in parte,
in general:

- Serverul trateaza doar CONN_REQ, SUB_NOSF, SUB_SF, UNSUB si CONN_END.
- Clientul trateaza doar CONN_NO, CONN_YES(desi se ia ca absenta unui CONN_NO),
CONN_END si MSG_NEW.

Un exemplu de functionare este urmatorul:

- Clientul trimite CONN_REQ imediat dupa apelul de connect.

- Serverul verifica lista de abonati. Daca mai este un client conectat cu
acelasi ID, serverul trimite CONN_END clientului nou, acesta urmand a
interpreta mesajul si a se inchide. In caz contrar, nu se transmite nimic,
semn ca operatiunile dintre client si server pot continua.

- Clientul conectat trimite un mesaj cu SUB_SF sau SUB_NOSF in functie de
tipul de subscribe si cu topicul cerut in continutul mesajului.

- Serverul primeste mesajul si adauga topicul daca e cazul, sau adauga
clientul la abonatii topicului deja existent.

- Atunci cand serverul primeste un mesaj de la un client UDP, trimite
clientului un mesaj cu MSG_NEW in header si mesajul de la clientul UDP
(structura acestui mesaj este detaliata mai jos).

- Clientul primeste mesajul, realizeaza parsarea continutului si afiseaza
datele in formatul cerut in consola.

- In cazul in care clientul doreste sa se dezaboneze, el trimite un mesaj
cu flag-ul UNSUB si topicul cerut in continutul mesajului.

- Serverul interpreteaza acest mesaj si sterge clientul din lista de abonati.

- Clientul se inchide si da mesaj cu CONN_END serverului.

- Serverul interpreteaza mesajul, inchide fd-ul aferent clientului si il
invalideaza in structura(il pune -1) si marcheaza clientul ca fiind deconectat.

- Serverul se inchide si da mesaj cu CONN_END clientilor.

- Fiecare client interpreteaza mesajul, isi inchide socket-ul, elibereaza
resursele si se inchide.


Din punctul de vedere al eficientei, consider ca protocolul este acceptabil,
utilizand un header mic si transmitand datele utile cu un overhead minim.
De asemenea, nu se trimit mesaje mai mult decat este necesar.

Pentru transmiterea mesajului udp de la clientul UDP la cel TCP am folosit
urmatoarea structura de headere:

struct message_header   --- cu formatul mentionat mai sus
struct udp_header       --- cu adresa si portul clientului UDP care a trimis
                            mesajul
struct udp_message      --- cu topicul si tipul de date al mesajului UDP
continut                --- depinde de tipul de date

Aceasta structura se reflecta si in structura msg_to_send pe care
o folosesc pentru a stoca mesajele pentru implementarea SF.

Pentru a retine topicurile am folosit un unordered_map din STL,
cu un string cu rol de cheie si un struct topic ca valoare.

In struct topic retin mesajele primite daca acest lucru este necesar,
clientii care sunt abonati la topicul respectiv si numarul de
abonati cu SF pe 1. Daca acest numar este 0 mesajele primite nu se retin,
iar cele retinute deja sunt sterse.

Pentru a retine clientii am folosit un vector STL de tcp_client*. Am ales
sa folosesc pointeri la structuri in loc de structuri in acest caz pentru
a minimiza utilizarea memoriei si copierile inutile.

Pentru a reprezenta un client in server am folosit urmatoarea structura:

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

Abonatii dintr-un topic sunt retinuti intr-o structura wrapper numita
subscriber in care retin daca abonarea are sf sau nu si indexul din
vectorul de mesaje al ultimului mesaj trimis la client. Am ales sa retin
un pointer in aceasta structura din aceleasi considerente mentionate mai
sus.

Pentru a verifica daca un client este deja conectat astept un mesaj
din partea unei conexiuni noi si verific id-ul pachetului trimis.
Daca acesta este identic cu cel al unui client conectat, transmit
clientului care incearca sa se conecteze ca acest lucru nu se poate realiza.

Pentru multiplexare I/O am folosit API-ul specific Linux epoll,
deoarece l-am folosit si la tema 3 la SO si sunt cat de cat familiar
cu el. De asemenea, in TLPI scria ca este mai rapid decat select si poll
si mi s-a parut un avantaj in cazul de fata, dorind a realiza un server
rapid. Un alt avantaj este faptul ca nu trebuie sa imi gestionez
manual vectorul de fd-uri, ci pot folosi epoll_ctl care face asta
behind the scenes.

Am facut verificari pentru input invalid in cazuri precum:
- id prea lung al clientului
- comanda necunoscuta (sir random introdus de user precum adfafas)
- numar insuficient de parametri dat unei comenzi de subscribe/unsubscribe

Parsarea mesajelor primite de la UDP am ales sa o fac in cadrul clientului
si nu in server, considerand ca treaba serverului este doar sa dea mesajele
mai departe, nu de a interpreta datele. Am incercat sa prelucrez datele fara
sa folosesc functii din math.h.

Aveam tema pe jumatate implementata cand am decis sa folosesc cpp in loc de c,
si de aceea mare parte din cod este foarte c-like.

Am eliberat memoria alocata dinamic, modificand checker-ul pentru a rula
clientul si serverul prin valgrind pentru a ma asigura ca nu am leak-uri de
memorie. In acest scenariu a picat testul de quick flow, probabil datorita
faptului ca structurile STL pe care le folosesc fac multe alocari in spate
si asta produce mult overhead prin valgrind.