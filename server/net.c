/*******************************************************************************
 *
 * Copyright (c) 2004 Guillaume Cottenceau
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 ******************************************************************************/

/*
 * this file holds network transmission operations.
 * it should be as far away as possible from game considerations
 */

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <glib.h>

#include "game.h"
#include "tools.h"
#include "log.h"
#include "net.h"

/* this is set in game.c and used here for answering with
 * the requested command without passing this additional arg */
char* current_command;

const int proto_major = 1;
const int proto_minor = 0;

static char greets_msg_base[] = "SERVER_READY %s";
static char* servername = NULL;

static char ok_generic[] = "OK";

static char fl_client_nolf[] = "NO_LF_WITHIN_TOO_MUCH_DATA (I bet you're not a regular FB client, hu?)";
static char fl_server_full[] = "SERVER_IS_FULL";
static char fl_server_overloaded[] = "SERVER_IS_OVERLOADED";

static double date_amount_transmitted_reset;

#define DEFAULT_PORT 1511  // a.k.a 0xF 0xB thx misc
#define DEFAULT_MAX_USERS 200
#define DEFAULT_MAX_TRANSMISSION_RATE 100000
static int port = DEFAULT_PORT;
static int max_users = DEFAULT_MAX_USERS;
static int max_transmission_rate = DEFAULT_MAX_TRANSMISSION_RATE;

static int lan_game_mode = 0;

static int tcp_server_socket;
static int udp_server_socket = -1;

static GList * conns = NULL;
static GList * conns_prio = NULL;

#define INCOMING_DATA_BUFSIZE 4096
static char incoming_data_buffers[256][INCOMING_DATA_BUFSIZE] __attribute__((aligned(INCOMING_DATA_BUFSIZE)));
static int incoming_data_buffers_count[256];

/* send line adding the protocol in front of the supplied msg */
static ssize_t send_line(int fd, char* msg)
{
        char buf[1000];
        if (current_command)
                snprintf(buf, sizeof(buf), "FB/%d.%d %s: %s\n", proto_major, proto_minor, current_command, msg);
        else 
                snprintf(buf, sizeof(buf), "FB/%d.%d ???: %s\n", proto_major, proto_minor, msg);
        return send(fd, buf, strlen(buf), 0);
}

ssize_t send_line_log(int fd, char* dest_msg, char* inco_msg)
{
        l3("[%d] %s -> %s", fd, inco_msg, dest_msg);
        return send_line(fd, dest_msg);
}

ssize_t send_line_log_push(int fd, char* dest_msg)
{
        char * tmp = current_command;
        ssize_t b;
        current_command = "PUSH";
        l2("[%d] PUSH %s", fd, dest_msg);
        b = send_line(fd, dest_msg);
        current_command = tmp;
        return b;
}

ssize_t send_line_log_push_binary(int fd, char* dest_msg, char* printable_msg)
{
        char * tmp = current_command;
        ssize_t b;
        current_command = "PUSH";
        l2("[%d] PUSH (binary message) %s", fd, printable_msg);
        b = send_line(fd, dest_msg);
        current_command = tmp;
        return b;
}

ssize_t send_ok(int fd, char* inco_msg)
{
        return send_line_log(fd, ok_generic, inco_msg);
}


static void fill_conns_set(gpointer data, gpointer user_data)
{
        FD_SET(GPOINTER_TO_INT(data), (fd_set *) user_data);
}

