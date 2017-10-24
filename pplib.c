#include "mpi.h"
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include<stdlib.h>
#include<unistd.h> 
#include<stdio.h>
#include <sys/time.h>
#include <pthread.h>

int MPI_ppExecPort = 0;
int MPI_ppExecConnFd = 0;
char* MPI_ppExecHostname;
int MPI_currentCommSize;
int MPI_currentRank;
int MPI_acceptSocketFdToListen;
int MPI_acceptSocketPortToListen;
char MPI_CurrentHostname[50];
int MPI_Communicators[30];//index indicate the communicator number, value in index indicate whether the communicator is taken(0 not/1 taken). Exception of MPI_COMM_World(not zero, can take zero in mpi.h if want)

MPI_Request *headTempList = NULL;//List for temp tasks- where yet to know the header info
MPI_Request *headTaskList = NULL;//List for active tasks - Mostly reading
MPI_Request *headDoneList = NULL;//List for completed tasks
MPI_Request *headSendList = NULL;//List for Isend Tasks
MPI_Request *headRecvList = NULL;//List for Irecv Tasks

int MPI_runProgressEngine = 1;//set to 0 in Finalize... use a thread ??

int lockVariable = 0;//to have synchronization while changing the Task List betweeen Progress engine and the manageAccepts thread
int waitAccepting = 0;// to be used between MPI_Recieve, MPI_Wait etc where we are trying to handle the accept for time being
int doAccepting = 1;//for the manageAccepts thread to exit
int errorRace = 0; //to indicate the race condition between thread accepts(meanwhile recv or etc set the waitAccepting to 1), recv etc will check for this variable is set. then they check the list and if the data required is avaialbe in the list(this part can be done by progress engine ?? )
pthread_t idManageAccepts;//hold the id of the manageAccepts thread

						  //store the rank data once contacted the ppexec for next use
char MPI_rankHostnames[60][60];
int MPI_rankPorts[60];

void error_check(int val, const char *str)
{
	if (val < 0)
	{
		printf("%s :%d: %s\n", str, val, strerror(errno));
		exit(1);
	}
}

int setup_to_accept()
{
	int rc, accept_socket;
	int optval = 1;
	struct sockaddr_in sin;

	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = INADDR_ANY;
	sin.sin_port = 0;//htonl(port);//if set t0 zero port is auto assigned

	accept_socket = socket(AF_INET, SOCK_STREAM, 0);
	error_check(accept_socket, "setup_to_accept socket");

	setsockopt(accept_socket, SOL_SOCKET, SO_REUSEADDR, (char *)&optval, sizeof(optval));

	rc = bind(accept_socket, (struct sockaddr *)&sin, sizeof(sin));
	error_check(rc, "setup_to_accept bind");

	rc = listen(accept_socket, 5);
	error_check(rc, "setup_to_accept listen");

	return(accept_socket);
}

int accept_connection(int accept_socket)
{
	struct sockaddr_in from;
	int fromlen, client_socket, gotit;
	int optval = 1;

	fromlen = sizeof(from);
	gotit = 0;
	while (!gotit)
	{
		client_socket = accept(accept_socket, (struct sockaddr *)&from,
			(socklen_t *)&fromlen);
		if (client_socket == -1)
		{
			/* Did we get interrupted? If so, try again */
			if (errno == EINTR)
				continue;
			else
				error_check(client_socket, "accept_connection accept");
		}
		else
			gotit = 1;
	}
	setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, (char *)&optval, sizeof(optval));
	return(client_socket);
}


int connect_to_server(char *hostname, int port)
{
	int client_socket;
	struct sockaddr_in listener;
	struct hostent *hp;
	int rc;
	int optval = 1;

	hp = gethostbyname(hostname);
	if (hp == NULL)
	{
		//  printf("connect_to_server: gethostbyname %s: %s -- exiting\n", hostname, strerror(errno));
		//exit(-1);
	}

	bzero((void *)&listener, sizeof(listener));
	bcopy((void *)hp->h_addr, (void *)&listener.sin_addr, hp->h_length);
	listener.sin_family = hp->h_addrtype;
	listener.sin_port = htons(port);

	client_socket = socket(AF_INET, SOCK_STREAM, 0);
	//error_check(client_socket, "net_connect_to_server socket");

	rc = connect(client_socket, (struct sockaddr *) &listener, sizeof(listener));


	error_check(rc, "net_connect_to_server connect");

	setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, (char *)&optval, sizeof(optval));
	//printf("%d is rc\n",rc);

	/*char buf[50]="Finalize\0";
	int n;
	n = write(client_socket, buf, 50);
	printf("%d is rc, %d is fd, %d rank, %d is n\n ",rc,client_socket,MPI_currentRank,n);*/

	/*//fd_set readfds;//fds to set for reading in select
	fd_set writefds;//fds to set for writing in select
	FD_ZERO( &writefds );
	FD_SET( client_socket, &writefds);
	char buf[50]="Finalize\0";
	int n;
	int l=0;
	l=select( FD_SETSIZE, NULL, &writefds, NULL,NULL );

	//Do the accept thing
	if (FD_ISSET(client_socket, &writefds))
	{
	//accept connection
	printf("%d fd is ready!, %d rank, %d is l\n",client_socket,MPI_currentRank,l);


	n = r(client_socket, buf, 50);
	printf("%d is rc, %d is fd, %d rank, %d is n\n ",rc,client_socket,MPI_currentRank,n);
	}*/
	if (rc<0)
		return rc;
	else
		return(client_socket);
}


void extractHeader(char *header, char *MPI_Call_Type, int *senderRank, int *senderTag, int *senderMessageSize, int  *SenderDataType, int *senderComm)
{
	char* token;//to store temp data of strtok	

	token = strtok(header, ":");
	strcpy(MPI_Call_Type, token);

	token = strtok(NULL, ":");
	*senderRank = ntohl(atoi(token));

	token = strtok(NULL, ":");
	*senderTag = ntohl(atoi(token));
	token = strtok(NULL, ":");


	*senderMessageSize = ntohl(atoi(token));

	token = strtok(NULL, ":");
	*SenderDataType = ntohl(atoi(token));

	token = strtok(NULL, ":");
	*senderComm = ntohl(atoi(token));
}




void extractHeaderToReq(char *header, MPI_Request* req)
{
	//char *MPI_Call_Type,int *senderRank,int *senderTag,int *senderMessageSize,int  *SenderDataType,int *senderComm)

	extractHeader(header, req->type, &(req->rank), &(req->tag), &(req->count), &(req->datatype), &(req->communicator));

}


//get only type from header
void getType(char *header, char *result)
{
	char headerTemp[75];
	strcpy(headerTemp, header);
	result = strtok(headerTemp, ":");
}

//read for "send" and send the header... useful for Isend ... after fd is set in the progress engine for read
int sendHeader(MPI_Request* req)
{
	int n;
	//do a read for "send"
	char reply1[10];//expecting a string "send" which is of size 4 bytes
	n = read(req->fdTocheck, reply1, 5);//Check if the socket is readt for read ??
										//Check for every read and write if there is an error and return MPI Error every where ??
	char header[75];


	//http://beej.us/guide/bgnet/output/html/multipage/htonsman.html
	//http://stackoverflow.com/questions/30398185/how-much-space-to-allocate-for-printing-long-int-value-in-string
	//***htonl - in an integer of four bytes maximum of 10 digits, when converted to string 10 bytes - 4*10(number) + 3(:)+ 1(\0) = 44
	//htons is for host to network short(2 bytes), htonl is for host to network long(4 bytes). int - 64 bit (4 bytes ), 32 bit system(2 bytes)


	//******changing the rank in the header as that of destination used by connect to destination by sends. on there side mismatch of ranks
	// like 0 is expected by rank 1 and we send 1(destination) in place of source.... we hard code the rank to MPI_currentRank.. as this functions	 is used by sends like functions and we always need source rak to identiy 
	sprintf(header, "%s:%d:%d:%d:%d:%d", req->type, htonl(MPI_currentRank), htonl(req->tag), htonl(req->count), htonl(req->datatype), htonl(req->communicator)); // Replace count with countInBytes ?? as it might not work for char
																																								 //snprintf(header,sizeof(header),"%i:%i:%i:%i",htonl(MPI_currentRank),htonl(tag),htonl(count),htonl(dataType)); // Replace count with countInBytes ?? as it might not work for char
																																								 //printf("rank %d: %s:%d:%d:%d:%d:%d is the header 007 being sent...........\n",MPI_currentRank,req->type,MPI_currentRank,req->tag,req->count,req->datatype,req->communicator);
																																								 //puts(header);
	if (strcmp(reply1, "send") == 0)
	{
		n = write(req->fdTocheck, header, 74);
		return 1;//1 for success
	}

	return 0; //0 for failure

}

//read for "send" and send the actual data useful for Isend
int sendData(MPI_Request* req)
{
	int n;
	//wait on read for a reply from the destination rank to start sending the data
	char reply[10];//expecting a string "send" which is of size 4 bytes
	n = read(req->fdTocheck, reply, 5);//Check if the socket is readt for read ??
									   //Check for every read and write if there is an error and return MPI Error every where ??

	if (strcmp(reply, "send") == 0)
	{
		//Send the actual data
		n = write(req->fdTocheck, req->buffer, req->count);//Check if the socket is ready for write ?? as part of progress engine
	}
}

void acceptAndDoAck(MPI_Request* req)
{

	int source_socket = accept_connection(MPI_acceptSocketFdToListen);//accept connection from a source rank

																	  //Receive a header on the info of the data from rank, tag,rank,count
																	  //struct MPI_Status *currentStatus=malloc(sizeof(struct MPI_Status));

																	  //MPI_Status currentStatus;

																	  //Read a header - containing rank:count:tag:datatype ................ what is the size of the header 24 bytes
																	  //the header or envelope --- MPI_Call_Type:rank:tag:size/count:datatype - 24(updated in send) bytes(23 bytes when MPI_CHAR and 22 bytes when MPI_INT)
	char header[75];//changed from 45 to 65 to 75(+20 buffer for MPI_Call_Type, +10 communicator)
	int n;

	req->fdTocheck = source_socket;

	char reply1[5];
	strcpy(reply1, "send");//should have the /0 ?
	write(req->fdTocheck, reply1, 5);
}

