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

struct timespec unmarshal_ntpshort(const uint8_t * timestamp_ptr) {
    struct timespec t;
    uint16_t seconds = ntohs( *(uint16_t *) timestamp_ptr);
    t.tv_sec = seconds; // - 2208988800 not applicable for root dispersion
    uint16_t fraction = ntohs( *(uint16_t *) (timestamp_ptr + 2));
    uint64_t nsec = fraction;
    nsec *= 1000000000;
    nsec /= 4294967296;
    t.tv_nsec = nsec;
    return t;
}

struct timespec unmarshal_ntptimestamp(const uint8_t * timestamp_ptr) {
    struct timespec t;
    uint32_t seconds = ntohl( *(uint32_t *) timestamp_ptr);
    t.tv_sec = seconds - 2208988800; // 2208988800 seconds between year 1900 and 1970
    uint32_t fraction = ntohl( *(uint32_t *) (timestamp_ptr + 4));
    uint64_t nsec = fraction;
    nsec *= 1000000000;
    nsec /= 4294967296;
    t.tv_nsec = nsec;
    return t;
}


long double unmarshal_ntptimestamp_double(const uint8_t * timestamp_ptr) {
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
    int n;
    uint8_t msg[48] = {0};
    msg[0] = 35; // LI=0 VN=4 Mode=3 (00 100 011 == 35)
    uint8_t buf[48];
    struct sockaddr_storage their_addr;
    socklen_t addr_len;
    char s[INET6_ADDRSTRLEN];
    struct timespec root_dispersion;
    struct timespec t[4];
    long double d[4];
    long double offset2, offset1, offset, rtt, delay, dispersion, max, min, t1, t2;
    const long double two = 2;
    const long double onebillion = 1000000000;
    time_t offset_sec;
    long offset_nsec;

    if (argc < 3) {
        fprintf(stderr, "usage: ntpclient number_of_requests_per_server server1 server2 server3 ...\n");
        exit(1);
    }
    if (!digits_only(argv[1])) {
        fprintf(stderr,"ntpclient: wrong input! only digits for number_of_requests_per_server allowed!\n");
        exit(2);
    }
    n = atoi(argv[1]);
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
            clock_gettime(CLOCK_REALTIME, &t[0]); // TODO: check for errors, maybe move behind sendto()?
            if ((numbytes = sendto(sockfd, msg, 48, 0, p->ai_addr, p->ai_addrlen)) == -1) {
                perror("ntpclient: sendto()");
                exit(1);
            }

            fprintf(stderr, "ntpclient: sent %d bytes to %s\n", numbytes, argv[i]);
            fprintf(stderr, "ntpclient: waiting to recvfrom...\n");

            memset(buf, 0 , 48);
            addr_len = sizeof(their_addr);
            if ((numbytes = recvfrom(sockfd, buf, 48, 0, (struct sockaddr *) &their_addr, &addr_len)) == -1) {
                perror("recvfrom");
                exit(1);
            }
            clock_gettime(CLOCK_REALTIME, &t[3]); // TODO: check for errors

            fprintf(stderr, "ntpclient: got packet from %s\n", inet_ntop(their_addr.ss_family, get_in_addr((struct sockaddr *) &their_addr), s, sizeof(s)));
            fprintf(stderr, "ntpclient: packet is %d bytes long\n", numbytes);
            fprintf(stderr, "ntpclient: content of packet is: \"");
            fwrite(buf, sizeof(uint8_t), numbytes, stderr);
            fprintf(stderr, "\"\n");

            root_dispersion = unmarshal_ntpshort(buf + 8);
//            fprintf(stderr, "ntpclient: root_dispersion: %lld.%.9ld\n", (long long) root_dispersion.tv_sec, root_dispersion.tv_nsec);

            t[1] = unmarshal_ntptimestamp(buf + 32);
            t1 = unmarshal_ntptimestamp_double(buf + 32);
            t[2] = unmarshal_ntptimestamp(buf + 40);
            t2 = unmarshal_ntptimestamp_double(buf + 40);

            for (int k = 0; k < 4; k++) {
                d[k] = t[k].tv_sec;
                long double temp = t[k].tv_nsec;
                temp /= onebillion;
                d[k] += temp;
//                d[k] = (long double) t[k].tv_sec + ((long double) t[k].tv_nsec / (long double) 1000000000);
                fprintf(stderr, "ntpclient: t%d: %lld.%.9ld\n", k + 1, (long long) t[k].tv_sec, t[k].tv_nsec);
//                fprintf(stderr, "ntpclient: t%d: %Lf\n", k + 1, d[k]);
            }

            offset2 = ((t1 - d[0]) + (t2 - d[3])) / two;

            offset1 = ((d[1] - d[0]) + (d[2] - d[3])) / two;

            offset_sec = ((t[1].tv_sec - t[0].tv_sec) + (t[2].tv_sec - t[3].tv_sec)) / (time_t)2;
            offset_nsec = ((t[1].tv_nsec - t[0].tv_nsec) + (t[2].tv_nsec - t[3].tv_nsec)) / (long)2;
            offset = (long double) offset_sec + ((long double) offset_nsec / (long double) 1000000000);

            rtt = (d[3] - d[0]) - (d[2] - d[1]);
            delay = rtt / 2;

            latest_rtt[j % 8] = rtt;
            max = latest_rtt[0];
            min = latest_rtt[0];
            for (int k = 1; k < 8 && k <= j; k++) {
                if (latest_rtt[k] > max) max = latest_rtt[k];
                if (latest_rtt[k] < min) min = latest_rtt[k];
            }
            dispersion = max - min;

//            fprintf(stdout, "%Lf\n", offset1);
            fprintf(stdout, "%s;%d;%lld.%.9ld;%Lf;%Lf;%Lf\n", argv[i], j, (long long) root_dispersion.tv_sec, root_dispersion.tv_nsec, dispersion, delay, offset2);


//            if (j != n - 1)
                sleep(8);
        }

        close(sockfd);
    }


    return 0;
}