static GList * new_conns;
static int prio_processed;
static void handle_incoming_data_generic(gpointer data, gpointer user_data, int prio)
{
        int fd;

        if (FD_ISSET((fd = GPOINTER_TO_INT(data)), (fd_set *) user_data)) {
                int conn_terminated = 0;
                char buf[INCOMING_DATA_BUFSIZE];
                ssize_t len;
                ssize_t offset = incoming_data_buffers_count[fd];
                incoming_data_buffers_count[fd] = 0;
                memcpy(buf, incoming_data_buffers[fd], offset);
                len = recv(fd, buf + offset, INCOMING_DATA_BUFSIZE - 1 - offset, 0);
                if (len <= 0) {
                        l1("[%d] Peer shutdown", fd);
                        if (len == -1)
                                l2("[%d] This happened on a system error: %s", fd, strerror(errno));
                        goto conn_terminated;
                } else {
                        len += offset;
                        // If we don't have a newline, it means we are seeing a partial send. Buffer
                        // them, since we can't synchronously wait for newline now or else we'd offer a
                        // nice easy shot for DOS (and beside, this would slow down the whole rest).
                        if (buf[len-1] != '\n') {
                                if (len == INCOMING_DATA_BUFSIZE - 1) {
                                        send_line_log_push(fd, fl_client_nolf);
                                        goto conn_terminated;
                                }
                                l2("[%d] ****** buffering %d bytes", fd, len);
                                memcpy(incoming_data_buffers[fd], buf, len);
                                incoming_data_buffers_count[fd] = len;
                                return;
                        }
                        /* string operations will need a NULL conn_terminated string */
                        buf[len] = '\0';

                        if (prio) {
                                // prio e.g. in game
                                process_msg_prio(fd, buf, len + 1);
                                prio_processed = 1;
                        } else {
                                char * eol;
                                char * line = buf;
                                /* loop (to handle case when network gives us several lines at once) */
                                while (!conn_terminated && (eol = strchr(line, '\n'))) {
                                        eol[0] = '\0';
                                        if (strlen(line) > 0 && eol[-1] == '\r')
                                                eol[-1] = '\0';
                                        conn_terminated = process_msg(fd, line);
                                        line = eol + 1;
                                }

                                if (conn_terminated) {
                                conn_terminated:
                                        l1("[%d] Closing connection", fd);
                                        close(fd);
                                        player_part_game(fd);
                                        new_conns = g_list_remove(new_conns, data);
                                        player_disconnects(fd);
                                        if (lan_game_mode && g_list_length(new_conns) == 0 && udp_server_socket == -1) {
                                                l0("LAN game mode server exiting on last client exit.");
                                                exit(0);
                                        }
                                }
                        }
                }
        }
}

static void handle_incoming_data(gpointer data, gpointer user_data)
{
        handle_incoming_data_generic(data, user_data, 0);
}
static void handle_incoming_data_prio(gpointer data, gpointer user_data)
{
        handle_incoming_data_generic(data, user_data, 1);
}

static void handle_udp_request(void)
{
        static char ok_input_base[] = "FB/%d.%d SERVER PROBE";
        static char * ok_input = NULL;
        static char fl_unrecognized[] = "FB/1.0 You don't exist, go away.\n";
        static char ok_answer_base[] = "FB/%d.%d SERVER HERE AT PORT %d";
        static char * ok_answer = NULL;
        int n;
        char msg[128];
        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        char * answer;
        
        if (!ok_input)   // C sux
                ok_input = asprintf_(ok_input_base, proto_major, proto_minor);
        if (!ok_answer)
                ok_answer = asprintf_(ok_answer_base, proto_major, proto_minor, port);


        memset(msg, 0, sizeof(msg));
        n = recvfrom(udp_server_socket, msg, sizeof(msg), 0, (struct sockaddr *) &client_addr, &client_len);
        if (n == -1) {
                perror("recvfrom");
                return;
        }
        
        l2("UDP server receives %d bytes from %s.", n, inet_ntoa(client_addr.sin_addr));
        if (strcmp(msg, ok_input) || (lan_game_mode && g_list_length(conns_prio) > 0)) {
                answer = fl_unrecognized;
                l0("Unrecognized/full.");
        } else {
                answer = ok_answer;
                l0("Valid FB server probe, answering.");
        }
        
        if (sendto(udp_server_socket, answer, strlen(answer), 0, (struct sockaddr *) &client_addr, sizeof(client_addr)) != strlen(answer)) {
                perror("sendto");
        }
}

