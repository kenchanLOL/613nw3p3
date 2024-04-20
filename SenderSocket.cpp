#include "pch.h"
#include "SenderSocket.h"

UINT WorkerRun(LPVOID param) {
	SenderSocket* ss = (SenderSocket*)param;
	clock_t end;
	DWORD nextSend = ss->sendBaseNum;
	int kernelBuffer = 20e6;
	if (setsockopt(ss->sock, SOL_SOCKET, SO_RCVBUF, (char*)&kernelBuffer, sizeof(int)) == SOCKET_ERROR) {
		end = clock();
		printf("[ %.3f]  Rreceiver kernel buffer size fail to set as 20 MB with error code: %d\n", ((float)(end - ss->start) / CLOCKS_PER_SEC), WSAGetLastError());
	}
	if (setsockopt(ss->sock, SOL_SOCKET, SO_SNDBUF, (char*)&kernelBuffer, sizeof(int)) == SOCKET_ERROR) {
		end = clock();
		printf("[ %.3f]  Sender kernel buffer size fail to set as 20 MB with error code: %d\n", ((float)(end - ss->start) / CLOCKS_PER_SEC), WSAGetLastError());
	}
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	// trigger socketReady event when it has data to read
	WSAEventSelect(ss->sock, ss->socketReady, FD_READ);

	HANDLE events[] = { ss->socketReady, ss->qFull, ss->eQuit };
	clock_t timeout;
	while (true) {
		float current = ((float)clock() - ss->start) / CLOCKS_PER_SEC;
		if (ss->sendBaseNum != ss->nextSeqNum)
			ss->timeExpire = 1000 * (ss->e_rto) + current;
		else
			ss->timeExpire = INFINITE;
		//printf("time: %d, sendBaseNum : %d, nextSeqNum : %d, timeout : %d\n",(int)1000*current, ss->sendBaseNum, ss->nextSeqNum, ss->timeExpire);
		int ret = WaitForMultipleObjects(3, events, false, ss->timeExpire);
		switch (ret)
		{
		case (WAIT_TIMEOUT): {
			int slot = (ss->sendBaseNum) % ss->windSize;
			float current2 = ((float)clock() - ss->start) / CLOCKS_PER_SEC;
			//printf("Timeout : slot: %d\n", slot);
			if (ss->dups == MAX_RETX) {
				SetEvent(ss->eQuit);
				return TIMEOUT;
			}
			// retransmission
			current = ((float)clock() - ss->start) / CLOCKS_PER_SEC;
			ss->pending_pkts[slot].txTime = current;
			int status;
			if (status = sendto(ss->sock, ss->pending_pkts[slot].pkt, ss->pending_pkts[slot].size + sizeof(SenderDataHeader), 0, (struct sockaddr*)&ss->remote, sizeof(struct sockaddr_in)) == SOCKET_ERROR) {
				end = clock();
				printf("[ %.3f]  Sendto failed with error code: %d\n", ((float)(end - ss->start) / CLOCKS_PER_SEC), WSAGetLastError());
				SetEvent(ss->eQuit);
				return FAILED_SEND;
			}
			ss->retx = true;
			//ss->timeExpire = current + ss->e_rto;
			ss->dups++;
			ss->ss_stat->timeout++;
			break;
		}
		case (WAIT_OBJECT_0): { // recv pkt
			ReceiverHeader rh;
			struct sockaddr_in response;
			int addr_size = sizeof(response);
			int slot;
			clock_t recv = clock();
			int byte = recvfrom(ss->sock, (char*)&rh, sizeof(rh), 0, (struct sockaddr*)&response, &addr_size);
			if (byte == SOCKET_ERROR) {
				end = clock();
				// printf("[ %.3f]  --> failed recvfrom with %d\n", ((float)(end - ss->start) / CLOCKS_PER_SEC), WSAGetLastError());
				return FAILED_RECV;
			}
			//printf("Recv : %d, sendBase : %d\n", rh.recvWnd, ss->sendBaseNum);
			if (rh.ackSeq > ss->sendBaseNum) {
				slot = (rh.ackSeq - 1) % ss->windSize;
				if (!ss->retx) { // only consider non-retx package
					//printf("Recv Normal : slot: %d\n", slot);
					current = ((float)clock() - ss->start) / CLOCKS_PER_SEC;
					clock_t pkt_start = ss->pending_pkts[slot].txTime;
					float s_rtt = current - pkt_start;
					//printf("Real Time Diff : %.2f\n", s_rtt);
					s_rtt = 0.1;
					ss->e_rtt = (1 - 0.125) * ss->e_rtt + 0.125 * s_rtt;
					ss->dev_rtt = (1 - 0.25) * ss->dev_rtt + 0.25 * abs(ss->e_rtt - s_rtt);
					ss->e_rto = ss->e_rtt + 4 * max(ss->dev_rtt, 0.001);
				}
				else {
					//printf("Recv retx : slot: %d\n", slot);
				}
				ss->dups = 0;
				ss->retx = false;
				ss->timeExpire = 1000 * (ss->e_rto) + current; // reset timer
				ss->sendBaseNum = rh.ackSeq;
				ss->ss_stat->ackSeq = ss->nextSeqNum;
				ss->ss_stat->ackBytes += MAX_PKT_SIZE;
				ss->ss_stat->eff_winsize = ss->windSize;
				ss->ss_stat->e_rtt = ss->e_rtt;
				int stepSize = ss->sendBaseNum + min(ss->windSize, rh.recvWnd) - ss->lastRelease;
				ss->qEmpty_val += stepSize;
				//printf("produce %d window", stepSize);
				//printf("sb: %d windSize: %d recvWin: %d lastRelease: %d\n", ss->sendBaseNum, ss->windSize, rh.recvWnd, ss->lastRelease);
				//printf("stepSize: %d \n", stepSize);
				ss->lastRelease += stepSize;
				//printf("Recv : produce %d from qEmpty, current qEmpty Val: %d, sendBase: %d\n", stepSize, ss->qEmpty_val, ss->sendBaseNum);
				ReleaseSemaphore(ss->qEmpty, stepSize, NULL);
			}
			else if (rh.ackSeq == ss->sendBaseNum) {
				slot = ss->sendBaseNum % ss->windSize;
				//printf("Packet Loss : slot: %d, timer : %d\n", slot, ss->timeExpire);
				//printf("Packet Loss : slot: %d\n", slot);
				ss->dups++;
				if (ss->dups == 3 ) {
					//printf("Fast Retx : slot: %d\n", slot);
					ss->retx = true;
					ss->ss_stat->fast_retransmit++;
					// retransmission
					current = ((float)clock() - ss->start) / CLOCKS_PER_SEC;
					ss->pending_pkts[slot].txTime = current;
					int status;
					ss->timeExpire = 1000 * (ss->e_rto) + current;
					if (status = sendto(ss->sock, ss->pending_pkts[slot].pkt, ss->pending_pkts[slot].size + sizeof(SenderDataHeader), 0, (struct sockaddr*)&ss->remote, sizeof(struct sockaddr_in)) == SOCKET_ERROR) {
						end = clock();
						printf("[ %.3f]  Sendto failed with error code: %d\n", ((float)(end - ss->start) / CLOCKS_PER_SEC), WSAGetLastError());
						SetEvent(ss->eQuit);
						return FAILED_SEND;
					}

				}
				//ss->recvWnd = rh.recvWnd;
			}
			break;
		}
		case (WAIT_OBJECT_0 + 1): { // send pkt
			//printf("Sending : consume 1 from qFull, current qFull Val: %d, nextSend: %d\n", ss->qFull_val - 1, nextSend);
			ss->qFull_val -= 1;
			current = ((float)clock() - ss->start) / CLOCKS_PER_SEC;
			int status;
			int slot = nextSend % ss->windSize;
			//printf("sending slot : %d\n", slot);
			ss->pending_pkts[slot].txTime = current;
			if (status = sendto(ss->sock, ss->pending_pkts[slot].pkt, ss->pending_pkts[slot].size + sizeof(SenderDataHeader), 0, (struct sockaddr*)&ss->remote, sizeof(struct sockaddr_in)) == SOCKET_ERROR) {
				end = clock();
				printf("[ %.3f]  Sendto failed with error code: %d\n", ((float)(end - ss->start) / CLOCKS_PER_SEC), WSAGetLastError());
				SetEvent(ss->eQuit);
				return FAILED_SEND;
			}
			if (ss->sendBaseNum == nextSend) {
				ss->timeExpire = ss->e_rto + current;
			}
			nextSend++;
			break;
		}
		case (WAIT_OBJECT_0 + 2):
			return 0;
		default:
		{
			end = clock();
			printf("[ %.3f]  Waiting for Multiple objects failed with error code: %d\n", ((float)(end - ss->start) / CLOCKS_PER_SEC), GetLastError());
			break;
		}
		};

	}

}


