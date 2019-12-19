/* Fork server init timeout multiplier: we'll wait the user-selected timeout plus this much for the fork server to spin up. */ 
#define FORK_WAIT_MULT      10
/* Environment variable used to pass SHM ID to the called program. */
#define SHM_ENV_VAR "__AFL_SHM_ID"
/* Local port to communicate with python module. */
#define PORT                12012
/* Maximum line length passed from GCC to 'as' and used for parsing configuration files. */
#define MAX_LINE            8192
/* Designated file descriptors for forkserver commands (the application will use FORKSRV_FD and FORKSRV_FD + 1). */
#define FORKSRV_FD          198
/* Distinctive bitmap signature used to indicate failed execution. */
#define EXEC_FAIL_SIG       0xfee1dead
/* Smoothing divisor for CPU load and exec speed stats (1 - no smoothing). */
#define AVG_SMOOTHING       16

#define MAP_SIZE            (2<<18)