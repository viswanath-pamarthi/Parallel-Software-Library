#include<unistd.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include <errno.h> 
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>

#define MaxRank 60
#define NON_RESERVED_PORT    5305
#define BUFFER_LEN           1024    
#define SERVER_ACK           "ACK_FROM_SERVER"

char rankHostnames[MaxRank][MaxRank];//Store the rank index corresponding - hostname and port number
int rankPorts[MaxRank];
int acceptSocketFd[MaxRank];
int connectedRanks[MaxRank];// TO be used in the ranks  ?? remove
int totalRanks = 0;
int acceptSocketFd_Ppexec;
int acceptPort;
int	requiredSocketdetails[MaxRank];
int	toSocketdetails[MaxRank];

void* setupMpiInit(void* argument)
{
	pthread_exit(NULL);
}

//Method to execute the program on a node in the Cluster by SSH
int executeProgram(int n, char* hostfileName, int nflag, int fflag, char* programName)
{
	FILE* hosts;//Handle for the file containing hosts
	char temp[25];//to hold the hostnames from file 
	int rc = 0;//for fork return value	
	int i = 0;
	char hostname[50];
	gethostname(hostname, 50);


	if (fflag == 1)
	{
		//read from specified file			
		hosts = fopen(hostfileName, "r");
	}
	else
	{
		//read from the default file		
		hosts = fopen("hostnames", "r");
	}

	if (hosts == NULL)
	{

		//Run all ranks on the same hosts
		while (i < n)
		{
			rc = 0;
			rc = fork();
			if (rc == 0)
			{

				char** args1 = malloc(20 * sizeof(char *));
				char* param = malloc(250 * sizeof(char));

				args1[0] = strdup(programName);

				char *rank = malloc(50 * sizeof(char));
				char *size = malloc(50 * sizeof(char));
				char *port = malloc(50 * sizeof(char));
				sprintf(rank, "%d", i);
				sprintf(size, "%d", n);
				sprintf(port, "%s:%d", hostname, acceptPort);
				setenv("PP_MPI_RANK", rank, 1);
				setenv("PP_MPI_SIZE", size, 1);
				setenv("PP_MPI_HOST_PORT", port, 1);

				sprintf(param, " PP_MPI_RANK=%d PP_MPI_SIZE=%d PP_MPI_HOST_PORT=%s:%d %s", i, n, hostname, acceptPort, programName);

				extern char** environ;
				char *p;
				char *temp = strdup(programName);
				int j = 0;
				p = strtok(temp, " ");

				args1[j] = p;
				j++;

				while (p != NULL)
				{
					p = strtok(NULL, " ");
					args1[j] = p;
					j++;
				}

				execvpe(args1[0], args1, environ);
				perror("execve");

			}
			i++;

		}
	}
	else
	{
		//printf("in host=NOt NULL, %d is n , %d is i\n",n,i);
		//	printf("pid=%d of parent \n",getpid());
		//Run on the hosts 
		while (i < n)
		{
			if (fscanf(hosts, "%s", temp) == EOF)
			{
				fseek(hosts, 0, SEEK_SET);
				continue;
			}

			rc = 0;
			//Fork and Exec in the child process for ssh and parent process waits for the process
			rc = fork();

			//Child process
			if (rc == 0)
			{

				int returnCode = 0;
				char param[1500];
				char* args1[9];
				char cwd[400];
				getcwd(cwd, 400);

				args1[0] = strdup("ssh");
				args1[1] = strdup(temp);

				sprintf(param, "bash -c ' MPI_CWD=%s&&cd $MPI_CWD;PP_MPI_RANK=%d PP_MPI_SIZE=%d PP_MPI_HOST_PORT=%s:%d %s'", cwd, i, n, hostname, acceptPort, programName);

				args1[3] = NULL;
				args1[2] = strdup(param);
				extern char** environ;

				execvpe("ssh", args1, environ);
				perror("execvp");
			}

			i++;//Increment the rank

		}

	}



	//Loop through the program for MPI_Initialize..after receiving from all the ranks then send the whole ranks hostname and port to all the ranks
	int k = 0;
	char finalizeDone[5] = "done";
	fd_set readfds;//fds to set for reading in select
	int totalFinalize = 0;
	int j = 0, returnCount;
	char buf1[BUFFER_LEN];
	struct timeval tv;
	FD_ZERO(&readfds);
	//FD_SET( acceptSocketFd_Ppexec, &readfds );
	int client_socket;
	int bytes_read;
	char buf[BUFFER_LEN];
	int status;
	int value;

	//do{
	//while (waitpid(-1, &status, WNOHANG))

	while (k < totalRanks)
	{
		//Do the MPI thing here .. don't have a loop terminating on the count of finalize and just bank on the wait pid coid tp terminate
		//acceptSocketFd_Ppexec
		//do select on the FDS.. i.e. the progress engine
		FD_ZERO(&readfds);
		FD_SET(acceptSocketFd_Ppexec, &readfds);
		for (j = 0; j < totalRanks; j++)
		{

			if (acceptSocketFd[j] > 0)
			{//printf("In loop %d fd\n",acceptSocketFd[j]);

				FD_SET(acceptSocketFd[j], &readfds);
			}
		}

		tv.tv_sec = 2;
		tv.tv_usec = 0;

		returnCount = select(FD_SETSIZE, &readfds, NULL, NULL, &tv);

		value = waitpid(-1, &status, WNOHANG);

		//child process terminated normally
		if (WIFEXITED(status))
		{
			k++;
			status = 121212122;
		}
		/*if (WIFSIGNALED(status))
		{
		// It was terminated by a signal
		if (WTERMSIG(status) == SIGSEGV)
		{
		printf("terminadt...with sig fault++++\n");
		// It was terminated by a segfault
		}
		}*/

		int rankTemp1 = 0;
		char detailsSock1[40];
		for (rankTemp1 = 0; rankTemp1 < totalRanks; rankTemp1++)
		{
			//check if any socket details are requirred by any rank						
			if (requiredSocketdetails[rankTemp1] >= 0)
			{

				if ((strcmp(rankHostnames[requiredSocketdetails[rankTemp1]], "\0") != 0) && (rankPorts[requiredSocketdetails[rankTemp1]] > 1024))
				{
					sprintf(detailsSock1, "%s:%d", rankHostnames[requiredSocketdetails[rankTemp1]], rankPorts[requiredSocketdetails[rankTemp1]]);
					write(acceptSocketFd[toSocketdetails[rankTemp1]], detailsSock1, 40);

					requiredSocketdetails[rankTemp1] = -1;
					toSocketdetails[rankTemp1] = -1;
				}

			}

		}

		if (returnCount == 0)
		{
			continue;
		}

		if (returnCount == -1 && errno == EINTR)
		{
			continue;
		}


		if (returnCount < 0)
		{
			perror("select failed");
			break;
		}


		//Do the accept thing
		if (FD_ISSET(acceptSocketFd_Ppexec, &readfds))
		{
			client_socket = accept_connection(acceptSocketFd_Ppexec);//accept every connection from a rank		
			bytes_read = read(client_socket, buf, BUFFER_LEN);// be carefull while reading size of the buffer
			error_check(bytes_read, "recv_msg read");

			//do strtok and separate the date string from rank:hostname:port to rankHostnames,rankPorts arays based on rank		
			int rank;
			char* token;

			token = strtok(buf, ":");
			rank = atoi(token);
			token = strtok(NULL, ":");
			stpcpy(rankHostnames[rank], token);
			token = strtok(NULL, ":");
			rankPorts[rank] = atoi(token);
			acceptSocketFd[rank] = client_socket;

		}

		int n;
		for (j = 0; j < totalRanks; j++)
		{
			if (FD_ISSET(acceptSocketFd[j], &readfds))
			{
				n = read(acceptSocketFd[j], buf1, BUFFER_LEN);
				if (n == -1)
				{
					perror("read failed\n");
					continue;
				}
				else if (n == 0)// Zero is returned by read when EOF is read... socket other end is closed?
				{
					acceptSocketFd[j] = 0;
				}
				else
				{
					char* tempToken = strtok(buf1, ":");
					//remove switch and have if and if else statements, as switch on string doesnot work in c

					if (strcmp(tempToken, "SockDetails") == 0)
					{
						//Format of this kind of request is Sockdetails:rank
						tempToken = strtok(NULL, ":");

						int temp = atoi(tempToken);//socket details of the rank						
						char detailsSock[40];

						//check heere if there are details of rank zero available, if not leave the work for progress engine
						//if the socket details are not available then remember and each and every time looping through
						if (rankPorts[temp] == 0)
						{
							int rankTemp = 0;
							for (rankTemp = 0; rankTemp < totalRanks; rankTemp++)
							{
								//check for vacant spot
								if (requiredSocketdetails[rankTemp] < 0)
								{
									requiredSocketdetails[rankTemp] = temp;
									toSocketdetails[rankTemp] = j;
									break;
								}

							}
						}
						else
						{
							sprintf(detailsSock, "%s:%d", rankHostnames[temp], rankPorts[temp]);
							write(acceptSocketFd[j], detailsSock, 40);
						}

					}
					else if (strcmp(tempToken, "Finalize") == 0)
					{
						totalFinalize++;

						if (totalFinalize == totalRanks)
						{
							int ranks = 0;
							for (ranks = 0; ranks < totalRanks; ranks++)
							{
								write(acceptSocketFd[ranks], finalizeDone, 5);
							}
						}
					}

				}

			}
		}

	}//while(k<totalRanks);//totalFinalize<totalRanks);//k<totalRanks);
}

