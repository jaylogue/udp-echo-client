OPT ?= -O3
DEBUG ?= -g
CFLAGS = $(OPT) $(DEBUG) -Wall

ifeq ($(OS),Windows_NT)
all : UDPEchoClient.exe
else
all : UDPEchoClient
endif

UDPEchoClient : UDPEchoClient.cpp
	c++ $(CFLAGS) -o UDPEchoClient UDPEchoClient.cpp -pthread

UDPEchoClient.exe : UDPEchoClient.cpp
	x86_64-w64-mingw32-c++ $(CFLAGS) -o UDPEchoClient.exe UDPEchoClient.cpp -lws2_32

clean:
	rm -f UDPEchoClient UDPEchoClient.exe
