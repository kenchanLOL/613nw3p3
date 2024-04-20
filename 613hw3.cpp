// 613hw3.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include "pch.h"
#include "SenderSocket.h"
#include "Checksum.h"

struct params {
	Stat* stat;
};
void stat_thread(LPVOID stat_params ) {
	params* p = (params*)stat_params;
	Stat* stat = p->stat;
	int last_base=0;
	int num_round = 0;
	last_base = stat->ackSeq;
	Sleep(2000);
	while (!stat->stop) {
		stat->speed = (stat->ackSeq - last_base) * 8 * (MAX_PKT_SIZE - sizeof(SenderDataHeader)) / 2e6;
		num_round++;
		stat->avg_rate += stat->speed;
		printf("[ %d ] B %5d ( %.1f MB) N %5d T %2d F %2d W %2d S %.3f Mbps RTT %.3f \n",
			(int)(((double)clock() - (double)stat->start) / CLOCKS_PER_SEC),
			stat->ackSeq, stat->ackBytes / 1e6, stat->ackSeq + 1, stat->timeout, stat->fast_retransmit, stat->eff_winsize, stat->speed, stat->e_rtt);
		last_base = stat->ackSeq;
		Sleep(2000);
	}
	stat->avg_rate = stat->avg_rate / num_round;
}

int main(int argc, char** argv)
{
	// parse command-line parameters
	char* targetHost = argv[1];
	int power = atoi(argv[2]);
	int senderWindow = atoi(argv[3]);
	UINT64 dwordBufSize = (UINT64)1 << power;
	DWORD* dwordBuf = new DWORD[dwordBufSize]; // user-requested buffer
	LinkProperties lp;
	lp.RTT = atof(argv[4]);
	lp.speed = 1e6 * atof(argv[7]);
	lp.pLoss[FORWARD_PATH] = atof(argv[5]);
	lp.pLoss[RETURN_PATH] = atof(argv[6]);
	lp.bufferSize = senderWindow;
	printf("Main: \t sender W = %d, RTT %.3f sec, loss %g / %g, link %d Mbps\n", senderWindow, lp.RTT, lp.pLoss[FORWARD_PATH], lp.pLoss[RETURN_PATH], atoi(argv[7]));
	clock_t start = clock();

	for (UINT64 i = 0; i < dwordBufSize; i++) // required initialization
		dwordBuf[i] = i;
	printf("Main: \t initializing DWORD array with 2^%d elements... done in %d ms\n", power, (int)(1000 *((double)clock() - (double)start) / CLOCKS_PER_SEC));
	Stat* stat_pt = new Stat();
	SenderSocket ss(stat_pt); // instance of your class
	int status = -1;
	clock_t open = clock();
	if ((status = ss.Open(targetHost, MAGIC_PORT, senderWindow, &lp)) != STATUS_OK){
		// error handling: print status and quit
		printf("Main: \t connect fail with status %d\n", status);
		return 0;
	}
	printf("Main: \t connected to %s in %f sec, pkt size %d bytes\n", targetHost, (double) (clock() - open) / CLOCKS_PER_SEC, MAX_PKT_SIZE);
	char* charBuf = (char*)dwordBuf; // this buffer goes into socket
		UINT64 byteBufferSize = dwordBufSize << 2; // convert to bytes
	UINT64 off = 0; // current position in buffer

	params stat_params;
	stat_params.stat = stat_pt;
	HANDLE stat_handle = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)stat_thread, &stat_params, 0, NULL);
	clock_t send = clock();
	int count = 0;
	while (off < byteBufferSize)
	{
		count++;
		// decide the size of next chunk
		int bytes = min(byteBufferSize - off, MAX_PKT_SIZE - sizeof(SenderDataHeader));
		// send chunk into socket
		if ((status = ss.Send(charBuf + off, bytes)) != STATUS_OK) {
		//if ((status = ss.Send(charBuf, bytes)) != STATUS_OK) {
			printf("error in ss.Send: %d \n", status);
		}
			// error handing: print status and quit
		off += bytes;
	}
	stat_pt->stop = true;
	double elapsedTime = (double)(clock() - send) / CLOCKS_PER_SEC;

	WaitForSingleObject(stat_handle, INFINITE);
	CloseHandle(stat_handle);
	if ((status = ss.Close()) != STATUS_OK) {
		printf("Main: \t connect failed with status %d\n", status);
		return 0;
	}

	Checksum cs;
	printf("Main: transfer finished in %.3f sec, %.2f Kbps, checksum %X\n", elapsedTime, 1000 * stat_pt->avg_rate ,cs.CRC32((unsigned char*)charBuf, byteBufferSize));
	printf("Main: estRTT %.3f, ideal rate %.2f Kbps\n", stat_pt->e_rtt, 8*(MAX_PKT_SIZE - sizeof(SenderDataHeader)) / (stat_pt->e_rtt));
	return 1;  
}