void connections_manager(void)
{
        struct sockaddr_in client_addr;
        ssize_t len = sizeof(client_addr);
        struct timeval tv;
        static char * greets_msg = NULL;
        if (!greets_msg)   // C sux
                greets_msg = asprintf_(greets_msg_base, servername);

        date_amount_transmitted_reset = get_current_time();

        while (1) {
                int fd;
                int retval;
                fd_set conns_set;

                FD_ZERO(&conns_set);
                g_list_foreach(conns, fill_conns_set, &conns_set);
                g_list_foreach(conns_prio, fill_conns_set, &conns_set);
                if (tcp_server_socket != -1)
                        FD_SET(tcp_server_socket, &conns_set);
                if (udp_server_socket != -1)
                        FD_SET(udp_server_socket, &conns_set);
               
                tv.tv_sec = 30;
                tv.tv_usec = 0;

                if ((retval = select(FD_SETSIZE, &conns_set, NULL, NULL, &tv)) == -1) {
                        perror("select");
                        exit(-1);
                }

                // timeout
                if (!retval)
                        continue;

                prio_processed = 0;
                new_conns = g_list_copy(conns_prio);
                g_list_foreach(conns_prio, handle_incoming_data_prio, &conns_set);
                g_list_free(conns_prio);
                conns_prio = new_conns;

                // prio has higher priority (astounding statement, eh?)
                if (prio_processed)
                        continue;

                new_conns = g_list_copy(conns);
                g_list_foreach(conns, handle_incoming_data, &conns_set);
                g_list_free(conns);
                conns = new_conns;

                if (tcp_server_socket != -1 && FD_ISSET(tcp_server_socket, &conns_set)) {
                        if ((fd = accept(tcp_server_socket, (struct sockaddr *) &client_addr, (socklen_t *) &len)) == -1) {
                                perror("accept");
                                exit(-1);
                        }
                        l2("Accepted connection from %s: fd %d", inet_ntoa(client_addr.sin_addr), fd);
                        if (fd > 255 || conns_nb() >= max_users || (lan_game_mode && g_list_length(conns_prio) > 0)) {
                                send_line_log_push(fd, fl_server_full);
                                l1("[%d] Closing connection", fd);
                                close(fd);
                        } else {
                                double now = get_current_time();
                                double rate = get_reset_amount_transmitted() / (now - date_amount_transmitted_reset);
                                l1("Transmission rate: %.2f bytes/sec", rate);
                                date_amount_transmitted_reset = now;
                                if (rate > max_transmission_rate) {
                                        send_line_log_push(fd, fl_server_overloaded);
                                        l1("[%d] Closing connection", fd);
                                        close(fd);
                                } else {
                                        send_line_log_push(fd, greets_msg);
                                        conns = g_list_append(conns, GINT_TO_POINTER(fd));
                                        player_connects(fd);
                                        memset(incoming_data_buffers[fd], 0, sizeof(incoming_data_buffers[fd]));  // force Linux to allocate now
                                        incoming_data_buffers_count[fd] = 0;
                                        calculate_list_games();
                                }
                        }
                }

                if (udp_server_socket != -1 && FD_ISSET(udp_server_socket, &conns_set))
                        handle_udp_request();
        }
}


int conns_nb(void)
{
        return g_list_length(conns_prio) + g_list_length(conns);
}

#ifdef DEBUG
void net_debug(void)
{
        printf("conns_prio:%d;conns:%d\n", g_list_length(conns_prio), g_list_length(conns));
}
#endif
        
void add_prio(int fd)
{
        conns_prio = g_list_append(conns_prio, GINT_TO_POINTER(fd));
        new_conns = g_list_remove(new_conns, GINT_TO_POINTER(fd));
        if (lan_game_mode && g_list_length(conns_prio) > 0 && udp_server_socket != -1) {
                close(tcp_server_socket);
                close(udp_server_socket);
                tcp_server_socket = udp_server_socket = -1;
        }
}

