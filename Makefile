all: server subscriber

server: server.cpp common.o
	g++ -g -Wall -Wno-unused-value -o server server.cpp common.o

subscriber: subscriber.cpp common.o
	g++ -g -Wall -Wno-unused-value -o subscriber subscriber.cpp common.o

common.o: common.cpp
	g++ -g -c -Wall -Wno-unused-value -o common.o common.cpp 

clean:
	rm subscriber server common.o