/*
This program implements the lwwire protocol. It expects STDIN and STDOUT
to be connected to an appropriately configured communication channel.

The following timeouts are specified by the protocol and are listed here
for convenience:

Between bytes in a request: 10 milliseconds
Between bytes reading a response (client): 10ms <= timeout <= 1000ms

Server receiving bad request: >= 1100 milliseconds

Implementation notes:

This implementation uses low level I/O calls (read()/write()) on STDIN
and STDOUT because we MUST have fully unbuffered I/O for the protocol
to function properly and we really do not want the stdio overhead for
that.

The complexity of the lwwire_readdata() and lwwire_writedata() functions
is required to handle some possible corner cases that would otherwise
completely bollix everything up.

Command line options:

drive=N,PATH

Specify that drive #N should reference the file at PATH. Note that the
file at PATH will be created if it doesn't exist, but only if it is actually
accessed. N is a decimal number from 0 to 255. If N is prefixed with "C",
the drive is treated as read-only.


By default, no drives are associated with files. Also, it is unspecified
what happens if multiple protocol instances access the same drive.

*/

// for nanosleep
#define _POSIX_C_SOURCE 199309L

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include "netdev.h"

#define LWERR_NONE 0
#define LWERR_CHECKSUM 0xF3
#define LWERR_READ 0xF4
#define LWERR_WRITE 0xF5
#define LWERR_NOTREADY 0xF6

struct lwwire_driveinfo
{
	char *path;
	FILE *fp;
	int isconst;
};

struct lwwire_extension_data
{
	int enabled;
	int (*handler)(int op);	// handle op, return -1 if not supported
	int (*disable)(void);	// disable extension negotiated
	int (*enable)(void);	// enable extension negotiated (return -1 if failed to init)
	void (*reset)(void);	// on server reset opcode; extension will be disabled automatically here
};

struct lwwire_extension_data lwwire_extension_list[256];

struct lwwire_driveinfo drivedata[256];

void lwwire_protoerror(void);
int lwwire_readdata(void *, int, int);
int lwwire_writedata(void *, int);
void lwwire_write(void *, int);
int lwwire_read(void *, int);
int lwwire_read2(void *, int, int);
void lwwire_reset(void);
int lwwire_fetch_sector(int dn, int lsn, void *);
int lwwire_save_sector(int dn, int lsn, void *);
int nonblock(int);
int lwwire_drive_readononly(int dn);
int lwwire_register_extension(int, int (*handler)(int), int (*enable)(void), int (*disable)(void), void (*reset)(void));
void lwwire_proto_read(void);
void lwwire_proto_write(void);
void lwwire_proto_readex(void);
void lwwire_proto_requestextension(void);
void lwwire_proto_disableextension(void);
void lwwire_proto_extensionop(void);

int lwwire_read(void *v, int i){
    return 0;
}

int lwwire_read2(void *v, int i, int j){
    return 0;
}

