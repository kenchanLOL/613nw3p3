#pragma once
#pragma pack(push,1)
#include "pch.h"
#define MAGIC_PORT 22345 // receiver listens on this port
#define MAX_PKT_SIZE (1500-28) // maximum UDP packet size accepted by receiver 
#define MAGIC_PROTOCOL 0x8311AA 
#define MAX_ATTEMPTS 10
#define MAX_RETX 50 

#define STATUS_OK 0 // no error
#define ALREADY_CONNECTED 1 // second call to ss.Open() without closing connection
#define NOT_CONNECTED 2 // call to ss.Send()/Close() without ss.Open()
#define INVALID_NAME 3 // ss.Open() with targetHost that has no DNS entry
#define FAILED_SEND 4 // sendto() failed in kernel
#define TIMEOUT 5 // timeout after all retx attempts are exhausted
#define FAILED_RECV 6 // recvfrom() failed in kernel
#define RETRANSMIT 7

#define FORWARD_PATH 0
#define RETURN_PATH 1 

class Flags {
public:
	DWORD reserved : 5; // must be zero
	DWORD SYN : 1;
	DWORD ACK : 1;
	DWORD FIN : 1;
	DWORD magic : 24;
	Flags() { memset(this, 0, sizeof(*this)); magic = MAGIC_PROTOCOL; }
};

class SenderDataHeader {
public:
	Flags flags;
	DWORD seq; // must begin from 0
};

class LinkProperties {
public:
	// transfer parameters
	float RTT; // propagation RTT (in sec)
	float speed; // bottleneck bandwidth (in bits/sec)
	float pLoss[2]; // probability of loss in each direction
	DWORD bufferSize; // buffer size of emulated routers (in packets)
	LinkProperties() { memset(this, 0, sizeof(*this)); }
};

class SenderSynHeader {
	public:
		SenderDataHeader sdh;
		LinkProperties lp;
};


class ReceiverHeader {
	public:
		Flags flags;
		DWORD recvWnd; // receiver window for flow control (in pkts)
		DWORD ackSeq; // ack value = next expected sequence
};

//class DataPkt {
//	public:
//		SenderDataHeader sdh;
//		char data[MAX_PKT_SIZE];
//};

class Packet {
	public:
		int type;
		int size;
		double txTime;
		char pkt[MAX_PKT_SIZE];
};

class Stat {
	public:
		clock_t start;
		int timeout = 0;
		int fast_retransmit = 0;
		int ackSeq = 0;
		int eff_winsize = 1;
		double ackBytes = 0.0;
		double speed = 0.0;
		double e_rtt = 0.0;
		double avg_rate = 0.0;
		bool stop = false;
};

class SenderSocket {
	public:
		SOCKET sock;
		clock_t start;
		LinkProperties* lProp;
		
		int nOpenAttempt;
		int nCloseAttempt;

		double e_rto; // all three are in unit of sec
		double e_rtt;
		float dev_rtt;

		char* tHost;
		int tPort;
		struct sockaddr_in remote;
		bool isConnected = false;

		// DWORD ackSeq;
		Stat* ss_stat;
		
		Packet* pending_pkts;
		int dups;
		bool retx;

		HANDLE qFull;
		HANDLE qEmpty;
		HANDLE eQuit;
		HANDLE socketReady;
		HANDLE worker_thread;

		DWORD recvWnd;
		DWORD windSize; //determine by the min (senderWind and recvWind)
		DWORD sendBaseNum;
		DWORD nextSeqNum; // 
		DWORD lastRelease; // base + windSize
		DWORD retx_count;
		clock_t timeExpire;
		int qEmpty_val;
		int qFull_val;

		SenderSocket(Stat* stat);
		~SenderSocket();
		int Open(char* targetHost, int magic_port, int senderWindow, LinkProperties* lp);
		int Send(char* buf, int bytes);
		int Close();
};