void* setupMpiIni(void* argument)
{
	int i, client_socket;
	int bytes_read;
	char buf[BUFFER_LEN];

	for (i = 0; i < totalRanks; i++)
	{
		client_socket = accept_connection(acceptSocketFd_Ppexec);//accept every connection from a rank     
		bytes_read = read(client_socket, buf, BUFFER_LEN);// be carefull while reading size of the buffer
		error_check(bytes_read, "recv_msg read");

		//do strtok and separate the date string from rank:hostname:port to rankHostnames,rankPorts arays based on rank		
		int rank;
		char* token;

		token = strtok(buf, ":");
		rank = atoi(token);
		token = strtok(NULL, ":");
		stpcpy(rankHostnames[rank], token);
		token = strtok(NULL, ":");
		rankPorts[rank] = atoi(token);
		acceptSocketFd[rank] = client_socket;

	}


	/*Do the rest of reading from child process i.e the ranks here for all types of requests
	**receiving data is in the format -> TypeOfRequest : followed by the data
	** fix the number of bytes for the TypeOfRequest request, so that can read the specific bytes and do a switch on TypeOfRequest
	** SO, How do we loop over the avaialable fd's ... select ?
	*/
	int totalFinalize = 0;
	int j = 0, rc;
	char buf1[BUFFER_LEN];
	fd_set readfds;
	struct timeval tv;
	FD_ZERO(&readfds);

	while (totalFinalize < totalRanks)//or do a barrier till in starting of the finalize
	{
		//do select on the FDS.. i.e. the progress engine
		FD_ZERO(&readfds);
		for (j = 0; j < totalRanks; j++)
		{

			if (acceptSocketFd[j] > 0)
			{

				FD_SET(acceptSocketFd[j], &readfds);
			}
		}

		tv.tv_sec = 5;
		tv.tv_usec = 0;

		rc = select(FD_SETSIZE, &readfds, NULL, NULL, &tv);

		if (rc == 0)
		{
			continue;
		}

		if (rc == -1 && errno == EINTR)
		{
			continue;
		}


		if (rc < 0)
		{
			perror("select failed");
			break;
		}

		int n;
		for (j = 0; j < totalRanks; j++)
		{
			if (FD_ISSET(acceptSocketFd[j], &readfds))
			{
				n = read(acceptSocketFd[j], buf1, BUFFER_LEN);
				if (n == -1)
				{
					perror("read failed\n");
					continue;
				}
				else if (n == 0)// Zero is returned by read when EOF is read... socket other end is closed?
				{
					acceptSocketFd[j] = 0;
				}
				else
				{
					char* tempToken = strtok(buf1, ":"); // ?? Check on resetting buuf1 


					if (strcmp(tempToken, "SockDetails") == 0)
					{
						//Format of this kind of request is Sockdetails:rank
						tempToken = strtok(NULL, ":");

						int temp = atoi(tempToken);//socket details of the rank						
						char detailsSock[40];
						sprintf(detailsSock, "%s:%d", rankHostnames[temp], rankPorts[temp]);
						write(acceptSocketFd[j], detailsSock, 40);
					}
					else if (strcmp(tempToken, "Finalize") == 0)
					{
						totalFinalize++;
					}

				}

			}
		}
	}

	pthread_exit(NULL);
}