//receive Header and ack
void readHeaderAndAck(MPI_Request* req)
{

	char header[75];//changed from 45 to 65 to 75(+20 buffer for MPI_Call_Type, +10 communicator)
	int n;
	char reply1[5];
	strcpy(reply1, "send");//should have the /0 ?

	n = read(req->fdTocheck, header, 74);

	if (n == -1)
		sleep(30000);
	extractHeaderToReq(header, req);

	write(req->fdTocheck, reply1, 5);//"send" ack for data
}

void readActualData(MPI_Request* req)
{
	int bytes_read;

	if (req != NULL)
		bytes_read = read(req->fdTocheck, req->buffer, req->count);// be carefull while reading size of the buffer ?? change read size to countInBytes ??	
}

void addToStartListReq(char* toList, MPI_Request* newReq)
{
	MPI_Request* temp;

	if (strcmp(toList, "headTempList") == 0)
	{
		temp = headTempList;
	}
	else if (strcmp(toList, "headTaskList") == 0)
	{
		temp = headTaskList;
	}
	else if (strcmp(toList, "headDoneList") == 0)
	{
		temp = headDoneList;
	}
	else if (strcmp(toList, "headSendList") == 0)
	{
		temp = headSendList;
	}
	else if (strcmp(toList, "headRecvList") == 0)
	{
		temp = headRecvList;
	}

	if (temp == NULL)
	{
		if (strcmp(toList, "headTempList") == 0)
		{
			headTempList = newReq;
		}
		else if (strcmp(toList, "headTaskList") == 0)
		{
			headTaskList = newReq;
		}
		else if (strcmp(toList, "headDoneList") == 0)
		{
			headDoneList = newReq;
		}
		else if (strcmp(toList, "headSendList") == 0)
		{
			headSendList = newReq;
		}
		else if (strcmp(toList, "headRecvList") == 0)
		{
			headRecvList = newReq;
		}
	}
	else
	{
		//insert in starting of the list
		newReq->next = temp;

		if (strcmp(toList, "headTempList") == 0)
		{
			headTempList = newReq;
		}
		else if (strcmp(toList, "headTaskList") == 0)
		{
			headTaskList = newReq;
		}
		else if (strcmp(toList, "headDoneList") == 0)
		{
			headDoneList = newReq;
		}
		else if (strcmp(toList, "headSendList") == 0)
		{
			headSendList = newReq;
		}
		else if (strcmp(toList, "headRecvList") == 0)
		{
			headRecvList = newReq;
		}
		//list=list->next;
	}

}

void printList(char* toList)
{
	MPI_Request* temp;

	if (strcmp(toList, "headTempList") == 0)
	{
		temp = headTempList;
	}
	else if (strcmp(toList, "headTaskList") == 0)
	{
		temp = headTaskList;
	}
	else if (strcmp(toList, "headDoneList") == 0)
	{
		temp = headDoneList;
	}
	else if (strcmp(toList, "headSendList") == 0)
	{
		temp = headSendList;
	}
	else if (strcmp(toList, "headRecvList") == 0)
	{
		temp = headRecvList;
	}

	while (temp != NULL)
	{
		printf("\n--rank %d: %s is the type and %d is the fd and %d is rank,%d is tag, %s is status, %d is the statusTag, %d is the sequence\n", MPI_currentRank, temp->type, temp->fdTocheck, temp->rank, temp->tag, temp->status, temp->statusTag, temp->sequence);

		temp = temp->next;
	}

}
void addToEndListReq(char* toList, MPI_Request* newReq)
{
	MPI_Request* temp;

	if (strcmp(toList, "headTempList") == 0)
	{
		temp = headTempList;
	}
	else if (strcmp(toList, "headTaskList") == 0)
	{
		temp = headTaskList;
	}
	else if (strcmp(toList, "headDoneList") == 0)
	{
		temp = headDoneList;
	}
	else if (strcmp(toList, "headSendList") == 0)
	{
		temp = headSendList;
	}
	else if (strcmp(toList, "headRecvList") == 0)
	{
		temp = headRecvList;
	}


	if (temp == NULL)
	{

		if (strcmp(toList, "headTempList") == 0)
		{
			headTempList = newReq;
			headTempList->sequence = 0;
			newReq->sequence = 0;
		}
		else if (strcmp(toList, "headTaskList") == 0)
		{
			headTaskList = newReq;
			headTaskList->sequence = 0;
			newReq->sequence = 0;
		}
		else if (strcmp(toList, "headDoneList") == 0)
		{
			headDoneList = newReq;
			headDoneList->sequence = 0;
			newReq->sequence = 0;
		}
		else if (strcmp(toList, "headSendList") == 0)
		{
			headSendList = newReq;
			headSendList->sequence = 0;
			newReq->sequence = 0;
		}
		else if (strcmp(toList, "headRecvList") == 0)
		{
			headRecvList = newReq;
			headRecvList->sequence = 0;
			newReq->sequence = 0;
		}

	}
	else
	{
		do
		{

			if (temp->next == NULL)
			{
				//insert in starting of the list
				newReq->sequence = temp->sequence + 1;
				temp->next = newReq;
				newReq->next = NULL;
			}
			else {
			}
			temp = temp->next;
		} while (temp != NULL);
	}

}


MPI_Request* createRequest(char* sourceListHead, char* MPI_Call_Type, int fd, int read, int write, int rank, int tag, int countInBytes, int  datatype, int communicator, char *status, void *buff)
{
	MPI_Request *currentTaskPe = (MPI_Request *)malloc(sizeof(MPI_Request));

	if (MPI_Call_Type != NULL)
		strcpy(currentTaskPe->type, MPI_Call_Type);

	currentTaskPe->fdTocheck = fd;
	currentTaskPe->read = read;
	currentTaskPe->write = write;
	currentTaskPe->rank = rank;
	currentTaskPe->tag = tag;
	currentTaskPe->count = countInBytes;
	currentTaskPe->datatype = datatype;
	currentTaskPe->communicator = communicator;

	if (status != NULL)
	{
		strcpy(currentTaskPe->status, status);
	}
	else
		strcpy(currentTaskPe->status, "notDone");

	currentTaskPe->buffer = buff;
	currentTaskPe->next = NULL;

	if (sourceListHead != NULL)
	{
		addToEndListReq(sourceListHead, currentTaskPe);
	}

	return currentTaskPe;
}

void deleteFromlist(char* sourceListHead, MPI_Request* mvingReq)
{

	MPI_Request *temp;

	if (strcmp(sourceListHead, "headTempList") == 0)
	{
		temp = headTempList;
	}
	else if (strcmp(sourceListHead, "headTaskList") == 0)
	{
		temp = headTaskList;
	}
	else if (strcmp(sourceListHead, "headDoneList") == 0)
	{
		temp = headDoneList;
	}
	else if (strcmp(sourceListHead, "headSendList") == 0)
	{
		temp = headSendList;
	}
	else if (strcmp(sourceListHead, "headRecvList") == 0)
	{
		temp = headRecvList;
	}


	if ((strcmp(temp->type, mvingReq->type) == 0) && (temp->read == mvingReq->read) && (temp->write == mvingReq->write) && (temp->rank == mvingReq->rank) && (temp->tag == mvingReq->tag) && (temp->count == mvingReq->count) && (temp->datatype == mvingReq->datatype) && (temp->communicator == mvingReq->communicator) && (strcmp(temp->status, mvingReq->status) == 0))
	{
		if (strcmp(sourceListHead, "headTempList") == 0)
		{
			headTempList = headTempList->next;
		}
		else if (strcmp(sourceListHead, "headTaskList") == 0)
		{
			headTaskList = headTaskList->next;
		}
		else if (strcmp(sourceListHead, "headDoneList") == 0)
		{
			headDoneList = headDoneList->next;
		}
		else if (strcmp(sourceListHead, "headSendList") == 0)
		{
			headSendList = headSendList->next;
		}
		else if (strcmp(sourceListHead, "headRecvList") == 0)
		{
			headRecvList = headRecvList->next;
		}

		//addToEndListReq(headDoneList,mvingReq);
		free(temp);
	}
	else {

		while (temp != NULL)
		{
			//check next one is matching?
			if ((strcmp(temp->next->type, mvingReq->type) == 0) && (temp->next->read == mvingReq->read) && (temp->next->write == mvingReq->write) && (temp->next->rank == mvingReq->rank) && (temp->next->tag == mvingReq->tag) && (temp->next->count == mvingReq->count) && (temp->next->datatype == mvingReq->datatype) && (temp->next->communicator == mvingReq->communicator) && (strcmp(temp->next->status, mvingReq->status) == 0))
			{
				//last one
				if (temp->next->next == NULL)
				{
					MPI_Request *temp1;
					temp1 = temp->next;
					temp->next = NULL;
					//free(temp1);
					break;
				}
				else
				{
					MPI_Request *temp1;
					temp1 = temp->next;
					temp->next = temp->next->next;
					//free(temp1);					
					break;
				}

				break;
			}
			temp = temp->next;
		}
	}
}

//Move any request from a list to headDoneList(insertion at end)
void moveBtwnLists(char* sourceListHead, char* destinationListHead, MPI_Request* mvingReq)
{
	MPI_Request *temp;
	if (strcmp(sourceListHead, "headTempList") == 0)
	{
		temp = headTempList;
	}
	else if (strcmp(sourceListHead, "headTaskList") == 0)
	{
		temp = headTaskList;
	}
	else if (strcmp(sourceListHead, "headDoneList") == 0)
	{
		temp = headDoneList;
	}
	else if (strcmp(sourceListHead, "headSendList") == 0)
	{
		temp = headSendList;
	}
	else if (strcmp(sourceListHead, "headRecvList") == 0)
	{
		temp = headRecvList;
	}

	if ((strcmp(temp->type, mvingReq->type) == 0) && (temp->read == mvingReq->read) && (temp->write == mvingReq->write) && (temp->rank == mvingReq->rank) && (temp->tag == mvingReq->tag) && (temp->count == mvingReq->count) && (temp->datatype == mvingReq->datatype) && (temp->communicator == mvingReq->communicator) && (strcmp(temp->status, mvingReq->status) == 0))
	{
		if (strcmp(sourceListHead, "headTempList") == 0)
		{
			headTempList = headTempList->next;
		}
		else if (strcmp(sourceListHead, "headTaskList") == 0)
		{
			headTaskList = headTaskList->next;
		}
		else if (strcmp(sourceListHead, "headDoneList") == 0)
		{
			headDoneList = headDoneList->next;
		}
		else if (strcmp(sourceListHead, "headSendList") == 0)
		{
			headSendList = headSendList->next;
		}
		else if (strcmp(sourceListHead, "headRecvList") == 0)
		{
			headRecvList = headRecvList->next;
		}
		addToEndListReq(destinationListHead, mvingReq);
		//free(temp); 
	}
	else {
		while (temp != NULL)
		{
			//check next one is matching?
			if ((strcmp(temp->next->type, mvingReq->type) == 0) && (temp->next->read == mvingReq->read) && (temp->next->write == mvingReq->write) && (temp->next->rank == mvingReq->rank) && (temp->next->tag == mvingReq->tag) && (temp->next->count == mvingReq->count) && (temp->next->datatype == mvingReq->datatype) && (temp->next->communicator == mvingReq->communicator) && (strcmp(temp->next->status, mvingReq->status) == 0))
			{
				//last one
				if (temp->next->next == NULL)
				{
					MPI_Request *temp1;
					temp1 = temp->next;
					temp->next = NULL;
					addToEndListReq(destinationListHead, mvingReq);
					//free(temp1);
					break;
				}
				else
				{
					MPI_Request *temp1;
					temp1 = temp->next;
					temp->next = temp->next->next;
					//free(temp1);
					temp1->next = NULL;
					addToEndListReq(destinationListHead, mvingReq);
					break;
				}

				break;
			}
			temp = temp->next;
		}
	}


}

