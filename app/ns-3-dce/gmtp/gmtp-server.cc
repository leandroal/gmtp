#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <string.h>
#include <iostream>
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "gmtp.h"

using namespace std;

int main(int argc, char *argv[])
{
	int welcomeSocket, newSocket;
	struct sockaddr_in serverAddr;
	struct sockaddr_storage serverStorage;
	socklen_t addr_size;
	int media_rate = 300000; // B/s

	char filename[17];
	sprintf(filename, "server-%0.0f.log", MY_TIME(time_ms(tv)));
	FILE *log;
	log = fopen(filename, "w");
	if(log == NULL) {
		printf("Error while creating file\n");
		exit(1);
	}

	cout << "Starting GMTP Server..." << endl;
	welcomeSocket = socket(PF_INET, SOCK_GMTP, IPPROTO_GMTP);
	setsockopt(welcomeSocket, SOL_GMTP, GMTP_SOCKOPT_FLOWNAME, "1234567812345678", 16);

	cout << "Limiting tx_rate to " << media_rate << " B/s" << endl;
	setsockopt(welcomeSocket, SOL_GMTP, GMTP_SOCKOPT_MEDIA_RATE, &media_rate, sizeof(media_rate));

	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(SERVER_PORT);
	serverAddr.sin_addr.s_addr = INADDR_ANY;
	memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);

	bind(welcomeSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr));

	if(listen(welcomeSocket, 50) == 0)
		cout << "Listening\n";
	else
		cout << "Error\n";

	addr_size = sizeof serverStorage;
	newSocket = accept(welcomeSocket, (struct sockaddr *)&serverStorage,
			&addr_size);

	cout << "Connected with client!" << endl;
	print_server_log_header(log);

	double start = time_ms(tv);
	double t1  = time_ms(tv);
	int i, ndp = 0;
	const char *msg = " Hello, World!";
	double total_data, total;

	cout << "Sending data...\n" << endl;
	for(i = 0; i < 10000; ++i) {
		const char *numstr = NumStr(i+1); //Do not delete this.
		char *buffer = new char[BUFF_SIZE];
		strcpy(buffer, numstr);
		strcat(buffer, msg);
		int pkt_size = BUFF_SIZE + 36 + 20;

		//Control TX rate
		double sleep_time = (double)(pkt_size * 1000)/media_rate;
		ms_sleep(sleep_time); //control tx rate

		send(newSocket, buffer, BUFF_SIZE, 0);
		total += pkt_size;
		total_data += BUFF_SIZE;
		delete buffer;
		if(i % 1000 == 0) {
			print_stats(i, t1, total, total_data);
			cout << endl;
		}
		ndp = count_ndp_rcv(newSocket) + count_ndp_sent(newSocket);
		update_server_stats(i, ndp, log);
	}

	print_stats(i, t1, total, total_data);
	printf("Non data packets received: %d\n", count_ndp_rcv(newSocket));
	printf("Non data packets sent: %d\n", count_ndp_sent(newSocket));

	const char *outstr = "out";

	for(i = 0; i < 6; ++i) { // Send 'out' 5 times for now... gmtp-inter bug...
		printf("Sending out: %s\n", outstr);
		send(newSocket, outstr, strlen(outstr), 0);
	}

	double end = time_ms(tv);
	double duration = end - start;
	printf("Time of execution: %0.2f seconds\n\n", duration / 1000);
	printf("End of simulation...\n");

	printf("Closing server...\n");
	close(newSocket);
	close(welcomeSocket);

	printf("Server closed!\n\n");

	return 0;
}
