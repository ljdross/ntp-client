#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ctype.h>
#include <time.h>

#define SERVERPORT "123"

int digits_only(const char *s) {
    while (*s) {
        if (isdigit(*s++) == 0) return 0;
    }
    return 1;
}

void *get_in_addr(struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*) sa)->sin_addr);
    }
    return &(((struct sockaddr_in6*) sa)->sin6_addr);
}

long double unmarshal_ntpshort(const uint8_t * timestamp_ptr) {
    uint16_t seconds = ntohs( *(uint16_t *) timestamp_ptr);
    long double sec = seconds; // - 2208988800 not applicable for root dispersion
    uint16_t fraction = ntohs( *(uint16_t *) (timestamp_ptr + 2));
    long double nsec = fraction;
    nsec /= 65536;
    sec += nsec;
    return sec;
}

long double unmarshal_ntptimestamp(const uint8_t * timestamp_ptr) {
    uint32_t seconds = ntohl( *(uint32_t *) timestamp_ptr);
    long double sec = seconds - 2208988800; // 2208988800 seconds between year 1900 and 1970
    uint32_t fraction = ntohl( *(uint32_t *) (timestamp_ptr + 4));
    long double nsec = fraction;
    nsec /= 4294967296;
    sec += nsec;
    return sec;
}


int main(int argc, char **argv) {
    int sockfd;
    struct addrinfo hints, *servinfo, *p;
    int rv;
    int numbytes;
    long n;
    uint8_t msg[48] = {0};
    msg[0] = 35; // LI=0 VN=4 Mode=3 (00 100 011 == 35)
    uint8_t buf[48];
    struct sockaddr_storage their_addr;
    socklen_t addr_len;
    char s[INET6_ADDRSTRLEN];
    struct timespec t[4];
    long double d[4];
    long double root_disp, offset, rtt, delay, dispersion, max, min;
    const long double two = 2;
    const long double onebillion = 1000000000;

    if (argc < 3) {
        fprintf(stderr, "usage: ntpclient number_of_requests_per_server server1 server2 server3 ...\n");
        exit(1);
    }
    if (!digits_only(argv[1])) {
        fprintf(stderr,"ntpclient: wrong input! only digits (no minus!) for number_of_requests_per_server allowed!\n");
        exit(2);
    }
    n = strtol(argv[1], NULL, 10);
    if (n < 1) {
        fprintf(stderr,"ntpclient: wrong input! only numbers > 0 for number_of_requests_per_server allowed!\n");
        exit(3);
    }

    for (int i = 2; i < argc; i++) {
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;

        long double latest_rtt[8] = {0};

        if ((rv = getaddrinfo(argv[i], SERVERPORT, &hints, &servinfo)) != 0) {
            fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
            return 1;
        }

        for (p = servinfo; p != NULL; p = p->ai_next) {
            if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
                perror("ntpclient: socket()");
                continue;
            }
            break;
        }
        if (p == NULL) {
            fprintf(stderr, "ntpclient: failed to create socket\n");
            return 2;
        }
        freeaddrinfo(servinfo);

        for (int j = 0; j < n; j++) {
            addr_len = sizeof(their_addr);
            if ((numbytes = sendto(sockfd, msg, 48, 0, p->ai_addr, p->ai_addrlen)) == -1) {
                perror("ntpclient: sendto()");
                exit(4);
            }
            if (clock_gettime(CLOCK_REALTIME, &t[0]) == -1) {
                perror("clock_gettime");
                exit(5);
            }

            fprintf(stderr, "ntpclient: sent %d bytes to %s\n", numbytes, argv[i]);
            fprintf(stderr, "ntpclient: waiting to recvfrom...\n");

            numbytes = 0;
            while ( numbytes < 48 ) {
                memset(buf, 0 , 48);
                if ((numbytes = recvfrom(sockfd, buf, 48, 0, (struct sockaddr *) &their_addr, &addr_len)) == -1) {
                    perror("recvfrom");
                    exit(6);
                }
            }
            if (clock_gettime(CLOCK_REALTIME, &t[3]) == -1) {
                perror("clock_gettime");
                exit(7);
            }

            fprintf(stderr, "ntpclient: got packet from %s\n", inet_ntop(their_addr.ss_family, get_in_addr((struct sockaddr *) &their_addr), s, sizeof(s)));
            fprintf(stderr, "ntpclient: packet is %d bytes long\n", numbytes);
            fprintf(stderr, "ntpclient: content of packet is: \"");
            fwrite(buf, sizeof(uint8_t), numbytes, stderr);
            fprintf(stderr, "\"\n");

            root_disp = unmarshal_ntpshort(buf + 8);

            for (int k = 0; k < 4; k += 3) {
                d[k] = t[k].tv_sec;
                long double temp = t[k].tv_nsec;
                temp /= onebillion;
                d[k] += temp;
            }
            d[1] = unmarshal_ntptimestamp(buf + 32);
            d[2] = unmarshal_ntptimestamp(buf + 40);

            offset = ((d[1] - d[0]) + (d[2] - d[3])) / two;

            rtt = (d[3] - d[0]) - (d[2] - d[1]);
            delay = rtt / two;

            latest_rtt[j % 8] = rtt;
            max = latest_rtt[0];
            min = latest_rtt[0];
            for (int k = 1; k < 8 && k <= j; k++) {
                if (latest_rtt[k] > max)
                    max = latest_rtt[k];
                if (latest_rtt[k] < min)
                    min = latest_rtt[k];
            }
            dispersion = max - min;

            fprintf(stdout, "%s;%d;%Lf;%Lf;%Lf;%Lf\n", argv[i], j, root_disp, dispersion, delay, offset);

            if (j != n - 1)
                sleep(8);
        }

        close(sockfd);
    }

    return 0;
}