MPI_Request* addToList(char* list, char *MPI_Call_Type, int fd, int read, int write, int rank, int tag, int countInBytes, int  datatype, int communicator, char *status, void *buff)
{
	MPI_Request *currentTaskPe = (MPI_Request *)malloc(sizeof(MPI_Request));
	strcpy(currentTaskPe->type, MPI_Call_Type);
	currentTaskPe->fdTocheck = fd;

	currentTaskPe->read = read;
	currentTaskPe->write = write;
	currentTaskPe->rank = rank;
	currentTaskPe->tag = tag;
	currentTaskPe->count = countInBytes;
	currentTaskPe->datatype = datatype;
	currentTaskPe->communicator = communicator;

	if (status != NULL)
		strcpy(currentTaskPe->status, status);
	else
		strcpy(currentTaskPe->status, "notDone");

	currentTaskPe->buffer = buff;
	currentTaskPe->next = NULL;
	addToEndListReq(list, currentTaskPe);

	return currentTaskPe;

}

//Cmpare two Requests --- matches return 1, else return 0; req is the current rank MPI call.... can have MPI_ANY_TAG, MPI_ANY_SOURCE
int compareRequests(MPI_Request *temp, MPI_Request *req)
{
	//want to do this looping thing in PE as soon as you enter the progress engine ?? for MPI_recieve and MPI_Wait

	if (temp != NULL && req != NULL)
	{
		// Check if these conditions hold good ??  ... may fail interms of source if used for bsend and brecv
		if ((temp->rank == (req->rank) || req->rank == MPI_ANY_SOURCE) && ((req->tag) == temp->tag || req->tag == MPI_ANY_TAG) && (temp->count == (req->count)) && (temp->datatype == (req->datatype)) && (temp->communicator == (req->communicator)))
		{
			//Return 
			return 1;
		}
		else {
			//printf(("COmpare REquest sisn't match\n");
		}

		temp = temp->next;
	}

	return 0;//not  matching Info
}

MPI_Request* searchInList(char* list, MPI_Request *req)
{
	MPI_Request *temp;

	if (strcmp(list, "headTempList") == 0)
	{
		temp = headTempList;
	}
	else if (strcmp(list, "headTaskList") == 0)
	{
		temp = headTaskList;
	}
	else if (strcmp(list, "headDoneList") == 0)
	{
		temp = headDoneList;
	}
	else if (strcmp(list, "headSendList") == 0)
	{
		temp = headSendList;
	}
	else if (strcmp(list, "headRecvList") == 0)
	{
		temp = headRecvList;
	}
	//want to do this looping thing in PE as soon as you enter the progress engine ?? for MPI_recieve and MPI_Wait

	if (temp == NULL)

		while (temp != NULL)
		{
			// Check if these conditions hold good ??  ... may fail interms of source if used for bsend and brecv
			if ((temp->rank == (req->rank) || temp->rank == MPI_ANY_SOURCE) && ((req->tag) == temp->tag || temp->tag == MPI_ANY_TAG) && (temp->count == (req->count)) && (temp->datatype == (req->datatype)) && (temp->communicator == (req->communicator)))
			{
				//Return 
				return temp; // double check if   temp or *temp ??
			}

			temp = temp->next;
		}


	return NULL;//not found matching Info
}

MPI_Request* searchInListNonBlocking(char* list, MPI_Request *req)
{
	MPI_Request *temp;

	if (strcmp(list, "headTempList") == 0)
	{
		temp = headTempList;
	}
	else if (strcmp(list, "headTaskList") == 0)
	{
		temp = headTaskList;
	}
	else if (strcmp(list, "headDoneList") == 0)
	{
		temp = headDoneList;
	}
	else if (strcmp(list, "headSendList") == 0)
	{
		temp = headSendList;
	}
	else if (strcmp(list, "headRecvList") == 0)
	{
		temp = headRecvList;
	}

	while (temp != NULL)
	{
		// Check if these conditions hold good ??  ... may fail interms of source if used for bsend and brecv
		if ((strcmp(temp->type, req->type) == 0) && (temp->rank == (req->rank)) && ((req->tag) == temp->tag) && (temp->count == (req->count)) && (temp->datatype == (req->datatype)) && (temp->communicator == (req->communicator)) && (temp->sequence == req->sequence))//&& (strcmp(temp->status,"done")!=0))
		{
			return temp; // double check if   temp or *temp ??
		}
		else
			//printf("\n\n%s is list,rank %d:%d req->rank,%d req->tag,%d req->count,%d req->datatype,%d  req->communicator................_____\n\n",list,MPI_currentRank,req->rank,req->tag,req->count, req->datatype, req->communicator);

			temp = temp->next;
	}

	return NULL;//not found matching Info
}

MPI_Request* searchInListNonBlockingPE(char* list, MPI_Request *req)
{
	MPI_Request *temp;

	if (strcmp(list, "headTempList") == 0)
	{
		temp = headTempList;
	}
	else if (strcmp(list, "headTaskList") == 0)
	{
		temp = headTaskList;
	}
	else if (strcmp(list, "headDoneList") == 0)
	{
		temp = headDoneList;
	}
	else if (strcmp(list, "headSendList") == 0)
	{
		temp = headSendList;
	}
	else if (strcmp(list, "headRecvList") == 0)
	{
		temp = headRecvList;
	}
	//want to do this looping thing in PE as soon as you enter the progress engine ?? for MPI_recieve and MPI_Wait

	while (temp != NULL)
	{
		// Check if these conditions hold good ??  ... may fail interms of source if used for bsend and brecv
		if (((temp->rank == req->rank) || (temp->rank == MPI_ANY_SOURCE)) && ((req->tag == temp->tag) || (temp->tag == MPI_ANY_TAG)) && (temp->count == req->count) && (temp->datatype == req->datatype) && (temp->communicator == (req->communicator)) && (strcmp(temp->status, "done") != 0))
		{
			//Return 
			return temp; // double check if   temp or *temp ??
		}
		temp = temp->next;
	}

	return NULL;//not found matching Info
}

void MPIsum(void *inp, void *inoutp, int *len, MPI_Datatype *dptr)
{
	int i;
	int r;
	int *in = (int *)inp;
	int *inout = (int *)inoutp;

	for (i = 0; i < *len; i++)
	{
		r = *in + *inout;
		*inout = r;
		in++;
		inout++;
	}
}

//No parameter names ?
int MPI_Init(int *argc, char ***argv)
{
	//pthread_create(&idManageAccepts,NULL,manageAccepts,NULL);

	char buf[1024];
	int i = 0;
	char* RANK = getenv("PP_MPI_RANK");
	MPI_currentRank = atoi(RANK);
	char* SIZE = getenv("PP_MPI_SIZE");
	MPI_currentCommSize = atoi(SIZE);
	MPI_Communicators[0] = 1; //MPI_COMM_World taken
	opFuncPtr[MPI_SUM] = MPIsum;
	int l = 0;

	for (l = 1; l < 30; l++)
	{
		MPI_Communicators[l] = 0;//not taken
		opFuncPtr[l] = NULL;
	}

	//Retreive ppExecPort,ppExecHostname
	char* token;//to store temp data of strtok
	char* PP_MPI_HOST_PORT = getenv("PP_MPI_HOST_PORT");

	token = strtok(PP_MPI_HOST_PORT, ":");
	MPI_ppExecHostname = strdup(token);

	token = strtok(NULL, ":");
	MPI_ppExecPort = atoi(token);

	//Fetch a unused port??	
	//setup an accespt socket to use in MPI read
	MPI_acceptSocketFdToListen = setup_to_accept();

	struct sockaddr_in serv_addr;
	socklen_t len = sizeof(serv_addr);

	if (getsockname(MPI_acceptSocketFdToListen, (struct sockaddr *)&serv_addr, &len) == -1)
	{
		perror("getsockname");
		return;
	}

	//Assign to global variable
	MPI_acceptSocketPortToListen = ntohs(serv_addr.sin_port);

	gethostname(MPI_CurrentHostname, 50);

	for (i = 0; i < 60; i++)
	{
		MPI_rankHostnames[i][0] = '\0';
		MPI_rankPorts[i] = 0;
	}

	sprintf(buf, "%d:%s:%d", MPI_currentRank, MPI_CurrentHostname, MPI_acceptSocketPortToListen); //How to get the port number or which port number	
	MPI_ppExecConnFd = connect_to_server(MPI_ppExecHostname, MPI_ppExecPort);//Connecting to PPEXEC to send the details of own port and host name to connect on

	int n;
	n = write(MPI_ppExecConnFd, buf, 1024);

	return MPI_SUCCESS;
}

int MPI_Finalize(void)
{
	char buf[50] = "Finalize\0";
	int n;
	n = write(MPI_ppExecConnFd, buf, 1024);
	n = 0;
	char buf1[5];

	do {
		n = read(MPI_ppExecConnFd, buf1, 5);

		if (strcmp(buf1, "done") == 0)
		{
			break;
		}
		memset(buf1, '\0', 5);
	} while (1);

	doAccepting = 0;//stops the thread

	return MPI_SUCCESS;
}