int main(int argc, char **argv)
{
	unsigned char *buf;
	time_t curtime;
	struct tm *tmval;
	int rv;
	int i;
		

	memset(&drivedata, 0, sizeof(drivedata));
	memset(lwwire_extension_list, 0, sizeof(struct lwwire_extension_data) * 256);

	// call lwwire_register_extension() for each extension you define
	// statically. Add those calls here, or calls to the equivalent
	// of the "lwwire_register" function from a DSO type extension.
	
	for (i = 1; i < argc; i++)
	{
		if (strncmp("ext=", argv[i], 4) == 0)
		{
			// SO name is at argv[i]+4
			void *dlhandle;
			void *initfn;
			int (*rinitfn)(void);
			
			dlhandle = dlopen(argv[i] + 4, RTLD_NOW | RTLD_LOCAL);
			if (!dlhandle)
			{
				fprintf(stderr, "Cannot load extension %s: %s\n", argv[i] + 4, dlerror());
				continue;
			}
			initfn = dlsym(dlhandle, "lwwire_register");
			if (!initfn)
			{
				dlclose(dlhandle);
				fprintf(stderr, "Extension '%s' is not valid.\n", argv[i] + 4);
				continue;
			}
			rinitfn = initfn;
			if ((*rinitfn)() != 0)
			{
				dlclose(dlhandle);
				fprintf(stderr, "Initialization of extension '%s' failed.\n", argv[i] + 4);
				continue;
			}
		}
		else if (strncmp("drive=", argv[i], 6) == 0)
		{
			int dn=0;
			int isconst = 0;
			char *ptr;
			ptr = argv[i] + 6;
			if (*ptr == 'C')
			{
				isconst = 1;
				ptr++;
			}
			while (*ptr >= '0' && *ptr <= '9')
			{
				dn = dn * 10 + (*ptr - '0');
				ptr++;
			}
			if (*ptr != ',' || dn > 255)
			{
				fprintf(stderr, "Ignoring invalid drive specification: %s\n", argv[i]);
				continue;
			}
			ptr++;
			drivedata[dn].path = ptr;
			drivedata[dn].isconst = isconst;
		}
	}

	fprintf(stderr, "Running with the following disk images:\n");
	for (i = 0; i < 256; i++)
	{
		if (drivedata[i].path)
		{
			fprintf(stderr, "[%d%s] %s\n", i, drivedata[i].isconst ? "C" : "", drivedata[i].path);
		}
	}

	// setup dev
	dev_init( "wlan0" );

	// main loop reading operations and dispatching
	for (;;)
	{
		rv = dev_read(&buf);
		fprintf(stderr, "Handling opcode %02X\n", buf[0]);
		
		// we have an opcode here
		switch (buf[0])
		{
		case 0x00: // NOOP
			break;

		case 0x23: // TIME
			curtime = time(NULL);
			tmval = localtime(&curtime);
			buf[0] = tmval -> tm_year;
			buf[1] = tmval -> tm_mon + 1; // spec wants 1-12; localtime() gives 0-11
			buf[2] = tmval -> tm_mday;
			buf[3] = tmval -> tm_hour;
			buf[4] = tmval -> tm_min;
			buf[5] = tmval -> tm_sec;
			dev_write(buf, 6);
			dev_flush();
			break;
		
		case 0x46: // PRINTFLUSH
			// no printer is supported by this implemention so NO-OP
			break;
		
		case 0x47: // GETSTAT (useless dw3 operation)
		case 0x53: // SETSTAT (useless dw3 operation)
			// burn two bytes from the client and do nothing
			lwwire_read(buf, 2);
			break;
		
		case 0x49: // INIT (old style INIT call)
		case 0x54: // TERM (old DW3 op treated same as INIT)
		case 0xF8: // RESET3 (junk on the line during reset)
		case 0xFE: // RESET1 (junk on the line during reset)
		case 0xFF: // RESET2 (junk on the line during reset)
			lwwire_reset();
			break;
		
		case 0x50: // PRINT
			// burn a byte because we don't support any printers
			lwwire_read(buf, 1);
			fprintf(stderr, "%c", *buf );
			break;
		
		case 0x52: // READ
		case 0x72: // REREAD (same semantics as READ)
		    fprintf(stderr, "DWPROTO: read()\n");
			lwwire_proto_read();
			break;
		
		case 0x57: // WRITE
		case 0x77: // REWRITE (same semantics as WRITE)
		    fprintf(stderr, "DWPROTO: write()\n");
			lwwire_proto_write();
			break;
		
		case 0x5A: // DWINIT (new style init)
			lwwire_reset();
			if (lwwire_read(buf, 1) < 0)
				break;
			fprintf(stderr, "DWINIT: client drive code %02X\n", buf[0]);
			// tell the client we speak lwwire protocol
			buf[0] = 0x80;
			lwwire_write(buf, 1);
			break;
		
		case 0xD2: // READEX (improved reading operation)
		case 0xF2: // REREADEX (same semantics as READEX)
		    fprintf(stderr, "DWPROTO: readex()\n");
			lwwire_proto_readex();
			break;
		
		case 0xF0: // REQUESTEXTENSION
			lwwire_proto_requestextension();
			break;
		
		case 0xF1: // DISABLEEXTENSION
			lwwire_proto_disableextension();
			break;
		
		case 0xF3: // EXTENSIONOP
			lwwire_proto_extensionop();
			break;
		
		default:
			fprintf(stderr, "Unrecognized operation code %02X. Doing error state.\n", buf[0]);
			lwwire_protoerror();
			break;
		}
	}
}

// protocol handling functions
void lwwire_proto_read(void)
{
	unsigned char buf[259];
	int ec;
	int lsn;
	int i;
	
	if (lwwire_read(buf, 4) < 0)
		return;
	
	lsn = (buf[1] << 16) | (buf[2] << 8) | buf[3];
	
	ec = lwwire_fetch_sector(buf[0], lsn, buf + 1);
	buf[0] = ec;
	lwwire_write(buf, 1);
	if (ec)
		return;
	// all this futzing around here is probably a long enough
	// delay but testing on real hardware is needed here
	ec = 0;
	for (i = 1; i < 257; i++)
		ec += buf[i];
	buf[257] = (ec >> 8) & 0xff;
	buf[258] = ec & 0xff;
	lwwire_write(buf + 1, 258);
}