SenderSocket::SenderSocket(Stat* stat) {
	nOpenAttempt = 3;
	nCloseAttempt = 5;
	ss_stat = stat;
	WSADATA wsaData;
	int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock == INVALID_SOCKET)
	{
		int err_code = WSAGetLastError();
		printf("Failed with socket() generated error %d\n", err_code);
		WSACleanup();
	}
	eQuit = CreateEvent(NULL, true, false, NULL);
	socketReady = CreateEvent(NULL, false, false, NULL);
	 
}

SenderSocket::~SenderSocket()
{
	free(pending_pkts);
	//free(istransmitted);
	closesocket(sock);
	WSACleanup();
}
int SenderSocket::Open(char* targetHost, int magic_port, int senderWindow, LinkProperties* lp) {
	if (isConnected) {
		return ALREADY_CONNECTED;
	}
	e_rto = max(1, 2*lp->RTT);
	e_rtt = lp->RTT;
	dev_rtt = 0;
	tHost = targetHost;
	tPort = magic_port;
	lProp = lp;
	start = clock();
	DWORD IP = inet_addr(targetHost);
	struct hostent* target;
	struct sockaddr_in server;
	if (IP == INADDR_NONE)
	{
		// if not a valid IP, then do a DNS lookup
		if ((target = gethostbyname(targetHost)) == NULL)
		{
			int err = WSAGetLastError();
			printf("[%.3f] --> target %s is invalid\n", ((double) (clock()-start))/CLOCKS_PER_SEC, targetHost);
			WSACleanup();
			return INVALID_NAME;
		}
		else {
			memcpy((char*)&(server.sin_addr), target->h_addr, target->h_length);
		}
	}
	else {
		server.sin_addr.S_un.S_addr = IP;
	}

	server.sin_family = AF_INET;
	server.sin_port = htons(magic_port);
	SenderSynHeader ssh;
	ssh.sdh.seq = 0;
	ssh.sdh.flags.SYN = 1;
	memcpy(&ssh.lp, lp, sizeof(LinkProperties));
	windSize = senderWindow;
	pending_pkts = new Packet[windSize];
	dups = 0;
	retx = false;
	memset(pending_pkts, 0, windSize * sizeof(Packet));
	//WaitForSingleObject(workerQ, INFINITE);

	for (int i = 0; i < nOpenAttempt; i++) {
		char* ip = inet_ntoa(server.sin_addr);
		double time = ((double)(clock() - start)) / CLOCKS_PER_SEC;
		//printf("[%.3f] --> SYN %d (attempt %d of %d, RTO %.3f) to %s\n", time, ssh.sdh.seq, i+1, nOpenAttempt, 1.0, ip);
		clock_t rtt_start = clock();
		int status;
		if (status = sendto(sock, (char*) &ssh, sizeof(ssh), 0, (struct sockaddr*)&server, sizeof(struct sockaddr_in)) != SOCKET_ERROR) {
			timeval timeout;
			timeout.tv_sec = (long)e_rto;
			timeout.tv_usec = (e_rto - (long)e_rto) * 1000000;
			fd_set fd;
			FD_ZERO(&fd); // clear the set
			FD_SET(sock, &fd); // add your socket to the set
			int ret = select(0, &fd, NULL, NULL, &timeout);
			if (ret > 0) {
				ReceiverHeader rh;
				struct sockaddr_in response;
				int addr_size = sizeof(response);
				int byte = recvfrom(sock, (char*)&rh, sizeof(rh), 0, (struct sockaddr*)&response, &addr_size);
				if (byte == SOCKET_ERROR) {
					//printf("[ %.3f]  --> failed recvfrom() with %d\n", ((float)(clock() - rtt_start) / CLOCKS_PER_SEC), WSAGetLastError());
					return FAILED_RECV;
				}
				else if (rh.flags.SYN == 1 && rh.flags.ACK == 1){
					float s_rtt = (float)(clock() - rtt_start) / CLOCKS_PER_SEC;
					e_rtt = (1 - 0.125) * e_rtt + 0.125 * s_rtt;
					dev_rtt = (1 - 0.25) * dev_rtt + 0.25 * abs(e_rtt - s_rtt);
					e_rto = e_rtt + 4 * max(dev_rtt, 0.01);
					ss_stat->e_rtt = e_rtt;
					isConnected = true;
				}
				else {
					continue;
				}
				//printf("[%.3f] <-- SYN-ACK %d window %d; setting initial RTO to %.3f\n", ((float)(clock() - start) / CLOCKS_PER_SEC), rh.ackSeq, rh.recvWnd, e_rto);
				remote = server;
				recvWnd = rh.recvWnd;
				sendBaseNum = rh.ackSeq;
				//ss_stat->ackSeq = sendBaseNum;
				ss_stat->start = start;
				qFull = CreateSemaphore(NULL, 0, windSize, NULL);
				qEmpty = CreateSemaphore(NULL, 0, windSize, NULL);
				lastRelease = min(windSize, recvWnd);

				qFull_val = 0;
				qEmpty_val = lastRelease;
				ReleaseSemaphore(qEmpty, lastRelease, NULL);
				worker_thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)WorkerRun, this, 0, NULL);

				//move here --> HANDLE stat_handle = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)stat_thread, &stat_params, 0, NULL);

				return STATUS_OK;
			}

		}
		if (status == SOCKET_ERROR) {
			printf("[ %.3f]  --> failed sendto() with %d\n", ((float)(clock() - start) / CLOCKS_PER_SEC), WSAGetLastError());
			return FAILED_SEND;
		}
	}
	return TIMEOUT;
}