int MPI_Barrier(MPI_Comm comm)
{
	//Validate Communicator
	if (comm != 0)
	{
		if (MPI_Communicators[comm] == 0)
		{
			return MPI_ERR_COMM;
		}
	}

	int fds[60];
	int i = 0;
	int n;
	int fd;
	int fdRankZero;
	char reqSockDetails[1024];//Of rank zero(root)
	char sockDetails[41];

	if (MPI_currentRank == 0)
	{
		MPI_Request* currentTaskPe;
		char* dataTosend;
		currentTaskPe = createRequest(NULL, "MPI_Barrier", 0, 1, 0, 0, 1030301, 5, MPI_CHAR, comm, NULL, dataTosend);
		i = 0;
		n = MPI_currentCommSize;
		while (i < n - 1)
		{
			progressEngine(currentTaskPe);
			if (strcmp(currentTaskPe->status, "done") == 0)
			{
				fds[i] = currentTaskPe->fdTocheck;
				i++;
				currentTaskPe->fdTocheck = 0;
				strcpy(currentTaskPe->status, "notDone");
			}
		}

		waitAccepting = 1;

		//Implement the 2 phase commit kindof conecpt here.. having too loops

		int rc;//number of connections that are ready, identified by select
		fd_set readfds;
		struct timeval tv;
		FD_ZERO(&readfds);

		//loop through the list of tasks to check if there is any barrier things accepted by recv or any where, accept stuff is done
		//Expected: The after accepting will do "send" and get the header and see if it is bsend,or barrier or gather stuff, then don't send the
		// the "send" to get the actual data

		i = 0;
		char reply[5];
		strcpy(reply, "send");//should have the /0 ?
		char *reply2;
		reply2 = malloc(5);

		//loop through and send ok to all the processes --expected by other processes to wait on read
		while (i < n - 1)
		{
			i++;
		}

		i = 0;
		while (i < n - 1)
		{
			close(fds[i]);
			i++;
		}
		waitAccepting = 0;
	}
	else
	{
		//// to use the progressEngine to do the work of barrier and it is like a send
		//have to send rank zero, a data called "done" with tag 1030301		
		char *dataTosend = malloc(5);
		strcpy(dataTosend, "done");
		MPI_Request* currentTaskPe;

		currentTaskPe = createRequest(NULL, "MPI_Barrier", 0, 0, 1, 0, 1030301, 5, MPI_CHAR, comm, NULL, dataTosend);//setting write to 1 to differentiatte b/w rank 0(read =1)
		while (strcmp(currentTaskPe->status, "done") != 0)
		{
			progressEngine(currentTaskPe);
		}
	}
	//close(fdRankZero);
	return MPI_SUCCESS;
}

double MPI_Wtime(void)
{
	struct timeval tv;

	gettimeofday(&tv, (struct timezone *) 0);
	return (tv.tv_sec + (tv.tv_usec / 1000000.0));
}

int fetchRankDetails(int rankRequired)
{
	char reqSockDetails[1024];
	char sockDetails[41];
	int fdSocketDestination = 0;
	int n;

	sprintf(reqSockDetails, "SockDetails:%d", rankRequired);

	//ask ppexec for socket details of the destination rank
	n = write(MPI_ppExecConnFd, reqSockDetails, 1024);

	//Read the rank socket details here from ppexec
	n = read(MPI_ppExecConnFd, sockDetails, 40);// check to change the read lenght size to exact by checking the aprrox byte size including htonl etc for integers

												//extract the details of the destinaton rank	
	char* token;//to store temp data of strtok
	char* PP_MPI_HOST_PORT = getenv("PP_MPI_HOST_PORT");

	token = strtok(sockDetails, ":");
	strcpy(MPI_rankHostnames[rankRequired], token);

	token = strtok(NULL, ":");
	MPI_rankPorts[rankRequired] = atoi(token);

	//connect to destination rank
	fdSocketDestination = connect_to_server(MPI_rankHostnames[rankRequired], MPI_rankPorts[rankRequired]);

	return fdSocketDestination;
}

int MPI_Comm_dup(MPI_Comm comm, MPI_Comm *newcomm)
{
	//Validate Communicator
	if (comm != 0)
	{
		if (MPI_Communicators[comm] == 0)
		{
			return MPI_ERR_COMM;
		}
	}

	int l = 0;
	for (l = 1; l<30; l++)
	{
		if (MPI_Communicators[l] == 0)
		{
			*newcomm = l;
			MPI_Communicators[l] = 1;
			break;
		}

		//MPI_Communicators[l]=0;//not taken
	}
	return MPI_SUCCESS;
}


int MPI_Comm_size(MPI_Comm comm, int *size)
{
	//Validate Communicator
	if (comm != 0)
	{
		if (MPI_Communicators[comm] == 0)
		{
			return MPI_ERR_COMM;
		}
	}

	*size = MPI_currentCommSize;

	return MPI_SUCCESS;
}

int MPI_Comm_rank(MPI_Comm comm, int *rank)
{
	//Validate Communicator
	if (comm != 0)
	{
		if (MPI_Communicators[comm] == 0)
		{
			return MPI_ERR_COMM;
		}
	}

	*rank = MPI_currentRank;

	return MPI_SUCCESS;
}


//Change status of all elements matching in a list to new status
void updateStatusInList(char* toList, char *oldStatus, char *newStatus)
{
	MPI_Request *temp;

	//want to do this looping thing in PE as soon as you enter the progress engine ?? for MPI_recieve and MPI_Wait

	if (strcmp(toList, "headTempList") == 0)
	{
		temp = headTempList;
	}
	else if (strcmp(toList, "headTaskList") == 0)
	{
		temp = headTaskList;
	}
	else if (strcmp(toList, "headDoneList") == 0)
	{
		temp = headDoneList;
	}
	else if (strcmp(toList, "headSendList") == 0)
	{
		temp = headSendList;
	}
	else if (strcmp(toList, "headRecvList") == 0)
	{
		temp = headRecvList;
	}

	while (temp != NULL)
	{

		if (strcmp(temp->status, oldStatus) == 0)
		{
			strcpy(temp->status, newStatus);
		}

		temp = temp->next;
	}
}


