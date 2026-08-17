/* Small IPC client for SC's private, per-terminal control socket. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char* argv[])
{
	const char* socket_path;
	struct sockaddr_un address = {0};
	char request[32];
	char reply[4096];
	int fd;
	ssize_t n;
	size_t request_len = 0;

	if ( argc < 2 )
		return 2;
	for ( int i = 1; i < argc; ++i ) {
		const size_t arg_len = strlen(argv[i]);
		const size_t separator_len = i > 1;
		if ( sizeof(request) < request_len + separator_len + arg_len + 2 )
			return 2;
		if ( separator_len )
			request[request_len++] = ' ';
		memcpy(request + request_len, argv[i], arg_len);
		request_len += arg_len;
	}
	request[request_len++] = '\n';
	request[request_len] = '\0';

	if ( (socket_path = getenv("SC_SOCKET")) == NULL
	  || socket_path[0] == '\0'
	  || strlen(socket_path) >= sizeof(address.sun_path) )
		return 1;

	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, socket_path);
	if ( (fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0 )
		return 1;
	if ( connect(fd, (struct sockaddr*)&address, sizeof(address)) < 0
 	  || write(fd, request, strlen(request)) < 0 ) {
		close(fd);
		return 1;
	}

	n = read(fd, reply, sizeof(reply));
	close(fd);

	if ( n <= 0 || fwrite(reply, 1, (size_t)n, stdout) != (size_t)n )
		return 1;
	return 0;
}