void lwwire_proto_write(void)
{
	unsigned char buf[262];
	int lsn;
	int ec;
	int i;	
	if (lwwire_read(buf, 262) < 0)
		return;
	
	lsn = (buf[1] << 16) | (buf[2] << 8) | buf[3];
	for (ec = 0, i = 4; i < 260; i++)
		ec += buf[i];
	if (ec != ((buf[260] << 8) | buf[261]))
	{
		buf[0] = LWERR_CHECKSUM;
	}
	else
	{
		ec = lwwire_save_sector(buf[0], lsn, buf + 4);
		buf[0] = ec;
	}
	lwwire_write(buf, 1);
}

void lwwire_proto_readex(void)
{
	unsigned char buf[256];
	int lsn;
	int ec;
	int i;
	int csum;
	if (lwwire_read(buf, 4) < 0)
		return;
	lsn = (buf[1] << 16) | (buf[2] << 8) | buf[3];
	ec = lwwire_fetch_sector(buf[0], lsn, buf);
	if (ec)
		memset(buf, 0, 256);
	for (i = 0, csum = 0; i < 256; i++)
		csum += buf[i];
	lwwire_write(buf, 256);
	if ((i = lwwire_read2(buf, 2, 5)) < 0)
	{
		fprintf(stderr, "Error reading protocol bytes: %d, %s\n", i, strerror(errno));
		return;
	}
	i = (buf[0] << 8) | buf[1];
	if (i != csum)
		ec = LWERR_CHECKSUM;
	buf[0] = ec;
	lwwire_write(buf, 1);
}

void lwwire_proto_requestextension(void)
{
	unsigned char buf[1];
	int ext;
		
	if (lwwire_read(buf, 1) < 0)
		return;
	ext = buf[0];
	buf[0] = 0x55;	// default to NAK
	if (lwwire_extension_list[ext].enable)
	{
		if ((*(lwwire_extension_list[ext].enable))() == 0)
		{
			// enable succeeded; enable it
			buf[0] = 0x42;
			lwwire_extension_list[ext].enabled = 1;
		}
	}
	lwwire_write(buf, 1);
}

void lwwire_proto_disableextension(void)
{
	unsigned char buf[1];
	int ext;
	
	if (lwwire_read(buf, 1) < 0)
		return;
	ext = buf[0];
	
	buf[0] = 0x42;	// default to ACK
	if (lwwire_extension_list[ext].disable)
	{
		if ((*(lwwire_extension_list[ext].disable))() != 0)
		{
			// extension says it can't be disabled; NAK response
			buf[0] = 0x55;
		}
	}
	if (buf[0] == 0x42)
		lwwire_extension_list[ext].enabled = 0;
	lwwire_write(buf, 1);
}

void lwwire_proto_extensionop(void)
{
	unsigned char buf[2];
	int ext;
	int op;
	
	if (lwwire_read(buf, 2) < 0)
		return;
	ext = buf[0];
	op = buf[1];
	if (lwwire_extension_list[ext].enabled == 1)
	{
		if (lwwire_extension_list[ext].handler)
		{
			if ((*(lwwire_extension_list[ext].handler))(op) == 0)
				return;
		}
		lwwire_protoerror();
		return;
	}
	// extension not enabled; do a protocol error
	lwwire_protoerror();
}


/*
Write data to the output. This will time out after 10 seconds. The timeout
is only there in case the underlying communication channel goes out to lunch.

It returns -1 on error or len on success.

The timeout requires the file descriptor to be non-blocking.

*/
int lwwire_writedata(void *buf, int len)
{
	int towrite = len;
	int rv;
	fd_set fdset;
	struct timeval timeout;
	struct timespec sltime;
	struct timespec rtime;

        
	// Sleep for 50us to client has time to start
	// listening	
	sltime.tv_sec = 0;
	sltime.tv_nsec = 50000;
	while (nanosleep(&sltime, &rtime) < 0)
	{
		// anything other than EINTR indicates something seriously messed up
		if (errno != EINTR)
			return -1;
		sltime = rtime;
	}


	while (towrite > 0)
	{
		rv = write(1, buf, towrite);
		if (rv == towrite)
			break;
		if (rv < 0)
		{
			if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			{
				return -1;
			}
			rv = 0;
		}
		// now rv is the number of bytes read
		buf += rv;
		towrite -= rv;
		
		// now wait for the descriptor to be writable
		FD_ZERO(&fdset);
		FD_SET(1, &fdset);
		timeout.tv_sec = 10;
		timeout.tv_usec = 0;
		
		rv = select(2, NULL, &fdset, NULL, &timeout);
		if (rv < 0)
		{
			// this is a last ditch effort to not break completely
			// in the face of a signal; it should occur only rarely
			// and it is not clear what the correct behaviour should
			// be.
			if (errno == EINTR)
				continue;
			return -1;
		}
		// timeout condition
		if (rv == 0)
		{
			errno = ETIMEDOUT;
			return -1;
		}
		// anything else here means we have more bytes to write
	}
	fprintf(stderr, "Protocol bytes written (%d):", len);
	for (rv = 0; rv < len; rv++)
	    fprintf(stderr, " %02X ", ((char *)(buf))[rv] & 0xff);
	    	fprintf(stderr, "\n");
	return len;
}