//Divide the progress engine into acccepts and progress parts(sending and recieving etc)
int progressEngine(MPI_Request *current)
{
	MPI_Request *temp;
	fd_set readfds;//fds to set for reading in select
	fd_set readfds1;//fds to set for reading in select
					//fd_set writefds;//fds to set for writing in select
	FD_ZERO(&readfds);
	FD_ZERO(&readfds1);
	//FD_ZERO( &writefds );
	int currentTaskStatus = 1;
	MPI_Request **tempList;//to temporarly store the list elements that have to be check by select ..... also all those will be accept or read	
	int returnCount = 0;
	struct timeval tv;

	//loop through list and add the fds to select
	temp = headTaskList;

	//Give the actual status for tasks that are moved from temp list to TaskList(General List)
	updateStatusInList("headTaskList", "changeToReceivedHeader", "receivedHeader");
	updateStatusInList("headRecvList", "changeToReceivedHeader", "receivedHeader");

	//Do all accepts available
	FD_ZERO(&readfds);

	//add accept socket to select
	FD_SET(MPI_acceptSocketFdToListen, &readfds);

	tv.tv_sec = 2;
	tv.tv_usec = 0;

	returnCount = select(FD_SETSIZE, &readfds, NULL, NULL, &tv);

	//Do the accept thing
	if (FD_ISSET(MPI_acceptSocketFdToListen, &readfds))
	{
		while (returnCount > 0)
		{
			//accept connection and do send and add them to temp list, in next iteration we will add this fd to read and examine the headers to move them to appropriate list
			MPI_Request* tempReq;

			//status connected					
			tempReq = createRequest("headTempList", NULL, 0, 0, 0, -1, -1, -1, -1, -1, "connected", NULL);
			//fd is set in here on accept
			acceptAndDoAck(tempReq);
			//printf("%d is the fd created createRequest\n",tempReq->fdTocheck);
			returnCount--;
		}
	}

	//Do send if current is not null and it is MPI_Send, first make a connection	
	if (current != NULL)
	{
		if (((strcmp(current->type, "MPI_Send") == 0) || (strcmp(current->type, "MPI_Ssend") == 0) || ((strcmp(current->type, "MPI_Barrier") == 0) && (current->write == 1))))
		{
			if (current->fdTocheck == 0)//Not connected
			{
				int returnValue;

				returnValue = fetchRankDetails(current->rank);//connect to the required destination, rest will be taken care by the select part of progress engine

				if (returnValue > 0)
				{
					current->fdTocheck = returnValue;
					strcpy(current->status, "connectedSend");
				}
			}
		}
	}

	//Do the send stuff here --Isend -- loop on the
	//headSendList
	if (headSendList != NULL)
	{
		MPI_Request *temp1;
		temp1 = headSendList;
		while (temp1 != NULL)
		{
			if (temp1->fdTocheck == 0)//Not connected
			{
				int returnValue;

				returnValue = fetchRankDetails(temp1->rank);//connect to the required destination, rest will be taken care by the select part of progress engine

				if (returnValue > 0)
				{
					temp1->fdTocheck = returnValue;
					strcpy(temp1->status, "connectedSend");
				}
			}
			temp1 = temp1->next;
		}
	}

	FD_ZERO(&readfds1);
	//MPI_Request *temp;
	temp = NULL;
	temp = headSendList;

	//Add the Isend fd that are not done, once done they are moved to done list ??
	while (temp != NULL)
	{
		if ((temp->fdTocheck > 0) && (strcmp(temp->status, "done") != 0))
		{
			FD_SET(temp->fdTocheck, &readfds1);
		}

		temp = temp->next;
	}

	//take care of Irecv ?? nothing to do with Irecv stuff till the time we got a request in tempList and go the header for examination
	//how ever if from header it is identified as Irecv, then copy the data from item from temp list and can delete the item from temp list

	temp = NULL;
	temp = headRecvList;

	while (temp != NULL)
	{
		if ((temp->fdTocheck > 0) && (strcmp(temp->status, "done") != 0))
		{
			FD_SET(temp->fdTocheck, &readfds1);
		}

		temp = temp->next;
	}

	//Set the other tasks in general list
	temp = NULL;
	temp = headTaskList;

	while (temp != NULL)
	{
		if ((temp->fdTocheck > 0) && (strcmp(temp->status, "done") != 0))
		{
			FD_SET(temp->fdTocheck, &readfds1);
		}

		temp = temp->next;
	}



	//Set the other tasks in headTempList, all items in this are the ones with connected status and yet to know their headers
	temp = NULL;
	temp = headTempList;
	while (temp != NULL)
	{
		if ((temp->fdTocheck > 0) && (strcmp(temp->status, "connected") == 0))
		{
			FD_SET(temp->fdTocheck, &readfds1);
		}

		temp = temp->next;
	}

	if (current != NULL)
	{
		//Set the current item fd to check of its status is "receivedHeader" and fd is >0... return back if this fd is set then read the data and return to MPI_Rev call
		if ((strcmp(current->type, "MPI_Recv") == 0) && (strcmp(current->status, "receivedHeader") == 0) && (current->fdTocheck > 0))
		{
			FD_SET(current->fdTocheck, &readfds1);
		}
		else if (((strcmp(current->type, "MPI_Send") == 0) || (strcmp(current->type, "MPI_Ssend") == 0) || ((strcmp(current->type, "MPI_Barrier") == 0) && (current->write == 1))) && (strcmp(current->status, "done") != 0) && (current->fdTocheck > 0))//MPI_Send, had a connection and has to read for "send"
		{
			FD_SET(current->fdTocheck, &readfds1);
		}
	}

	tv.tv_sec = 2;
	tv.tv_usec = 0;
	returnCount = -1;
	returnCount = select(FD_SETSIZE, &readfds1, NULL, NULL, &tv);

	if (returnCount == 0) // check this condition as indefinite loop ?? break condition
	{
		//inrement a variable and check like count is <3 only with other conditions continue
		//printf("select timed out\n");
		//continue;
	}

	if (returnCount == -1 && errno == EINTR)
	{
		//printf("select interrupted; continuing\n");
		//continue;
	}

	if (returnCount < 0)
	{
		perror("select failed");
	}

	if (returnCount > 0)
	{


		if (current != NULL)
		{
			if ((current->fdTocheck > 0) && (strcmp(current->status, "receivedHeader") == 0))
			{
				if (FD_ISSET(current->fdTocheck, &readfds1))
				{
					//read for data.... and change status to done and return...
					readActualData(current);
					strcpy(current->status, "done");
				}
			}
			else if (((strcmp(current->type, "MPI_Send") == 0) || (strcmp(current->type, "MPI_Ssend") == 0) || ((strcmp(current->type, "MPI_Barrier") == 0) && (current->write == 1))) && (current->fdTocheck > 0))//MPI_Send, has to read for "send"
			{
				if (FD_ISSET(current->fdTocheck, &readfds1))
				{
					if (strcmp(current->status, "connectedSend") == 0)
					{
						//send the header
						sendHeader(current);

						strcpy(current->status, "sentHeader");
					}
					else if (strcmp(current->status, "sentHeader") == 0)
					{
						//send actual data
						sendData(current);
						strcpy(current->status, "done");
					}
				}
			}
		}

		//check from temp list - read for header if set and change status to receivedHeader
		temp = NULL;
		temp = headTempList;

		while (temp != NULL)
		{
			if (FD_ISSET(temp->fdTocheck, &readfds1))
			{

				readHeaderAndAck(temp);//request is filled with header data... check for the header retrieved values			
				strcpy(temp->status, "receivedHeader");

				//check for the currenttask fd.. what if it is duplicated in list and here.. it won't as mostly it will be Recv,Brecv			
				if (strcmp(temp->type, "MPI_Isend") == 0)//if Isend or send... check if there is a matching Irecv, then copy the fd and status to the irecv in its list.... let the actual data transfer take place in MPI_Wait?
				{
					//check if there is any Irecv pending in recvlist
					MPI_Request *temp1;
					temp1 = searchInListNonBlockingPE("headRecvList", temp);

					if (temp1 != NULL)// if there is a matching Irecv.. waiting
					{
						temp1->fdTocheck = temp->fdTocheck;
						strcpy(temp1->status, temp->status);

						//filling the actual source rank and Tag fo the purpose of &status checking in MPI_Irecv or Recv
						if (temp1->rank == MPI_ANY_SOURCE)
							temp1->statusSource = temp->rank;//if the current here is *******MPI_ANY_SOURCE in recv, then you won't knw from which rank you got the data

															 //even same is the case with tag.... if you have MPI_ANY_TAG in rev or Irecv, in &status param you have to fill the tag from where you got data									
						if (temp1->tag == MPI_ANY_TAG)
							temp1->statusTag = temp->tag;

						//remove temp from the the temp list ... but watchout... 
						//if deleted and you want to do temp=temp->next might give an error or you can, intialize that to the head again simply
						//delete it from the headTempList
						deleteFromlist("headTempList", temp);
						temp = headTempList;//prevents the issue of null segfault while doing temp=temp->next in while loop

											/*Now how the same issue... i have set the status as receivedHeader and next loop i am going to do a check in the recvlist to progress and we didn't ddo the select again
											Use the technique of intermediate state before receivedHeader..... "changeToReceivedHeader"
											*/
						strcpy(temp1->status, "changeToReceivedHeader");
					}
					else if (current != NULL)
					{
						//check if it matching the current is recv
						if ((strcmp(current->type, "MPI_Recv") == 0) && compareRequests(temp, current))// How can you justify if all the tags, communicator,  etc matched
						{
							//just copy the fd and new status to current and we will 
							//check this fd is set by checking the current- fd>0 and status receivedHeader, so that there is data avaialable on fd , because while receiving the header we asked for data by doing "send"... 
							//.... this approach will work for both Isend and Send... recv... now have to do nothing in MPI_Recv mpi call.. everything is done here in progress engine

							current->fdTocheck = temp->fdTocheck;
							strcpy(current->status, temp->status);

							if (current->rank == MPI_ANY_SOURCE)
								current->statusSource = temp->rank;//if the current here is *******MPI_ANY_SOURCE in recv, then you won't knw from which rank you got the data

																   //even same is the case with tag.... if you have MPI_ANY_TAG in rev or Irecv, in &status param you have to fill the tag from where you got data									
							if (current->tag == MPI_ANY_TAG)
								current->statusTag = temp->tag;

							deleteFromlist("headTempList", temp);
							temp = headTempList;//prevents the issue of null segfault while doing temp=temp->next in while loop

						}
						else
						{
							//Nothing Matches for the ISend then move it to the general list
							strcpy(temp->status, "changeToReceivedHeader");	//Temporary state... to skip this iteation and check the  fd in next iteraion
							temp->buffer = malloc(temp->count + 1);
							moveBtwnLists("headTempList", "headTaskList", temp);//Move from headTempList to headTaskList
						}
					}
					else {
						//Nothing Matches for the ISend then move it to the general list
						//** Have to malloc the buffer with the count(in its request)... will do this before we try to read the content... that next select will say that there is data then have to malloc

						/*Issues....... if i move this item to General list... right after looping through this temp loop i will check the general loop. The fd is already set in the readfds by select!, so have to
						have a solution to prevent this situations...for similar moves from temp list to genearl list, where the fd is set by select for current loop */

						/*Introduce a new intermediate state - "changeToReceivedHeader", will check for these states in the starting of the PE and change them to headerReceived

						*/

						strcpy(temp->status, "changeToReceivedHeader");	//Temporary state... to skip this iteation and check the  fd in next iteraion
																		//as this is Isend from another rank and didn't match any receives on this rank, so if incase data is being received.. the buffer is
																		//not yet allocated... so allocate the memory for buffer
																		//temp->buffer=malloc(temp->count+1);
						moveBtwnLists("headTempList", "headTaskList", temp);//Move from headTempList to headTaskList

					}
				}
				else if (((strcmp(temp->type, "MPI_Send") == 0) || (strcmp(temp->type, "MPI_Ssend") == 0)))
				{
					//Check if there is any irecv waiting or if 					
					//check if there is any Irecv pending in recvlist
					MPI_Request *temp1;
					temp1 = searchInListNonBlockingPE("headRecvList", temp);

					if (temp1 != NULL)
					{
						temp1->fdTocheck = temp->fdTocheck;

						if (temp1->rank == MPI_ANY_SOURCE)
							temp1->statusSource = temp->rank;//if the current here is *******MPI_ANY_SOURCE in recv, then you won't knw from which rank you got the data

															 //even same is the case with tag.... if you have MPI_ANY_TAG in rev or Irecv, in &status param you have to fill the tag from where you got data									
						if (temp1->tag == MPI_ANY_TAG)
							temp1->statusTag = temp->tag;

						strcpy(temp1->status, temp->status);	//just move the status and fd to Irecv it has all the required details... will progress to receiving the data while checking the listofIrecv..if anything is avaialbe on select

																//remove temp from the the temp list ... but watchout... 
																//if deleted and you want to do temp=temp->next might give an error or you can, intialize that to the head again simply
						deleteFromlist("headTempList", temp);
						temp = headTempList;//prevents the issue of null segfault while doing temp=temp->next in while loop

						strcpy(temp1->status, "changeToReceivedHeader");//To let the select happen(Skip to another list)
					}
					else if (current != NULL)
					{
						//check if it matching the current is recv						
						if ((strcmp(current->type, "MPI_Recv") == 0) && compareRequests(temp, current))
						{

							current->fdTocheck = temp->fdTocheck;

							if (current->rank == MPI_ANY_SOURCE)
								current->statusSource = temp->rank;//if the current here is *******MPI_ANY_SOURCE in recv, then you won't knw from which rank you got the data

							if (current->tag == MPI_ANY_TAG)
								current->statusTag = temp->tag;

							strcpy(current->status, temp->status);
							deleteFromlist("headTempList", temp);
							temp = headTempList;//prevents the issue of null segfault while doing temp=temp->next in while loop.. this fix is not working as headTempList is also NULL after one item and deletion... will work if there is atleast one element after deletion

						}
						else		//is this really required for Send, when nothing is matching......... **********Bsend....... requires a matching receiving to be already posted... at this place we can see that nothing is posted or matches.. Irecv in list and current for recv call*******					
						{		//Nothing Matches for the Send then move it to the general list

							strcpy(temp->status, "notDone");
							moveBtwnLists("headTempList", "headTaskList", temp);//Move from headTempList to headTaskList
						}


					}
					else {
						// Send and notDone status can checked for matching
						strcpy(temp->status, "notDone");	//notDone and it is not receivedHeader because  i need not progress a blocking send... if i do then the send on other side will complete and it is not supposed to happen
						moveBtwnLists("headTempList", "headTaskList", temp);//Move from headTempList to headTaskList
					}
				}
				else if (strcmp(temp->type, "MPI_Barrier") == 0)//look into this later
				{
					//else if Brecv? or Barrier or Gather or.... do the passing of current  in Brecv(instead of barrier or gather or bcast)... won't do the passing of current in Bsend side 
					//check if Current 	
					if (current != NULL)
					{
						//check if it matching the current is recv									
						if ((strcmp(current->type, "MPI_Barrier") == 0) && (current->communicator == temp->communicator) && (current->read == 1) && (strcmp(current->status, "done") != 0))
						{
							strcpy(current->status, "done");
							deleteFromlist("headTempList", temp);
							temp = headTempList;//prevents the issue of null segfault while doing temp=temp->next in while loop.. this fix is not working as headTempList is also NULL after one item and deletion... will work if there is atleast one element after deletion

						}
					}
				}
				else if (strcmp(temp->type, "Gsend") == 0)
				{
					if (current != NULL)
					{
						//check if it matching the current is recv					
						if ((strcmp(current->type, "MPI_Gather") == 0) && (current->communicator == temp->communicator) && (current->tag == temp->tag) && (strcmp(current->status, "done") != 0))
						{
							strcpy(current->status, "done");
							current->fdTocheck = temp->fdTocheck;
							current->rank = temp->rank;

							deleteFromlist("headTempList", temp);
							temp = headTempList;//prevents the issue of null segfault while doing temp=temp->next in while loop.. this fix is not working as headTempList is also NULL after one item and deletion... will work if there is atleast one element after deletion

						}
						else if ((strcmp(current->type, "Brecv") == 0) && (current->rank == temp->rank) && (current->communicator == temp->communicator) && (current->tag == temp->tag) && (strcmp(current->status, "done") != 0))
						{
							strcpy(current->status, "done");
							current->fdTocheck = temp->fdTocheck;
							//current->rank=temp->rank;
							deleteFromlist("headTempList", temp);
							temp = headTempList;
						}
					}
				}
				else if (strcmp(temp->type, "Bsend") == 0)
				{
					if (current != NULL)
					{

						if ((strcmp(current->type, "Brecv") == 0) && (current->rank == temp->rank) && (current->communicator == temp->communicator) && (current->tag == temp->tag) && (strcmp(current->status, "done") != 0))
						{
							strcpy(current->status, "done");
							current->fdTocheck = temp->fdTocheck;
							//current->rank=temp->rank;
							deleteFromlist("headTempList", temp);
							temp = headTempList;
						}
						else if ((strcmp(current->type, "MPI_Reduce") == 0) && (current->communicator == temp->communicator) && (current->tag == temp->tag) && (strcmp(current->status, "done") != 0))
						{
							strcpy(current->status, "done");
							current->fdTocheck = temp->fdTocheck;
							current->rank = temp->rank;

							deleteFromlist("headTempList", temp);
							temp = headTempList;//prevents the issue of null segfault while doing temp=temp->next in while loop.. this fix is not working as headTempList is also NULL after one item and deletion... will work if there is atleast one element after deletion

						}
					}

				}
			}

			if (temp != NULL)
				temp = temp->next;
		}


		//check from Isend list			
		temp = NULL;
		temp = headSendList;

		while (temp != NULL)
		{
			if (FD_ISSET(temp->fdTocheck, &readfds1))
			{
				//...check the status - if it is connectedSend then do a read for "send" and send the header, 
				//if it is connectedSend then do a read for "send" and send the data
				if (strcmp(temp->status, "connectedSend") == 0)
				{
					//send the header
					sendHeader(temp);

					strcpy(temp->status, "sentHeader");
				}
				else if (strcmp(temp->status, "sentHeader") == 0)
				{
					//send actual data
					sendData(temp);
					strcpy(temp->status, "done");
				}
			}
			temp = temp->next;
		}

		//Check from Ireceive list				
		temp = NULL;
		temp = headRecvList;

		while (temp != NULL)
		{
			if (FD_ISSET(temp->fdTocheck, &readfds1))
			{
				//only one possiblity of receiving the data
				if (strcmp(temp->status, "receivedHeader") == 0)
				{
					//receive the data
					readActualData(temp);

					//status to done
					strcpy(temp->status, "done");
				}
			}

			temp = temp->next;
		}


		//Check for general list... Isend with "receivedHeader"(have to receive the data)				
		temp = NULL;
		temp = headTaskList;

		while (temp != NULL)
		{
			if (FD_ISSET(temp->fdTocheck, &readfds1))
			{
				//only one possiblity of receiving the data
				if (strcmp(temp->status, "receivedHeader") == 0)
				{
					//receive the data
					readActualData(temp);

					//status to done
					strcpy(temp->status, "done");
				}
			}

			temp = temp->next;
		}
	}
}

