#include "evhttpconn.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <getopt.h>
#include <sys/time.h>

#include <pcre.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

static void usage()
{
    printf("usage: \n");
}

typedef struct
{
    int code;
    int content_length;
    int millis;
} result_t;

typedef struct
{
    struct
    {
        int number;
        int concurrent;
        int threads;
        const char *url;
    } args;

    volatile int next_result_index;
    result_t *results;

    struct sockaddr_in serv_addr;
    evhttp_string_t request;
} state_t;

typedef struct
{
    int fd;
    evhttp_connection_t http_conn;
    int running;
    long started;
    result_t *results;
} connection_t;

typedef struct
{
    state_t *state;
    struct ev_loop *loop;
    connection_t *conns;
} worker_info_t;


static long now()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

static void on_first_line(evhttp_string_t first, evhttp_string_t second, evhttp_string_t third, void *data)
{
    connection_t *conn = (connection_t *)data;
    conn->results->code = strtol(second.data, NULL, 10);
    //printf("%i\n", conn->results->code);
}

static void on_chunk(evhttp_string_t content, void *data)
{
    connection_t *conn = (connection_t *)data;
    conn->results->content_length += content.length;
}

static void on_complete(void *data)
{
    connection_t *conn = (connection_t *)data;
    evhttp_connection_terminate(&conn->http_conn);
}

static void on_close(void *data)
{
    connection_t *conn = (connection_t *)data;
    conn->running = 0;
    conn->results->millis = now() - conn->started;
    close(conn->fd);
    //printf("%i\n", conn->results->millis);
}

static void *worker(worker_info_t *info)
{
    int c = 0, i, j;
    int done = 0;

    state_t *state = info->state;

    while (c<state->args.number)
    {
        for (i=0; i<state->args.concurrent; ++i)
        {
            while (!info->conns[i].running)
            {
                c = __sync_fetch_and_add(&state->next_result_index, 1);
                if (c >= state->args.number)
                    break;

                info->conns[i].started = now();
                info->conns[i].fd = socket(AF_INET, SOCK_STREAM, 0);
                if (connect(info->conns[i].fd, (struct sockaddr *)&state->serv_addr, sizeof(state->serv_addr)) < 0)
                {
                    // connection failed
                    state->results[c].code = -1;
                    state->results[c].content_length = -1;
                    state->results[c].millis = 0;
                    printf("conn failed %i\n", i);
                    close(info->conns[i].fd);
                }
                else
                {
                    evhttp_connection_init(&info->conns[i].http_conn,
                                           info->loop,
                                           info->conns[i].fd,
                                           on_first_line,
                                           NULL,
                                           NULL,
                                           on_chunk,
                                           NULL,
                                           on_complete,
                                           on_close,
                                           (void *)(info->conns + i));
                    state->results[c].code = -2;
                    state->results[c].content_length = 0;
                    state->results[c].millis = 0;
                    evhttp_connection_send(&info->conns[i].http_conn, state->request);
                    info->conns[i].running = 1;
                    info->conns[i].results = state->results + c;
    done ++;
                }
            }
        }

        if (c >= state->args.number)
            break;

        ev_loop(info->loop, EVLOOP_ONESHOT);
    }

    for (;;)
    {
        int still_running = 0;
        for (i=0; i<state->args.concurrent; ++i)
        {
            if (info->conns[i].running)
            {
                still_running = 1;
                break;
            }
        }

        if (!still_running)
            break;

        ev_loop(info->loop, EVLOOP_ONESHOT);
    }
}

