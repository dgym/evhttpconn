#include <ev.h>

// Callback singatures

typedef struct
{
    const char *data;
    int length;
} evhttp_string_t;

typedef void (*evhttp_connection_on_first_line)(evhttp_string_t first, evhttp_string_t second, evhttp_string_t third, void *data);
typedef void (*evhttp_connection_on_header)(evhttp_string_t key, evhttp_string_t value, void *data);
typedef void (*evhttp_connection_on_headers_end)(evhttp_string_t message, void *data);
typedef void (*evhttp_connection_on_content)(evhttp_string_t content, void *data);
typedef void (*evhttp_connection_on_complete)(void *data);
typedef void (*evhttp_connection_on_close)(void *data);

typedef struct evhttp_connection evhttp_connection_t;

void evhttp_connection_init(evhttp_connection_t *self,
                            struct ev_loop *loop,
                            int fd,
                            evhttp_connection_on_first_line on_first_line,
                            evhttp_connection_on_header on_header,
                            evhttp_connection_on_headers_end on_headers_end,
                            evhttp_connection_on_content on_chunk,
                            evhttp_connection_on_content on_complete_content,
                            evhttp_connection_on_complete on_complete,
                            evhttp_connection_on_close on_close,
                            void *callback_data);
void evhttp_connection_close(evhttp_connection_t *self);

void evhttp_connection_send(evhttp_connection_t *self, evhttp_string_t data);
//char *evhttp_connection_make_send_buffer(evhttp_connection_t *self, int length);
void evhttp_connection_terminate(evhttp_connection_t *self);

// Internal structs, defined so evhttp_connection_t can be put on the stack

typedef struct
{
    char *data;
    int start;
    int size;
    int allocated;
} evhttp_buffer_t;

struct evhttp_connection
{
    struct ev_loop *loop;
    int fd;
    evhttp_buffer_t read_buffer;
    evhttp_buffer_t write_buffer;
    struct ev_io read_watcher;
    struct ev_io write_watcher;

    int state;
    int content_length;
    int tmp[4];
    int terminating;

    evhttp_connection_on_first_line on_first_line;
    evhttp_connection_on_header on_header;
    evhttp_connection_on_headers_end on_headers_end;
    evhttp_connection_on_content on_chunk;
    evhttp_connection_on_content on_complete_content;
    evhttp_connection_on_complete on_complete;
    evhttp_connection_on_close on_close;
    void *callback_data;
};