int SenderSocket::Close() {

	DWORD IP = inet_addr(tHost);
	struct hostent* target;
	struct sockaddr_in server;
	if (IP == INADDR_NONE)
	{
		// if not a valid IP, then do a DNS lookup
		if ((target = gethostbyname(tHost)) == NULL)
		{
			int err = WSAGetLastError();
			printf("[%.3f] --> target %s is invalid\n", ((double)(clock() - start)) / CLOCKS_PER_SEC, tHost);
			WSACleanup();
			return INVALID_NAME;
		}
		else {
			memcpy((char*)&(server.sin_addr), target->h_addr, target->h_length);
		}
	}
	else {
		server.sin_addr.S_un.S_addr = IP;
	}
	SetEvent(eQuit);
	WaitForSingleObject(worker_thread, INFINITE);
	CloseHandle(worker_thread);
	server.sin_family = AF_INET;
	server.sin_port = htons(tPort);
	SenderSynHeader ssh;
	ssh.sdh.flags.FIN = 1;
	//ssh.sdh.seq = ackSeq;
	ssh.sdh.seq = nextSeqNum;
	memcpy(&ssh.lp, lProp, sizeof(LinkProperties));
	for (int i = 0; i < nCloseAttempt; i++) {
		double time = ((double)(clock() - start)) / CLOCKS_PER_SEC;
		//printf("[%.3f] --> FIN %d (attempt %d of %d, RTO %.3f)\n", time, ssh.sdh.seq, i+1, nOpenAttempt, e_rto);
		clock_t rtt_start = clock();
		int status;
		if (status = sendto(sock, (char*)&ssh, sizeof(ssh), 0, (struct sockaddr*)&server, sizeof(struct sockaddr_in)) != SOCKET_ERROR) {
			timeval timeout;
			timeout.tv_sec = (long)e_rto;
			timeout.tv_usec = (e_rto - (long)e_rto) * 1000000;
			fd_set fd;
			FD_ZERO(&fd); // clear the set
			FD_SET(sock, &fd); // add your socket to the set
			int ret = select(0, &fd, NULL, NULL, &timeout);
			if (ret > 0) {
				ReceiverHeader rh;
				struct sockaddr_in response;
				int addr_size = sizeof(response);
				int byte = recvfrom(sock, (char*)&rh, sizeof(rh), 0, (struct sockaddr*)&response, &addr_size);
				if (byte == SOCKET_ERROR) {
					//printf("[ %.3f]  --> failed recvfrom with %d\n", ((float)(clock() - rtt_start) / CLOCKS_PER_SEC), WSAGetLastError());
					return FAILED_RECV;
				}
				else if (rh.flags.FIN == 1 && rh.flags.ACK == 1) {
					float s_rtt = (float)(clock() - rtt_start) / CLOCKS_PER_SEC;
					e_rtt = (1 - 0.125) * e_rtt + 0.125 * s_rtt;
					dev_rtt = (1 - 0.25) * dev_rtt + 0.25 * abs(e_rtt - s_rtt);
					e_rto = e_rtt + 4 * max(dev_rtt, 0.01);
				}
				else {
					continue;
				}
				//printf("[%.3f] <-- FIN-ACK %d window %d\n", ((float)(clock() - start) / CLOCKS_PER_SEC), rh.ackSeq, rh.recvWnd);
				recvWnd = rh.recvWnd;
				isConnected = false;
				//ss_stat->ackSeq = ackSeq;
				ss_stat->ackSeq = nextSeqNum;
				printf("[ %.2f ] <-- FIN-ACK %d window %X \n", (double)(clock() - start) / CLOCKS_PER_SEC, rh.ackSeq, rh.recvWnd);
				return STATUS_OK;
			}

		}
		if (status == SOCKET_ERROR) {
			//printf("[ %.3f]  --> failed sendto with %d\n", ((float)(clock() - start) / CLOCKS_PER_SEC), WSAGetLastError());
			return FAILED_SEND;
		}
	}
}