// like lwwire_writedata() except it bails the program on error.
void lwwire_write(void *buf, int len)
{
	if (lwwire_writedata(buf, len) < 0)
	{
		fprintf(stderr, "Error writing %d bytes to client: %s\n", len, strerror(errno));
		exit(1);
	}
}


/*
Handle a protocol error by maintaining radio silence for
at least 1100 ms. The pause must be *at least* 1100ms so
it's no problem if the messing about takes longer. Do a
state reset after the error.
*/
void lwwire_protoerror(void)
{
	struct timespec sltime;
	struct timespec rtime;
	
	sltime.tv_sec = 1;
	sltime.tv_nsec = 100000000;
	
	while (nanosleep(&sltime, &rtime) < 0)
	{
		// anything other than EINTR indicates something seriously messed up
		if (errno != EINTR)
			break;
		sltime = rtime;
	}
	lwwire_reset();
}

/* fetch a file pointer for the specified drive number */
FILE *lwwire_fetch_drive_fp(int dn)
{
	if (drivedata[dn].path == NULL)
		return NULL;
	if (drivedata[dn].fp)
	{
		if (ferror(drivedata[dn].fp))
			fclose(drivedata[dn].fp);
		else
			return drivedata[dn].fp;
	}
	
	drivedata[dn].fp = fopen(drivedata[dn].path, "r+");
	if (!drivedata[dn].fp)
	{
		if (errno == ENOENT && !drivedata[dn].isconst)
		{
			drivedata[dn].fp = fopen(drivedata[dn].path, "w+");
		}
	}
	return drivedata[dn].fp;
}

int lwwire_drive_readonly(int dn)
{
	return drivedata[dn].isconst;
}


/* read a sector from a disk image */
int lwwire_fetch_sector(int dn, int lsn, void *buf)
{
	FILE *fp;
	int rc;
		
	fp = lwwire_fetch_drive_fp(dn);
	if (!fp)
		return LWERR_NOTREADY;
	
	if (fseek(fp, lsn * 256, SEEK_SET) < 0)
		return LWERR_READ;
	rc = fread(buf, 1, 256, fp);
	if (rc < 256)
	{
		memset(buf + rc, 0, 256 - rc);
	}
	return 0;
}

int lwwire_save_sector(int dn, int lsn, void *buf)
{
	FILE *fp;
	int rc;

	if (lwwire_drive_readonly(dn))
		return LWERR_WRITE;
	fp = lwwire_fetch_drive_fp(dn);
	if (!fp)
		return LWERR_NOTREADY;
	if (drivedata[dn].isconst)
		return LWERR_WRITE;
	if (fseek(fp, lsn * 256, SEEK_SET) < 0)
		return LWERR_WRITE;
	rc = fwrite(buf, 1, 256, fp);
	if (rc < 256)
		return LWERR_WRITE;
	return 0;
}	


/*
Reset the protocol state to "base protocol" mode.
*/
void lwwire_reset(void)
{
	int i;
	
	// tell all extensions to reset and disable them
	for (i = 0; i < 256; i++)
	{
		if (lwwire_extension_list[i].handler)
		{
			(*(lwwire_extension_list[i].reset))();
		}
		lwwire_extension_list[i].enabled = 0;
	}
}

/*
Register an extension. This will replace the registration if the same
extension number is registered twice. Returns -1 on parameter error.
ALL function arguments are required. num must be between 0 and 255 inclusive
currently.  Returns 0 if the extension is registered.
*/
int lwwire_register_extension(int num, int (*handler)(int), int (*enable)(void), int (*disable)(void), void (*reset)(void))
{
	if (!handler || !enable || !disable || !reset)
		return -1;
	if (num < 0 || num > 255)
		return -1;
	lwwire_extension_list[num].handler = handler;
	lwwire_extension_list[num].enable = enable;
	lwwire_extension_list[num].disable = disable;
	lwwire_extension_list[num].reset = reset;
	return 0;
}