char* joinStrings(char** argv, int startIndex, const int argc)
{
	char* result = malloc(600 * sizeof(char));
	int i;
	for (i = startIndex; i < argc; i++)
	{
		strcat(result, argv[i]);
		if (i < argc - 1)
		{
			strcat(result, " ");
		}
		else {
			strcat(result, "\0");
		}
	}

	return result;
}

void InitializeAcceptSockFd()
{
	int j = 0;

	for (j = 0; j<totalRanks; j++)
	{
		acceptSocketFd[j] = 0;
		requiredSocketdetails[j] = -1;
		toSocketdetails[j] = -1;
		strcpy(rankHostnames[j], "\0");
		rankPorts[j] = 0;
	}
}

int main(int argc, char* argv[])
{
	//setbuf(stdout,NULL);
	int nflag = 0;//to chech if argument n is passed
	int fflag = 0;//to check if argument f is passed
	int character = 0;//to hold the parsed character
	int commParams = 0;//to validate a maximum command line parameters count, max here is 2 one for -n and another for -f
	int nArgIndex = 0;//to help indicate the index of the -n value in the argv array
	int fArgIndex = 0;//to help indicate the index of the -f value in the argv array

					  //MPI_Finalize();//To test MPI lib linking

	int totalArgvTemp;//to hold the temporary index value of all arguments
	int commArgvTemp;//to hold the temporary index value of  the command line parameter with "-"

	while ((character = getopt(argc, argv, "nf")) != -1)
	{

		switch (character)
		{
		case 'n':
			if (nflag == 0)
			{
				nflag = 1;
				commArgvTemp++;

				nArgIndex = fflag + 1;
			}
			else
			{
				printf("More than one -n argument provided or invalid argument\n");
				return 1;//More than one -n argument provided or invalid argument
			}
			break;

		case 'f':
			if (fflag == 0)
			{
				fflag = 1;
				fArgIndex = nflag + 1;
				commArgvTemp++;
			}
			else
			{
				printf("More than one -f argument provided or invalid argument\n");
				return 1;//More than one -f argument provided or invalid argument
			}

			break;

		case '?':
			printf("only command line parameters '-n' and '-f' are supported\n");
			return 1;
			break;

		default:
			printf("In default\n");
			break;

		}

		totalArgvTemp++;
	}


	// TODO: Validation - every - parameter has its value(professor would provide values for sure) and -n value is integer
	int i = 0;

	pthread_t id[3];//Three threads for Initialize	

					//Call a method to do the rest of work

					//Set up the accept socket
	acceptSocketFd_Ppexec = setup_to_accept();
	struct sockaddr_in serv_addr;
	socklen_t len = sizeof(serv_addr);


	if (getsockname(acceptSocketFd_Ppexec, (struct sockaddr *)&serv_addr, &len) == -1)
	{
		perror("getsockname");
		return;
	}

	acceptPort = ntohs(serv_addr.sin_port);

	if (nflag == 1 && fflag == 1)
	{

		//TODO: Validate argc count of 6	
		//TODO: Validate n is a number
		totalRanks = atoi(argv[nArgIndex + 2]);
		InitializeAcceptSockFd();
		//pthread_create(&id[0],NULL,setupMpiInit,NULL);
		executeProgram(atoi(argv[nArgIndex + 2]), argv[fArgIndex + 2], 1, 1, joinStrings(argv, 5, argc));


	}
	else if (nflag == 1)
	{
		totalRanks = atoi(argv[nArgIndex + 1]);
		InitializeAcceptSockFd();
		executeProgram(totalRanks, "\0", 1, 0, joinStrings(argv, 3, argc));
	}
	else if (fflag == 1)
	{
		//Value of n is defaulted to 1
		totalRanks = 1;
		InitializeAcceptSockFd();
		//pthread_create(&id[0],NULL,setupMpiInit,NULL);
		executeProgram(1, argv[fArgIndex + 1], 0, 1, joinStrings(argv, 3, argc));
	}
	else
	{

		//Value of n is defaulted to 1
		totalRanks = 1;
		InitializeAcceptSockFd();
		//pthread_create(&id[0],NULL,setupMpiInit,NULL);
		executeProgram(1, "\0", 0, 0, joinStrings(argv, 1, argc));
	}

	return 0;
}