int SenderSocket::Send(char* buf, int bytes) {
	HANDLE arr[] = {qEmpty, eQuit };
	int ret = WaitForMultipleObjects(2, arr, false, INFINITE);
	switch (ret)
	{
	case WAIT_OBJECT_0:
	{
		//printf("consume 1 from qEmpty, current qEmpty Val: %d\n", qEmpty_val - 1);
		qEmpty_val -= 1;
		DWORD slot = nextSeqNum % windSize;
		Packet* p = pending_pkts + slot;
		SenderDataHeader* sdh = (SenderDataHeader*)(&(p->pkt[0]));
		sdh->seq = nextSeqNum;
		sdh->flags.magic = MAGIC_PROTOCOL;
		memcpy(sdh + 1, buf, bytes);
		p->txTime = 0;
		p->size = bytes;
		nextSeqNum++;
		qFull_val += 1;
		//printf("writing on slot : %d\n", slot);
		//printf("produce 1 to qFull, current qFull Val: %d,  CurSeqNum of pkt: %d\n", qFull_val, nextSeqNum-1);
		ReleaseSemaphore(qFull, 1, NULL);
		break;
	}
	case WAIT_OBJECT_0 + 1:
	{
		
		return FAILED_SEND;
	}
	default:
	{
		printf("[ %.3f]  Wating for qEmpty failed with error code:%d\n", ((float)(clock() - start) / CLOCKS_PER_SEC), GetLastError());
		return RETRANSMIT;
		break;
	}
	}
	return STATUS_OK;
}