int main(int argc, char * const argv[])
{
    int option_index = 0;
    int c, i, j;
    int use_deflate = 0;

    state_t state;

    state.args.number = 1;
    state.args.concurrent = 1;
    state.args.threads = 1;

    state.next_result_index = 0;

    for (;;)
    {
        static struct option long_options[] = { {0, 0, 0, 0} };

        c = getopt_long(argc, argv, "n:c:t:z",
                        long_options, &option_index);

        if (c == -1)
            break;

        switch (c)
        {
        case 'n':
            state.args.number = atoi(optarg);
            break;
        case 'c':
            state.args.concurrent = atoi(optarg);
            break;
        case 't':
            state.args.threads = atoi(optarg);
            break;
        case 'z':
            use_deflate = 1;
            break;
        default:
            usage();
            return 1;
        }
    }

    if (argc - optind != 1)
    {
        usage();
        return 1;
    }

    // parse the URL
    state.args.url = argv[optind];

    int group_info[8*3/2];
    const char *error;
    int error_offset;
    pcre *re = pcre_compile("^http://([a-zA-Z0-9.-]+)(?::(\\d+))?(/\\S*)$", 0, &error, &error_offset, NULL);
    int rc =  pcre_exec(re, NULL, state.args.url, strlen(state.args.url), 0, 0, group_info, 8*3/2);
    if (rc < 0)
    {
        printf("Error: %s is not a valid URL\n", state.args.url);
        return 1;
    }
    pcre_free(re);

    char *host_header;
    char *host = strndup(state.args.url + group_info[2], group_info[3] - group_info[2]);
    char *path = strndup(state.args.url + group_info[6], group_info[7] - group_info[6]);
    int port = 80;
    if (group_info[4] != -1)
    {
        char *tmp = strndup(state.args.url + group_info[4], group_info[5] - group_info[4]);
        port = atoi(tmp);
        free(tmp);

        host_header = strndup(state.args.url + group_info[2], group_info[5] - group_info[2]);
    }
    else
        host_header = strndup(state.args.url + group_info[2], group_info[3] - group_info[2]);

    // host lookup
    struct hostent *server = gethostbyname(host);
    if (server == NULL) {
        fprintf(stderr, "ERROR, no such host: %s\n", host);
        exit(1);
    }
    free(host);

    bzero((char *)&state.serv_addr, sizeof(state.serv_addr));
    state.serv_addr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, 
          (char *)&state.serv_addr.sin_addr.s_addr,
          server->h_length);
    state.serv_addr.sin_port = htons(port);

    // build the request
    char request[4096];
    int printed = snprintf(request,
                           4096,
                           "GET %s HTTP/1.0\r\n"
                           "Host: %s\r\n"
                           "User-Agent: benchmark\r\n"
                           "%s"
                           "Accept: */*\r\n\r\n",
                           path,
                           host_header,
                           use_deflate ? "accept-encoding: gzip,deflate\r\n" : ""
                           );
    if (printed < 1 || printed >= 4096)
    {
        fprintf(stderr, "error formatting request");
        exit(1);
    }
    free(host_header);
    free(path);

    state.request.data = request;
    state.request.length = printed;

    // init results
    state.results = malloc(sizeof(result_t) * state.args.number);

    // init all worker info
    worker_info_t *worker_infos = malloc(sizeof(worker_info_t) * state.args.threads);

    for (i=0; i<state.args.threads; ++i)
    {
        worker_infos[i].state = &state;
        worker_infos[i].loop = ev_loop_new(0);
        worker_infos[i].conns = malloc(sizeof(connection_t) * state.args.concurrent);
        for (j=0; j<state.args.concurrent; ++j)
        {
            connection_t *conn = worker_infos[i].conns + j;

            conn->fd = -1;
            conn->running = 0;
            conn->started = 0;
            conn->results = NULL;
        }
    }

    // run
    pthread_t threads[state.args.threads];

    printf("Sending %i request(s) to  %s.\n%i thread(s)\n%i concurrent connection(s) per thread\n\n", state.args.number, state.args.url, state.args.threads, state.args.concurrent);
    long started = now();

    for (i=1; i<state.args.threads; ++i)
    {
        pthread_create(threads + i, NULL, (void *(*)(void *))worker, worker_infos + i);
    }

    worker(worker_infos);

    for (i=1; i<state.args.threads; ++i)
    {
        pthread_join(threads[i], 0);
    }

    long millis = now() - started;

    // clean up
    for (i=0; i<state.args.threads; ++i)
    {
        free(worker_infos[i].conns);
        ev_loop_destroy(worker_infos[i].loop);
    }
    free(worker_infos);

    // report results
    printf("%i requests in %li millis\n", state.args.number, millis);

    int conn_failed = 0;
    int conn_ok = 0;
    int conn_other = 0;
    int content_length = 0;
    for (i=0; i<state.args.number; ++i)
    {
        switch (state.results[i].code)
        {
        case -1:
            ++conn_failed;
            break;
        case 200:
            ++conn_ok;
            content_length += state.results[i].content_length;
            break;
        default:
            ++conn_other;
            printf("?? %i\n", state.results[i].code);
            break;
        }
    }
    printf("%i OK. %i failed. %i other\n", conn_ok, conn_failed, conn_other);
    if (conn_ok)
        printf("%g average bytes per page\n", (double)content_length / (double)conn_ok);

    printf("%g rps\n", 1000.0 * ((double)state.args.number) / ((double)millis));
    free(state.results);

    return 0;
}

