#include <linux/tcp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>

int main(int argc, char **argv)
{
    if (argc != 3)
        return 1;
    struct tcp_info sender = {0}, receiver = {0};
    socklen_t sender_size = sizeof(sender), receiver_size = sizeof(receiver);
    if (getsockopt(atoi(argv[1]), IPPROTO_TCP, TCP_INFO, &sender, &sender_size) ||
        getsockopt(atoi(argv[2]), IPPROTO_TCP, TCP_INFO, &receiver, &receiver_size))
        return 1;
    if (sender_size < offsetof(struct tcp_info, tcpi_bytes_retrans) + sizeof(sender.tcpi_bytes_retrans) ||
        receiver_size < offsetof(struct tcp_info, tcpi_bytes_retrans) + sizeof(receiver.tcpi_bytes_retrans))
        return 1;
    printf("{\"sender_tcp_data_bytes_sent\": %llu, \"receiver_tcp_data_bytes_sent\": %llu, "
           "\"sender_tcp_bytes_retransmitted\": %llu, \"receiver_tcp_bytes_retransmitted\": %llu}\n",
           (unsigned long long)sender.tcpi_bytes_sent, (unsigned long long)receiver.tcpi_bytes_sent,
           (unsigned long long)sender.tcpi_bytes_retrans, (unsigned long long)receiver.tcpi_bytes_retrans);
    return 0;
}