int BSend(void* data, int count, MPI_Datatype datatype, int destination, int tag, MPI_Comm communicator)
{
	char sockDetails[41];
	char reqSockDetails[1024];
	int n;
	int fdSocketDestination;

	//extract the details of the destinaton rank and connect to destination rank
	fdSocketDestination = fetchRankDetails(destination);//connect_to_server(MPI_rankHostnames[destination],MPI_rankPorts[destination]);

														//send the header or envelope --- rank;tag;size/count;datatype - 24 bytes(23 bytes when MPI_CHAR and 22 bytes when MPI_INT)
	char header[75];  //(+ 20 for MPI_Call_Type)
	int countInBytes = 0;

	if (datatype == MPI_INT)
		countInBytes = count * sizeof(int);
	else if (datatype == MPI_CHAR)
		countInBytes = count;

	//***htonl - in an integer of four bytes maximum of 10 digits, when converted to string 10 bytes - 4*10(number) + 3(:)+ 1(\0) = 44
	//htons is for host to network short(2 bytes), htonl is for host to network long(4 bytes). int - 64 bit (4 bytes ), 32 bit system(2 bytes)

	if (tag == 1020201)
		sprintf(header, "Gsend:%d:%d:%d:%d:%d", htonl(MPI_currentRank), htonl(tag), htonl(countInBytes), htonl(datatype), htonl(communicator)); // Replace count with countInBytes ?? as it might not work for char
	else if ((tag == 1010101) || (tag == 1080801))
		sprintf(header, "Bsend:%d:%d:%d:%d:%d", htonl(MPI_currentRank), htonl(tag), htonl(countInBytes), htonl(datatype), htonl(communicator)); // Replace count with countInBytes ?? as it might not work for char																																		  	

	char reply2[10];//expecting a string "send" which is of size 4 bytes
	n = read(fdSocketDestination, reply2, 5);//Check if the socket is readt for read ??

	if (strcmp(reply2, "send") == 0)
	{
		n = write(fdSocketDestination, header, 74);
	}

	//wait on read for a reply from the destination rank to start sending the data
	char reply[10];//expecting a string "send" which is of size 4 bytes
	n = read(fdSocketDestination, reply, 5);//Check if the socket is readt for read ??

	if (strcmp(reply, "send") == 0)
	{
		//Send the actual data		
		if (count == 0)
		{
			n = write(fdSocketDestination, data, 5);//Check if the socket is ready for write ?? as part of progress engine
		}
		else {
			n = write(fdSocketDestination, data, countInBytes);//Check if the socket is ready for write ?? as part of progress engine
		}
	}

	char reply1[5];
	int bytes_read1;

	if ((tag == 1020201) || (tag == 1080801))
	{
		read(fdSocketDestination, reply1, 5);// Check if the fd is ready to write ??
		if (strcmp(reply1, "done") == 0)
		{
			return MPI_SUCCESS;
		}
	}

	return MPI_SUCCESS;
}

//Set a tag of 1010101 reserved for broadcast
int BRecv(void* data, int count, MPI_Datatype datatype, int source, int tag, MPI_Comm communicator, MPI_Status *status)
{
	int source_socket;
	int bytes_read;
	char dataBuff[1024];

	int receiveFlag = 1;
	int countInBytes = 0;

	if (datatype == MPI_INT)
		countInBytes = count * sizeof(int);
	else if (datatype == MPI_CHAR)
		countInBytes = count;


	//Check the Progress engine linked list (not the progress engine), if the exact header details are present in the list
	//If yes then make the receiveFlag to 0 ?? and do the progress engine with no checking to be done in the current call

	MPI_Request *temp;
	MPI_Request *new;

	int senderRank;
	int senderTag;
	int senderMessageSize = 0;
	int senderComm = 0;
	char MPI_Call_Type[20];

	MPI_Request *currentTaskPe;//to be passed to progressEngine, to keep reference of the current task (here it is receive,blocking), return from PE as soon as this task is done
	currentTaskPe = malloc(sizeof(MPI_Request));

	strcpy(currentTaskPe->type, "Brecv");
	//fdTocheck is accept FD as this is a blocking reading -- will be filled on accept
	currentTaskPe->read = 0;
	currentTaskPe->write = 0;
	currentTaskPe->rank = source;//**imp have to fill the rank while receiving .... of the sender as MPI_ANY_SOURCE, same with tag also MPI_ANY_TAG
	currentTaskPe->tag = tag;
	currentTaskPe->count = countInBytes;
	currentTaskPe->datatype = datatype;
	currentTaskPe->communicator = communicator;
	strcpy(currentTaskPe->status, "notDone");
	currentTaskPe->buffer = data;
	currentTaskPe->next = NULL;

	while (strcmp(currentTaskPe->status, "done") != 0)
	{
		progressEngine(currentTaskPe);
	}

	if (count == 0)//case where done is sent by last rank to root... this makes sense when we make use of actual bytes to read from header, instead of 1024
	{
		bytes_read = read(currentTaskPe->fdTocheck, data, 5);

	}
	else {
		//Next read for the message	
		//Read the actual data from socket
		bytes_read = read(currentTaskPe->fdTocheck, data, 1024);// be carefull while reading size of the buffer ?? change read size to countInBytes ??
	}

	return MPI_SUCCESS;
}

