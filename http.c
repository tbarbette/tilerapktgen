#include "http.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

char names[500][4];

typedef struct {
	void* syn;
	//void* syn_ack;
	void* ack;

	void* get;
//	void* get_ack;

	void** responses;
	int response_count;
//	void* responses_ack[];


	void* fin;
//	void* fin_ack;
	void* fin_last;
//	void* fin_last_ack;

} http_session;

void open_tcp(http_session* session){

}

size_t http_init() {
	DIR *dfd;
	struct dirent *dp;
	char filename_qfd[100] ;

	 if ((dfd = opendir("http")) == NULL)
	 {
	  fprintf(stderr, "Can't open %s\n", "http");
	  return -1;
	 }

	 int num;
	 size_t space = 0;

	 chdir("http");

	 while ((dp = readdir(dfd)) != NULL)
	 {
	  if (dp->d_type == DT_REG && dp->d_name[0] >= '0' && dp->d_name[0] <= '9') {
		  strcpy(names[num],dp->d_name);
		 /* struct stat st;
		   stat(dp->d_name, &st);
		   space+= (((((st.st_size / 1400) + 1) * 1500) / 64) + 9) * 64;
		  num++;*/
	  }
	}

	closedir(dfd);

}

void http_genpackets(void* mem) {

}

//Send request packets then discard responses
void client_body() {
	char get_body[255];

//	for (int i = 0; i < host; i++) {

	//	sprintf(get_body,"GET / HTTP/1.1\n\rUser-Agent: Wget/1.14 (linux-gnu)\n\rAccept: */*\n\rHost: %d\n\rConnection: Keep-Alive\n\r\n\r",host);
	//}

}

//Receive requests and send responses in the send buffer
void server_body() {
	//uint8_t gxio_mpipe_idesc_get_l4_offset (gxio_mpipe_idesc_t ∗ idesc)

}