void help(void)
{
        printf("[[ Frozen-Bubble server ]]\n");
        printf(" \n");
        printf("Copyright (c) Guillaume Cottenceau, 2004.\n");
        printf("\n");
        printf("This program is free software; you can redistribute it and/or modify\n");
        printf("it under the terms of the GNU General Public License version 2, as\n");
        printf("published by the Free Software Foundation.\n");
        printf(" \n");
        printf("Usage: fb-server [OPTION]...\n");
        printf("\n");
        printf("     -h                        display this help then exits\n");
        printf("     -n name                   set the server name (mandatory)\n");
        printf("     -l                        LAN mode: create an UDP server (on port %d) to answer broadcasts of clients discovering where are the servers\n", DEFAULT_PORT);
        printf("     -L                        LAN/game mode: create an UDP server as above, but limit number of games to 1 (this is for an FB client hosting a LAN server)\n");
        printf("     -p port                   set the server port (defaults to %d)\n", DEFAULT_PORT);
        printf("     -u max_users              set the maximum of connected users (defaults to %d, physical maximum 252)\n", DEFAULT_MAX_USERS);
        printf("     -t max_transmission_rate  set the maximum transmission rate, in bytes per second (defaults to %d)\n", DEFAULT_MAX_TRANSMISSION_RATE);
}

void create_udp_server(void)
{
        struct sockaddr_in server_addr;

        l1("Creating UDP server for answering broadcast server discover, on default port %d", DEFAULT_PORT);

        udp_server_socket = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_server_socket < 0) {
                perror("socket");
                exit(1);
        }

        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        server_addr.sin_port = htons(DEFAULT_PORT);
        if (bind(udp_server_socket, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
                perror("bind");
                exit(1);
        }
}

void create_server(int argc, char **argv)
{
        struct sockaddr_in client_addr;
        int valone = 1;

        while (1) {
                int c = getopt(argc, argv, "hn:lLp:u:t:");
                if (c == -1)
                        break;
                
                switch (c) {
                case 'h':
                        help();
                        exit(0);
                case 'n':
                        if (strlen(optarg) > 12) {
                                l0("Commandline: name is too long, maximum is 12 characters");
                                exit(1);
                        } else {
                                int i;
                                for (i = 0; i < strlen(optarg); i++) {
                                        if (!((optarg[i] >= 'a' && optarg[i] <= 'z')
                                              || (optarg[i] >= '0' && optarg[i] <= '9')
                                              || optarg[i] == '.' || optarg[i] == '-')) {
                                                l0("Commandline: name must contain only chars in [a-z0-9.-]");
                                                exit(1);
                                        }
                                }
                                servername = strdup(optarg);
                        }
                        break;
                case 'l':
                        create_udp_server();
                        break;
                case 'L':
                        create_udp_server();
                        lan_game_mode = 1;
                        break;
                case 'p':
                        port = charstar_to_int(optarg);
                        if (port != 0)
                                l1("Commandline: setting port to %d", port);
                        else {
                                port = DEFAULT_PORT;
                                l1("Commandline: %s not convertible to int, ignoring", optarg);
                        }
                        break;
                case 'u':
                        max_users = charstar_to_int(optarg);
                        if (max_users != 0)
                                l1("Commandline: setting maximum users to %d", max_users);
                        else {
                                max_users = DEFAULT_MAX_USERS;
                                l1("Commandline: %s not convertible to int, ignoring", optarg);
                        }
                        break;
                case 't':
                        max_transmission_rate = charstar_to_int(optarg);
                        if (max_transmission_rate != 0)
                                l1("Commandline: setting maximum transmission rate to %d bytes/sec", max_transmission_rate);
                        else {
                                max_transmission_rate = DEFAULT_MAX_TRANSMISSION_RATE;
                                l1("Commandline: %s not convertible to int, ignoring", optarg);
                        }
                        break;
                }
        }

        if (!servername) {
                l0("Must give a name to the server with -n <name>.");
                exit(1);
        }

        l1("Creating TCP game server on port %d", port);

        tcp_server_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (tcp_server_socket < 0) {
                perror("socket");
                exit(-1);
        }

        setsockopt(tcp_server_socket, SOL_SOCKET, SO_REUSEADDR, &valone, sizeof(valone));

        client_addr.sin_family = AF_INET;
        client_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        client_addr.sin_port = htons(port);
        if (bind(tcp_server_socket, (struct sockaddr *) &client_addr, sizeof(client_addr))) {
                perror("bind");
                exit(-1);
        }

        if (listen(tcp_server_socket, 1000) < 0) {
                perror("listen");
                exit(-1);
        }
}