int MPI_Reduce(void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, int root, MPI_Comm comm)
{
	//validate count - less than 0? return error.... =0 is valid at times when you want to send some info in tag
	if (count < 0)
	{
		return MPI_ERR_COUNT;//Invalid count argument
	}

	if (datatype != MPI_CHAR && datatype != MPI_INT)
	{
		return MPI_ERR_TYPE;// Invalid data type
	}


	//Validate Communicator
	if (comm != 0)
	{
		if (MPI_Communicators[comm] == 0)
		{
			return MPI_ERR_COMM;
		}
	}

	if (sendbuf == NULL)//|| recvbuf==NULL)
	{
		return	MPI_ERR_BUFFER;
	}

	int countInBytes = 0;
	if (datatype == MPI_INT)
		countInBytes = count * sizeof(int);
	else if (datatype == MPI_CHAR)
		countInBytes = count;


	int n;
	int senderRank;
	int senderTag;
	int senderMessageSize = 0;
	int senderComm = 0;
	int fd;
	int fds[60];
	char MPI_Call_Type[20];
	int i = 0;
	int offset = 0;
	int step = 1;
	if (datatype == MPI_INT)
	{
		step = sizeof(int);
	}

	int j = 0;

	if (MPI_currentRank == root)
	{
		int *in = (int *)sendbuf;
		int *inout = (int *)recvbuf;

		for (j = 0; j < count; j++)
		{
			*inout = *in;

			in++;
			inout++;
		}

		MPI_Request *temp;
		n = MPI_currentCommSize;
		MPI_Request* currentTaskPe;
		char* dataTosend;
		currentTaskPe = createRequest(NULL, "MPI_Reduce", 0, 0, 0, 0, 1080801, countInBytes, datatype, comm, NULL, dataTosend);
		i = 0;
		n = MPI_currentCommSize;

		while (i < n - 1)
		{
			progressEngine(currentTaskPe);
			if (strcmp(currentTaskPe->status, "done") == 0)
			{
				fds[currentTaskPe->rank] = currentTaskPe->fdTocheck;
				i++;
				currentTaskPe->fdTocheck = 0;
				strcpy(currentTaskPe->status, "notDone");

				//send a mesage to send the actual message
				char reply[5];
				int bytes_read;
				strcpy(reply, "send");//should have the /0 ?

				write(fds[currentTaskPe->rank], reply, 5);// Check if the fd is ready to write ??

														  //receive the data - based on the rank store the data based on the offset caluculated
														  //if dataRecv is the array to store the data receive based on the rank as index and count
				int data[count];
				bytes_read = read(fds[currentTaskPe->rank], &data, countInBytes);

				opFuncPtr[op](&data, recvbuf, &count, &datatype);

			}
		}

		//send reply "done"
		char reply1[5];
		int bytes_read1;
		strcpy(reply1, "done");//should have the /0 ?

		for (i = 0; i < n; i++)
		{
			if (i != root)
			{
				write(fds[i], reply1, 5);// Check if the fd is ready to write ??
			}
		}
	}
	else
	{
		BSend(sendbuf, count, datatype, root, 1080801, comm);
	}

	return MPI_SUCCESS;
}


int MPI_Op_create(void(*MPI_User_function)(), int commute, MPI_Op *op)
{
	int l = 0;
	for (l = 1; l<30; l++)
	{
		if (opFuncPtr[l] == NULL)
		{
			*op = l;
			opFuncPtr[l] = MPI_User_function;
			break;
		}
	}

}


int MPI_Gather(void* sendbuf, int sendcount, MPI_Datatype sendtype, void* recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm)
{

	//validate count - less than 0? return error.... =0 is valid at times when you want to send some info in tag
	if (sendcount < 0 || recvcount < 0)
	{
		return MPI_ERR_COUNT;//Invalid count argument
	}

	if ((sendtype != MPI_CHAR && sendtype != MPI_INT) || (recvtype != MPI_CHAR && recvtype != MPI_INT))
	{
		return MPI_ERR_TYPE;// Invalid data type
	}

	//Validate Communicator
	if (comm != 0)
	{
		if (MPI_Communicators[comm] == 0)
		{
			return MPI_ERR_COMM;
		}
	}

	if (sendbuf == NULL)//|| recvbuf==NULL)
	{
		return	MPI_ERR_BUFFER;
	}

	int countInBytes = 0;
	if (sendtype == MPI_INT)
		countInBytes = recvcount * sizeof(int);
	else if (sendtype == MPI_CHAR)
		countInBytes = recvcount;

	int countInBytes1 = 0;
	if (sendtype == MPI_INT)
		countInBytes1 = recvcount * sizeof(int);
	else if (sendtype == MPI_CHAR)
		countInBytes1 = recvcount;

	int n;
	int senderRank;
	int senderTag;
	int senderMessageSize = 0;
	int senderComm = 0;
	int fd;
	int fds[60];
	char MPI_Call_Type[20];
	int i = 0;
	int offset = 0;
	int step = 1;

	if (recvtype == MPI_INT)
	{
		step = sizeof(int);
	}

	if (MPI_currentRank == root)
	{
		MPI_Request *temp;
		//accept the connection and recv the header first, if it is MPI_Gather - OK or else add it to the list for porgress engine		
		n = MPI_currentCommSize;
		offset = (0 + step*root);
		*((int *)(recvbuf + offset)) = *((int *)sendbuf);

		MPI_Request* currentTaskPe;
		char* dataTosend;
		currentTaskPe = createRequest(NULL, "MPI_Gather", 0, 0, 0, 0, 1020201, countInBytes1, recvtype, comm, NULL, dataTosend);
		i = 0;
		n = MPI_currentCommSize;
		while (i < n - 1)
		{
			progressEngine(currentTaskPe);
			if (strcmp(currentTaskPe->status, "done") == 0)
			{
				fds[currentTaskPe->rank] = currentTaskPe->fdTocheck;
				i++;
				currentTaskPe->fdTocheck = 0;
				strcpy(currentTaskPe->status, "notDone");

				//send a mesage to send the actual message
				char reply[5];
				int bytes_read;
				strcpy(reply, "send");//should have the /0 ?

				write(fds[currentTaskPe->rank], reply, 5);// Check if the fd is ready to write ??

														  //receive the data - based on the rank store the data based on the offset caluculated
														  //if dataRecv is the array to store the data receive based on the rank as index and count
				int data[recvcount];
				bytes_read = read(fds[currentTaskPe->rank], &data, countInBytes1);

				//Find the number of ranks in the communicator and based on that create the receive buffer ?? i think this is already created and passed right
				//Also check for the count if 1 then fine else have
				offset = (0 + step*(currentTaskPe->rank));
				*((int *)(recvbuf + offset)) = data[0];

			}
		}

		//send reply "done"
		char reply1[5];
		int bytes_read1;
		strcpy(reply1, "done");//should have the /0 ?
		for (i = 0; i < n; i++)
		{
			if (i != root)
			{
				write(fds[i], reply1, 5);// Check if the fd is ready to write ??
			}
		}

	}
	else
	{
		BSend(sendbuf, sendcount, sendtype, root, 1020201, comm);
	}

	return MPI_SUCCESS;
}

int MPI_Bcast(void* buffer, int count, MPI_Datatype datatype, int root, MPI_Comm comm)
{
	//validate count - less than 0? return error.... =0 is valid at times when you want to send some info in tag
	if (count<0)
	{
		return MPI_ERR_COUNT;//Invalid count argument
	}

	if (datatype != MPI_CHAR && datatype != MPI_INT)
	{
		return MPI_ERR_TYPE;// Invalid data type
	}


	//Validate Communicator
	if (comm != 0)
	{
		if (MPI_Communicators[comm] == 0)
		{
			return MPI_ERR_COMM;
		}
	}

	//validate root with in the range of the ranks and if the specified rank is in the particular communicator
	//MPI_ERR_ROOT
	if (((root >= MPI_currentCommSize) || root <0))
		return	MPI_ERR_ROOT;

	if (buffer == NULL)
	{
		return	MPI_ERR_BUFFER;
	}

	int prevRank = 0;
	int nextRank = 0;

	//calculate the rank before the current rank
	if (MPI_currentRank == 0)
	{
		prevRank = MPI_currentCommSize - 1;
	}
	else
	{
		prevRank = MPI_currentRank - 1;
	}

	//calculate the next rank
	if (MPI_currentRank == (MPI_currentCommSize - 1))
	{
		nextRank = 0;
	}
	else
	{
		nextRank = MPI_currentRank + 1;
	}

	int bytesToCommunicate = 0;

	if (MPI_currentRank == root)
	{
		char temp[5];
		//send/write
		BSend(buffer, count, datatype, nextRank, 1010101, comm);

		MPI_Status status;
		//recv from prevRank		
		BRecv(temp, 0, datatype, prevRank, 1010101, comm, &status);
		//printf("%s is the temp....\n",temp);
		if (strcmp(temp, "done") == 0)
		{
			return MPI_SUCCESS;
		}

	}
	else
	{
		MPI_Status status;

		//Recv
		BRecv(buffer, count, datatype, prevRank, 1010101, comm, &status);

		//Send		
		if (nextRank == root)
		{

			char temp[5];
			strcpy(temp, "done");
			BSend(temp, 0, datatype, nextRank, 1010101, comm);
		}
		else
		{
			BSend(buffer, count, datatype, nextRank, 1010101, comm);
		}

	}


	return MPI_SUCCESS;
}

int MPI_Isend(void *buf, int count, MPI_Datatype datatype, int destination, int tag, MPI_Comm communicator, MPI_Request *request)
{
	//Validate Communicator
	if (communicator != 0)
	{
		if (MPI_Communicators[communicator] == 0)
		{
			return MPI_ERR_COMM;
		}
	}

	//validate count - less than 0? return error.... =0 is valid at times when you want to send some info in tag
	if (count<0)
	{
		return MPI_ERR_COUNT;//Invalid count argument
	}

	//check if less than zero and not MPI ANY TAG and return error if passes MPI_ERR_TAG
	if (tag<0)// validate if tag is greater some maximum limit ??
	{
		return MPI_ERR_TAG;
	}

	if (datatype != MPI_CHAR && datatype != MPI_INT)
	{
		return MPI_ERR_TYPE;// Invalid data type
	}


	if ((destination >= MPI_currentCommSize) || destination <0)
	{
		return	MPI_ERR_RANK;
	}

	int countInBytes = 0;
	if (datatype == MPI_INT)
		countInBytes = count * sizeof(int);
	else if (datatype == MPI_CHAR)
		countInBytes = count;

	//Create a request and add that to the list
	//Create MPI Request
	MPI_Request *newReq;

	//non blocking stuff, add before the list, others add at end of the list	
	//Add to list??
	newReq = addToList("headSendList", "MPI_Isend", 0, 0, 1, destination, tag, countInBytes, datatype, communicator, "notDone", buf);
	strcpy(request->type, newReq->type);
	request->read = newReq->read;
	request->write = newReq->write;
	request->rank = newReq->rank;
	request->tag = newReq->tag;
	request->count = newReq->count;
	request->datatype = newReq->datatype;
	request->communicator = newReq->communicator;
	strcpy(request->status, newReq->status);
	request->sequence = newReq->sequence;

	return MPI_SUCCESS;
}


