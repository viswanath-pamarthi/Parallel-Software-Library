/* sample header file for 6430 file sp17 - may require modifying depending on details */

typedef int MPI_Datatype;
#define MPI_CHAR           ((MPI_Datatype)0x4c000101)
#define MPI_INT            ((MPI_Datatype)0x4c000405)


#define MPI_PROC_NULL   (-1)
#define MPI_ANY_SOURCE 	(-2)
#define MPI_ROOT        (-3)
#define MPI_ANY_TAG     (-1)

/* Communicators */
typedef int MPI_Comm;
#define MPI_COMM_WORLD ((MPI_Comm)0000000000)
typedef void (MPI_User_function) ( void *, void *, int *, MPI_Datatype * ); 
/* MPI request opjects */
//typedef int MPI_Request;

typedef int MPI_Aint;

/* Collective operations */
typedef int MPI_Op;
#define MPI_SUM     (MPI_Op)(0000000000)
void (*opFuncPtr[60])();
//MPI_User_function
/*
#define MPI_MAX     (MPI_Op)(0x58000001)
#define MPI_MIN     (MPI_Op)(0x58000002)
#define MPI_SUM     (MPI_Op)(0x58000003)
#define MPI_PROD    (MPI_Op)(0x58000004)
#define MPI_LAND    (MPI_Op)(0x58000005)
#define MPI_BAND    (MPI_Op)(0x58000006)
#define MPI_LOR     (MPI_Op)(0x58000007)
#define MPI_BOR     (MPI_Op)(0x58000008)
#define MPI_LXOR    (MPI_Op)(0x58000009)
#define MPI_BXOR    (MPI_Op)(0x5800000a)
#define MPI_MINLOC  (MPI_Op)(0x5800000b)
#define MPI_MAXLOC  (MPI_Op)(0x5800000c)
#define MPI_REPLACE (MPI_Op)(0x5800000d)
*/
typedef struct MPI_Status {
    int count;
    int cancelled;
    int MPI_SOURCE;
    int MPI_TAG;
    int MPI_ERROR;
} MPI_Status;

//To do a linked list and loop through this list in Progress Engine
//In receive first check by looping through this list if the required tag etc. of the header matches with the requirement
typedef struct MPI_Request{
	char type[50];//MPI_Recv or MPI_Send or MPI_Irecv etc
	int fdTocheck;// read or write, how to differentiate for select ??
	int read;//set to 1 read (some kind of receive or any other command that is trying to receive)
	int write;//set to 1 write (some kind of send or sending out data )
	int rank;//Part of header
	int tag;//Part of header
	int count;//Part Of header
	MPI_Datatype datatype;//Part Of header
	MPI_Comm communicator;//Part Of header - P3
	char status[50];// done,notDone.... useful when a non blocking operation is done to check the status-- 
						//Isend you know that it is isend, irecv, you don't know till the header has arrived and matched the irecv request we have(can be Isend(store this in general list on receiving side) or a send or a Bsend)
					/*new status for Isend ... connectedSend(means have to read for "send" and if true send the header - change status to "sentHeader"
											   sentHeader - means sent the header and waiting for "send" if to send the data and status to "done" and move to doneList
								for Irecv ... connected(everything in temp list initially has connected status as soon as accepting, till header has arrived), connectedRecv - means connected and sent "send" . change status to "receiveheader"(for connected - chance of being Non Blocking Irecv or blocking if in blocking receive as not yet clear on the header stuff if in case)
											  receivedHeader - read for the header.. and do write "send" (add the fd to select and check these status and do appropriate function calls/action
											  
						*/
	void* buffer;//To store the data for the non blocking calls ?? double check on Isend when to return or add the fd for the status of isend?
	struct MPI_Request *next;
	
	int statusTag;//to fill the status i the irecv or recv has Any source and Anytag, then have to get the actual source and tag we go the data from 
	int statusSource;
	int sequence;
}MPI_Request;


/* RMA and Windows */
typedef int MPI_Win;
#define MPI_WIN_NULL ((MPI_Win)0x20000000)

/* for info */
typedef int MPI_Info;
#define MPI_INFO_NULL         ((MPI_Info)0x1c000000)
#define MPI_MAX_INFO_KEY       255
#define MPI_MAX_INFO_VAL      1024

/* MPI's error classes */
#define MPI_SUCCESS          0      /* Successful return code */
/* Communication argument parameters */
#define MPI_ERR_BUFFER       1      /* Invalid buffer pointer */
#define MPI_ERR_COUNT        2      /* Invalid count argument */
#define MPI_ERR_TYPE         3      /* Invalid datatype argument */
#define MPI_ERR_TAG          4      /* Invalid tag argument */
#define MPI_ERR_COMM         5      /* Invalid communicator */
#define MPI_ERR_RANK         6      /* Invalid rank */
#define MPI_ERR_ROOT         7      /* Invalid root */
#define MPI_ERR_TRUNCATE    14      /* Message truncated on receive */
#define MPI_ERR_REQUEST		55		/*Invalid MPI Request argument*/

void MPIsum(void *inp, void *inoutp, int *len, MPI_Datatype *dptr);

/* We require that the C compiler support prototypes */
/* Begin Prototypes */
int MPI_Init(int *, char ***);
int MPI_Finalize(void);
int MPI_Abort(MPI_Comm, int);
int MPI_Attr_put(MPI_Comm, int, void*);
int MPI_Attr_get(MPI_Comm, int, void *, int *);
double MPI_Wtime(void);
double MPI_Wtick(void);
int MPI_Gather(void* , int, MPI_Datatype, void*, int, MPI_Datatype, int, MPI_Comm); 
int MPI_Barrier(MPI_Comm );
int MPI_Bcast(void*, int, MPI_Datatype, int, MPI_Comm );
int MPI_Comm_dup(MPI_Comm, MPI_Comm *);
int MPI_Comm_size(MPI_Comm, int *);
int MPI_Comm_rank(MPI_Comm, int *);
int MPI_Isend(void*, int, MPI_Datatype, int, int, MPI_Comm, MPI_Request *);
int MPI_Irecv(void*, int, MPI_Datatype, int, int, MPI_Comm, MPI_Request *);
int MPI_Probe(int, int, MPI_Comm, MPI_Status *);
int MPI_Recv(void*, int, MPI_Datatype, int, int, MPI_Comm, MPI_Status *);
int MPI_Reduce(void* , void*, int, MPI_Datatype, MPI_Op, int, MPI_Comm);
int MPI_Rsend(void*, int, MPI_Datatype, int, int, MPI_Comm);
int MPI_Send(void*, int, MPI_Datatype, int, int, MPI_Comm);
int MPI_Ssend(void*, int, MPI_Datatype, int, int, MPI_Comm);
int MPI_Test(MPI_Request *, int *, MPI_Status *);
int MPI_Wait(MPI_Request *, MPI_Status *);
int MPI_Get(void *, int, MPI_Datatype, int, MPI_Aint, int, MPI_Datatype, MPI_Win);
int MPI_Put(void *, int, MPI_Datatype, int, MPI_Aint, int, MPI_Datatype, MPI_Win);
int MPI_Win_create(void *, MPI_Aint, int, MPI_Info, MPI_Comm, MPI_Win *);
int MPI_Win_lock(int, int, int, MPI_Win);
int MPI_Win_unlock(int, MPI_Win);
//int MPI_Op_create(MPI_User_function *user_fn, int commute, MPI_Op *op);