int MPI_Irecv(void *buf, int count, MPI_Datatype datatype, int source, int tag, MPI_Comm communicator, MPI_Request *request)
{
	//Validate Communicator
	if (communicator != 0)
	{
		if (MPI_Communicators[communicator] == 0)
		{
			return MPI_ERR_COMM;
		}
	}

	//validate count - less than 0? return error.... =0 is valid at times when you want to send some info in tag
	if (count < 0)
	{
		return MPI_ERR_COUNT;//Invalid count argument
	}

	//Validate Tags	
	//check if less than zero and not MPI ANY TAG and return error if passes MPI_ERR_TAG
	if (tag < 0 && tag != MPI_ANY_TAG)
		return MPI_ERR_TAG;

	if (datatype != MPI_CHAR && datatype != MPI_INT)
	{
		return MPI_ERR_TYPE;// Invalid data type
	}

	if (((source >= MPI_currentCommSize) || source < 0) && source != MPI_ANY_SOURCE)
		return	MPI_ERR_RANK;


	int countInBytes = 0;
	if (datatype == MPI_INT)
		countInBytes = count * sizeof(int);
	else if (datatype == MPI_CHAR)
		countInBytes = count;

	//Create MPI Request
	MPI_Request *newReq;
	newReq = malloc(sizeof(MPI_Request));

	//add this to list
	newReq = addToList("headRecvList", "MPI_Irecv", 0, 1, 0, source, tag, countInBytes, datatype, communicator, "notDone", buf);
	strcpy(request->type, newReq->type);
	request->read = newReq->read;
	request->write = newReq->write;
	request->rank = newReq->rank;
	request->tag = newReq->tag;
	request->count = newReq->count;
	request->datatype = newReq->datatype;
	request->communicator = newReq->communicator;
	strcpy(request->status, newReq->status);
	request->sequence = newReq->sequence;

	return MPI_SUCCESS;
}


int MPI_Test(MPI_Request *request, int *flag, MPI_Status *status)
{
	MPI_Request *request1 = NULL;

	//get the status	
	if (strcmp(request->type, "MPI_Isend") == 0)
	{
		request1 = searchInListNonBlocking("headSendList", request);

		if (request1 == NULL)
		{
			request1 = searchInListNonBlocking("headDoneList", request);
		}
	}
	else
		request1 = searchInListNonBlocking("headRecvList", request);

	progressEngine(NULL);

	if (strcmp(request1->status, "done") == 0)
	{
		*flag = 1;
	}
	else {
		*flag = 0;
	}

	return MPI_SUCCESS;
}

int MPI_Wait(MPI_Request *request, MPI_Status *status)
{
	if ((request == NULL) || (status == NULL))
	{
		return MPI_ERR_REQUEST;
	}

	MPI_Request *request1 = NULL;
	//get the status

	if (strcmp(request->type, "MPI_Isend") == 0)
	{
		request1 = searchInListNonBlocking("headSendList", request);

		if (request1 == NULL)
		{
			request1 = searchInListNonBlocking("headDoneList", request);
		}
	}
	else
	{
		request1 = searchInListNonBlocking("headRecvList", request);
	}

	while (strcmp(request1->status, "done") != 0)
	{
		progressEngine(NULL);
	}

	if (strcmp(request->type, "MPI_Irecv") == 0)
	{
		if (request->rank != MPI_ANY_SOURCE)
			status->MPI_SOURCE = request1->rank;
		else
			status->MPI_SOURCE = request1->statusSource;

		if (request->tag != MPI_ANY_TAG)
		{
			status->MPI_TAG = request1->tag;
		}
		else
		{
			status->MPI_TAG = request1->statusTag;

		}
	}

	return MPI_SUCCESS;
}

int MPI_Recv(void* data, int count, MPI_Datatype datatype, int source, int tag, MPI_Comm communicator, MPI_Status *status)
{
	int source_socket;
	int bytes_read;
	char dataBuff[1024];
	waitAccepting = 1;//halt the thread for sometime from doing the accepts

					  //Validate Tags	
					  //check if less than zero and not MPI ANY TAG and return error if passes MPI_ERR_TAG
	if (tag < 0 && tag != MPI_ANY_TAG)
	{
		waitAccepting = 0;
		return MPI_ERR_TAG;
	}

	//validate count - less than 0? return error.... =0 is valid at times when you want to send some info in tag
	if (count < 0)
	{
		waitAccepting = 0;
		return MPI_ERR_COUNT;//Invalid count argument
	}

	if (datatype != MPI_CHAR && datatype != MPI_INT)
	{
		waitAccepting = 0;
		return MPI_ERR_TYPE;// Invalid data type
	}

	//validate rank with in the range of the ranks and if the specified rank is in the particular communicator
	//MPI_ERR_RANK
	if (((source >= MPI_currentCommSize) || source < 0) && source != MPI_ANY_SOURCE)
	{
		waitAccepting = 0;
		return	MPI_ERR_RANK;
	}

	//Validate Communicator
	if (communicator != 0)
	{
		if (MPI_Communicators[communicator] == 0)
		{
			waitAccepting = 0;
			return MPI_ERR_COMM;
		}
	}

	int receiveFlag = 1;
	int countInBytes = 0;

	if (datatype == MPI_INT)
		countInBytes = count * sizeof(int);
	else if (datatype == MPI_CHAR)
		countInBytes = count;

	MPI_Request *temp;
	MPI_Request *new;
	MPI_Request *currentTaskPe;//to be passed to progressEngine, to keep reference of the current task (here it is receive,blocking), return from PE as soon as this task is done
	currentTaskPe = malloc(sizeof(MPI_Request));
	strcpy(currentTaskPe->type, "MPI_Recv");
	//fdTocheck is accept FD as this is a blocking reading -- will be filled on accept
	currentTaskPe->read = 1;
	currentTaskPe->write = 0;
	currentTaskPe->rank = source;//**imp have to fill the rank while receiving .... of the sender as MPI_ANY_SOURCE, same with tag also MPI_ANY_TAG
	currentTaskPe->tag = tag;
	currentTaskPe->count = countInBytes;
	currentTaskPe->datatype = datatype;
	currentTaskPe->communicator = communicator;
	strcpy(currentTaskPe->status, "notDone");
	currentTaskPe->buffer = data;
	currentTaskPe->next = NULL;

	addToEndListReq("headRecvList", currentTaskPe);

	while (strcmp(currentTaskPe->status, "done") != 0)
	{

		progressEngine(NULL);

	}

	if (source != MPI_ANY_SOURCE)
		status->MPI_SOURCE = currentTaskPe->rank;
	else
		status->MPI_SOURCE = currentTaskPe->statusSource;

	if (tag != MPI_ANY_TAG)
	{
		status->MPI_TAG = currentTaskPe->tag;
	}
	else
	{
		status->MPI_TAG = currentTaskPe->statusTag;

	}

	status->count = count;
	return MPI_SUCCESS;

}

int MPI_Send(void* data, int count, MPI_Datatype datatype, int destination, int tag, MPI_Comm communicator)
{
	char sockDetails[41];
	char reqSockDetails[1024];
	int n;
	int fdSocketDestination;

	//check if less than zero and not MPI ANY TAG and return error if passes MPI_ERR_TAG
	if (tag < 0)// validate if tag is greater some maximum limit ??
	{
		return MPI_ERR_TAG;
	}

	//validate count - less than 0? return error
	if (count <= 0)
	{
		return MPI_ERR_COUNT;//Invalid count argument
	}

	if (datatype != MPI_CHAR && datatype != MPI_INT)
	{
		return MPI_ERR_TYPE;// Invalid data type
	}

	//validate rank with in the range of the ranks and if the specified rank is in the particular communicator
	//MPI_ERR_RANK
	if ((destination >= MPI_currentCommSize) || destination < 0)
	{
		//printf("in destination %d\n",destination);
		return	MPI_ERR_RANK;
	}


	//Validate Communicator
	if (communicator != 0)
	{
		if (MPI_Communicators[communicator] == 0)
		{
			return MPI_ERR_COMM;
		}
	}

	int countInBytes = 0;
	if (datatype == MPI_INT)
		countInBytes = count * sizeof(int);
	else if (datatype == MPI_CHAR)
		countInBytes = count;

	MPI_Request* currentTaskPe;

	currentTaskPe = createRequest("headSendList", "MPI_Send", 0, 0, 0, destination, tag, countInBytes, datatype, communicator, NULL, data);
	while (strcmp(currentTaskPe->status, "done") != 0)
	{
		progressEngine(NULL);
	}
	return MPI_SUCCESS;
}


int MPI_Ssend(void* data, int count, MPI_Datatype datatype, int destination, int tag, MPI_Comm communicator)
{
	char sockDetails[41];
	char reqSockDetails[1024];
	int n;
	int fdSocketDestination;

	//Validate Tags	

	//check if less than zero and not MPI ANY TAG and return error if passes MPI_ERR_TAG
	if (tag < 0)// validate if tag is greater some maximum limit ??
	{
		return MPI_ERR_TAG;
	}

	//validate count - less than 0? return error
	if (count <= 0)
	{
		return MPI_ERR_COUNT;//Invalid count argument
	}

	if (datatype != MPI_CHAR && datatype != MPI_INT)
	{
		return MPI_ERR_TYPE;// Invalid data type
	}

	//validate rank with in the range of the ranks and if the specified rank is in the particular communicator
	//MPI_ERR_RANK
	if ((destination >= MPI_currentCommSize) || destination < 0)
	{
		return	MPI_ERR_RANK;
	}

	//Validate Communicator
	if (communicator != 0)
	{
		if (MPI_Communicators[communicator] == 0)
		{
			return MPI_ERR_COMM;
		}
	}

	int countInBytes = 0;
	if (datatype == MPI_INT)
		countInBytes = count * sizeof(int);
	else if (datatype == MPI_CHAR)
		countInBytes = count;

	MPI_Request* currentTaskPe;

	currentTaskPe = createRequest(NULL, "MPI_Ssend", 0, 0, 0, destination, tag, countInBytes, datatype, communicator, NULL, data);
	while (strcmp(currentTaskPe->status, "done") != 0)
	{

		progressEngine(currentTaskPe);

	}

	return MPI_SUCCESS;
}

